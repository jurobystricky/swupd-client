/*
 *   Software Updater - client side
 *
 *      Copyright © 2012-2016 Intel Corporation.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, version 2 or later of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Authors:
 *         Arjan van de Ven <arjan@linux.intel.com>
 *         Tim Pepper <timothy.c.pepper@linux.intel.com>
 *
 */

/*
 * The curl library is great, but it is a little bit of a pain to get it to
 * reuse connections properly for simple cases. This file will manage our
 * curl handle properly so that we have a standing chance to get reuse
 * of our connections.
 *
 * NOTE NOTE NOTE
 *
 * Only use these from the main thread of the program. For multithreaded
 * use, you need to manage your own curl multi environment.
 */

#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "swupd.h"
#include "swupd_curl_internal.h"

#define SWUPD_CURL_LOW_SPEED_LIMIT 1
#define SWUPD_CURL_CONNECT_TIMEOUT 30
#define SWUPD_CURL_RCV_TIMEOUT 120

static CURL *curl = NULL;

uint64_t total_curl_sz = 0;

/* alternative CA Path */
static char *capath = NULL;

/* values for retry strategies */
enum retry_strategy {
	DONT_RETRY = 0,
	RETRY_NOW,
	RETRY_WITH_DELAY
};

/* Pretty print curl return status */
static void swupd_curl_strerror(CURLcode curl_ret)
{
	error("Curl - Download error - (%d) %s\n", curl_ret, curl_easy_strerror(curl_ret));
}

/*
 * Cannot avoid a TOCTOU here with the current curl API.  Only using the curl
 * API, a curl_easy_setopt does not detect if the client SSL certificate is
 * present on the filesystem.  This only happens during curl_easy_perform.
 * The emphasis is rather on how using an SSL client certificate is an opt-in
 * function rather than an opt-out function.
 */
CURLcode swupd_curl_set_optional_client_cert(CURL *curl)
{
	CURLcode curl_ret = CURLE_OK;
	char *client_cert_path;

	client_cert_path = mk_full_filename(path_prefix, SSL_CLIENT_CERT);
	if (access(client_cert_path, F_OK) == 0) {
		curl_ret = curl_easy_setopt(curl, CURLOPT_SSLCERT, client_cert_path);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
		curl_ret = curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	}

exit:
	free_string(&client_cert_path);
	return curl_ret;
}

static CURLcode swupd_curl_set_timeouts(CURL *curl)
{
	CURLcode curl_ret;

	curl_ret = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, SWUPD_CURL_CONNECT_TIMEOUT);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, SWUPD_CURL_LOW_SPEED_LIMIT);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, SWUPD_CURL_RCV_TIMEOUT);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

exit:
	return curl_ret;
}

static int check_connection(const char *test_capath)
{
	CURLcode curl_ret;
	long response = 0;

	debug("Curl - check_connection url: %s\n", version_url);
	curl_ret = swupd_curl_set_basic_options(curl, version_url, false);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	if (test_capath) {
		curl_ret = curl_easy_setopt(curl, CURLOPT_CAPATH, test_capath);
		if (curl_ret != CURLE_OK) {
			return -1;
		}
	}

	curl_ret = curl_easy_perform(curl);

	switch (curl_ret) {
	case CURLE_OK:
		return 0;
	case CURLE_SSL_CACERT:
		debug("Curl - Unable to verify server SSL certificate\n");
		return -SWUPD_BAD_CERT;
	case CURLE_SSL_CERTPROBLEM:
		debug("Curl - Problem with the local client SSL certificate\n");
		return -SWUPD_BAD_CERT;
	case CURLE_OPERATION_TIMEDOUT:
		debug("Curl - Timed out\n");
		return -CURLE_OPERATION_TIMEDOUT;
	case CURLE_HTTP_RETURNED_ERROR:
		if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response) != CURLE_OK) {
			response = 0;
		}
		debug("Curl - process_curl_error_codes: curl_ret = %d, response = %d\n", curl_ret, response);
		return -1;
	default:
		debug("Curl - Download error - (%d) %s\n", curl_ret,
		      curl_easy_strerror(curl_ret));
		return -1;
	}
}

int swupd_curl_init(void)
{
	CURLcode curl_ret;
	char *str;
	char *tok;
	char *ctx = NULL;
	int ret;
	struct stat st;

	if (curl) {
		warn("Curl has already been initialized\n");
		return 0;
	}

	curl_ret = curl_global_init(CURL_GLOBAL_ALL);
	if (curl_ret != CURLE_OK) {
		error("Curl - Failed to initialize environment\n");
		return -1;
	}

	curl = curl_easy_init();
	if (curl == NULL) {
		error("Curl - Failed to initialize session\n");
		curl_global_cleanup();
		return -1;
	}

	ret = check_connection(NULL);
	if (ret == 0) {
		return 0;
	} else if (ret == -CURLE_OPERATION_TIMEDOUT) {
		error("Curl - Communicating with server timed out\n");
		return -1;
	}

	if (FALLBACK_CAPATHS[0]) {
		str = strdup_or_die(FALLBACK_CAPATHS);
		for (tok = strtok_r(str, ":", &ctx); tok; tok = strtok_r(NULL, ":", &ctx)) {
			if (stat(tok, &st)) {
				continue;
			}
			if ((st.st_mode & S_IFMT) != S_IFDIR) {
				continue;
			}

			debug("Curl - Trying fallback CA path %s\n", tok);
			ret = check_connection(tok);
			if (ret == 0) {
				capath = strdup_or_die(tok);
				break;
			}
		}
		free_string(&str);
	}

	if (ret != 0) {
		error("Failed to connect to update server: %s\n", version_url);
		info("Possible solutions for this problem are:\n"
		     "\tCheck if your network connection is working\n"
		     "\tFix the system clock\n"
		     "\tRun 'swupd info' to check if the urls are correct\n"
		     "\tCheck if the server SSL certificate is trusted by your system ('clrtrust generate' may help)\n");
	}

	return ret;
}

void swupd_curl_deinit(void)
{
	if (!curl) {
		return;
	}

	curl_easy_cleanup(curl);
	curl = NULL;
	free_string(&capath);
	curl_global_cleanup();
}

static size_t dummy_write_cb(void UNUSED_PARAM *func, size_t size, size_t nmemb, void UNUSED_PARAM *data)
{
	/* Drop the content */
	return (size_t)(size * nmemb);
}

double swupd_curl_query_content_size(char *url)
{
	CURLcode curl_ret;
	double content_size;

	if (!curl) {
		error("Curl hasn't been initialized\n");
		return -1;
	}

	curl_easy_reset(curl);

	/* Set buffer for error string */
	curl_ret = curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, dummy_write_cb);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dummy_write_cb);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_URL, url);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	if (capath) {
		curl_ret = curl_easy_setopt(curl, CURLOPT_CAPATH, capath);
		if (curl_ret != CURLE_OK) {
			return -1;
		}
	}

	curl_ret = swupd_curl_set_optional_client_cert(curl);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_perform(curl);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	curl_ret = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_size);
	if (curl_ret != CURLE_OK) {
		return -1;
	}

	return content_size;
}

static size_t swupd_download_file_to_memory(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct curl_file_data *file_data = (struct curl_file_data *)userdata;
	size_t data_len = size * nmemb;

	if (data_len + file_data->len > file_data->capacity) {
		return 0;
	}

	memcpy(file_data->data + file_data->len, ptr, data_len);
	file_data->len += data_len;

	return data_len;
}

CURLcode swupd_download_file_create(struct curl_file *file)
{
	file->fh = fopen(file->path, "w");
	if (!file->fh) {
		error("Curl - Cannot open file for write \\*outfile=\"%s\",strerror=\"%s\"*\\\n",
		      file->path, strerror(errno));
		return CURLE_WRITE_ERROR;
	}
	return CURLE_OK;
}

CURLcode swupd_download_file_append(struct curl_file *file)
{
	file->fh = fopen(file->path, "a");
	if (!file->fh) {
		error("Curl - Cannot open file for append \\*outfile=\"%s\",strerror=\"%s\"*\\\n",
		      file->path, strerror(errno));
		return CURLE_WRITE_ERROR;
	}
	return CURLE_OK;
}

CURLcode swupd_download_file_close(CURLcode curl_ret, struct curl_file *file)
{
	if (file->fh) {
		if (fclose(file->fh)) {
			error("Curl - Cannot close file after write \\*outfile=\"%s\",strerror=\"%s\"*\\\n",
			      file->path, strerror(errno));
			if (curl_ret == CURLE_OK) {
				curl_ret = CURLE_WRITE_ERROR;
			}
		}
		file->fh = NULL;
	}
	return curl_ret;
}

enum download_status process_curl_error_codes(int curl_ret, CURL *curl_handle)
{
	char *url;
	if (curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK) {
		url = "<not available>";
	}

	/*
	 * retrieve bytes transferred, errors or not
	 */
	curl_off_t curl_sz = 0;
	if (curl_easy_getinfo(curl_handle, CURLINFO_SIZE_DOWNLOAD_T, &curl_sz) == CURLE_OK) {
		total_curl_sz += curl_sz;
	}

	if (curl_ret == CURLE_OK || curl_ret == CURLE_HTTP_RETURNED_ERROR ||
	    curl_ret == CURLE_RECV_ERROR) {
		long response = 0;
		if (curl_ret == CURLE_OK || curl_ret == CURLE_HTTP_RETURNED_ERROR) {
			if (curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response) != CURLE_OK) {
				response = -1; // Force error
			}
		}
		debug("Curl - process_curl_error_codes: curl_ret = %d, response = %d\n", curl_ret, response);
		/* curl command succeeded, download might've failed, let our caller handle */
		switch (response) {
		case 206:
			error("Curl - Partial file downloaded from '%s'\n", url);
			return DOWNLOAD_STATUS_PARTIAL_FILE;
		case 200:
		case 0:
			return DOWNLOAD_STATUS_COMPLETED;
		case 403:
			debug("Curl - Download failed - forbidden (403) - '%s'\n", url);
			return DOWNLOAD_STATUS_FORBIDDEN;
		case 404:
			debug("Curl - Download failed - file not found (404) - '%s'\n", url);
			return DOWNLOAD_STATUS_NOT_FOUND;
		default:
			error("Curl - Download failed: response (%ld) -  '%s'\n", response, url);
			return DOWNLOAD_STATUS_ERROR;
		}
	} else { /* download failed but let our caller do it */
		debug("Curl - process_curl_error_codes - curl_ret = %d\n", curl_ret);
		switch (curl_ret) {
		case CURLE_COULDNT_RESOLVE_PROXY:
			error("Curl - Could not resolve proxy\n");
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_COULDNT_RESOLVE_HOST:
			error("Curl - Could not resolve host - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_COULDNT_CONNECT:
			error("Curl - Could not connect to host or proxy - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_FILE_COULDNT_READ_FILE:
			return DOWNLOAD_STATUS_NOT_FOUND;
		case CURLE_PARTIAL_FILE:
			error("Curl - File incompletely downloaded - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_RECV_ERROR:
			error("Curl - Failure receiving data from server - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_WRITE_ERROR:
			error("Curl - Error downloading to local file - '%s'\n", url);
			error("Curl - Check free space for %s?\n", state_dir);
			return DOWNLOAD_STATUS_WRITE_ERROR;
		case CURLE_OPERATION_TIMEDOUT:
			error("Curl - Communicating with server timed out - '%s'\n", url);
			return DOWNLOAD_STATUS_TIMEOUT;
		case CURLE_SSL_CACERT_BADFILE:
			error("Curl - Bad SSL Cert file, cannot ensure secure connection - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_SSL_CERTPROBLEM:
			error("Curl - Problem with the local client SSL certificate - '%s'\n", url);
			return DOWNLOAD_STATUS_ERROR;
		case CURLE_RANGE_ERROR:
			error("Curl - Range command not supported by server, download resume disabled - '%s'\n", url);
			return DOWNLOAD_STATUS_RANGE_ERROR;
		default:
			swupd_curl_strerror(curl_ret);
		}
	}
	return DOWNLOAD_STATUS_ERROR;
}

/*
 * Download a single file SYNCHRONOUSLY
 *
 * - If in_memory_file != NULL the file will be stored in memory and not on disk.
 * - If resume_ok == true and resume is supported, the function will resume an
 *   interrupted download if necessary.
 * - If failure to download, partial download is not deleted.
 *
 * Returns: Zero (DOWNLOAD_STATUS_COMPLETED) on success or a status code > 0 on errors.
 *
 * NOTE: See full_download() for multi/asynchronous downloading of fullfiles.
 */
static enum download_status swupd_curl_get_file_full(const char *url, char *filename,
						     struct curl_file_data *in_memory_file, bool resume_ok)
{
	static bool resume_download_supported = true;

	CURLcode curl_ret;
	enum download_status status;
	struct curl_file local = { 0 };

restart_download:
	curl_easy_reset(curl);

	if (!in_memory_file) {
		// normal file download
		struct stat stat;

		local.path = filename;

		if (resume_ok && resume_download_supported && lstat(filename, &stat) == 0) {
			info("Curl - Resuming download for '%s'\n", url);
			curl_ret = curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)stat.st_size);
			if (curl_ret != CURLE_OK) {
				goto exit;
			}

			curl_ret = swupd_download_file_append(&local);
		} else {
			curl_ret = swupd_download_file_create(&local);
		}

		curl_ret = curl_easy_setopt(curl, CURLOPT_PRIVATE, (void *)&local);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
		curl_ret = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)local.fh);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	} else {
		curl_ret = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, swupd_download_file_to_memory);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
		curl_ret = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)in_memory_file);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
		curl_ret = curl_easy_setopt(curl, CURLOPT_COOKIE, "request=uncached");
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	}

	curl_ret = swupd_curl_set_basic_options(curl, url, true);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	debug("Curl - Start sync download: %s -> %s\n", url, in_memory_file ? "<memory>" : filename);
	curl_ret = curl_easy_perform(curl);

exit:
	if (!in_memory_file) {
		curl_ret = swupd_download_file_close(curl_ret, &local);
	}

	status = process_curl_error_codes(curl_ret, curl);
	debug("Curl - Complete sync download: %s -> %s, status=%d\n", url, in_memory_file ? "<memory>" : filename, status);
	if (status == DOWNLOAD_STATUS_RANGE_ERROR) {
		// Reset variable
		memset(&local, 0, sizeof(local));

		//Disable download resume
		resume_download_supported = false;
		goto restart_download;
	}
	if (status != DOWNLOAD_STATUS_COMPLETED && !resume_ok) {
		unlink(filename);
	}

	return status;
}

/*
 * Determines what strategy to use based on the return code:
 * - do not retry: if the fault indicates that the failure isn't transient or
 *                 is unlikely to be successful if repeated (i.e. disk full)
 * - retry immediately: if the specific fault reported is unusual or rare, it
 *                      might have been caused by unusual circumstances, the
 *                      same failure is unlikely to be repeated (e.g. corrupt
 *                      network package)
 * - retry after a delay: if the fault is caused by a transient fault (like
 *                        network connectivity)
 */
static enum retry_strategy determine_strategy(int status)
{
	/* we don't need to retry if the content URL is local */
	if (content_url_is_local) {
		return DONT_RETRY;
	}

	switch (status) {
	case DOWNLOAD_STATUS_FORBIDDEN:
	case DOWNLOAD_STATUS_NOT_FOUND:
	case DOWNLOAD_STATUS_WRITE_ERROR:
		return DONT_RETRY;
	case DOWNLOAD_STATUS_RANGE_ERROR:
	case DOWNLOAD_STATUS_PARTIAL_FILE:
		return RETRY_NOW;
	case DOWNLOAD_STATUS_ERROR:
	case DOWNLOAD_STATUS_TIMEOUT:
		return RETRY_WITH_DELAY;
	default:
		return RETRY_NOW;
	}
}

static int retry_download_loop(const char *url, char *filename, struct curl_file_data *in_memory_file, bool resume_ok)
{

	int current_retry = 0;
	int sleep_time = retry_delay;
	int strategy;
	int ret;

	if (!curl) {
		error("Curl hasn't been initialized\n");
		return -1;
	}

	for (;;) {

		/* download file */
		ret = swupd_curl_get_file_full(url, filename, in_memory_file, resume_ok);

		if (ret == DOWNLOAD_STATUS_COMPLETED) {
			/* operation successful */
			break;
		}

		/* operation failed, determine retry strategy */
		current_retry++;
		strategy = determine_strategy(ret);
		if (strategy == DONT_RETRY) {
			/* don't retry just return a failure */
			return -EIO;
		}

		/* if we got to this point and we haven't reached the retry limit,
		 * we need to retry, otherwise just return the failure */
		if (strategy == RETRY_NOW) {
			sleep_time = 0;
		}
		if (max_retries) {
			if (current_retry <= max_retries) {
				if (sleep_time) {
					info("Waiting %d seconds before retrying the download\n", sleep_time);
				}
				sleep(sleep_time);
				sleep_time = (sleep_time * DELAY_MULTIPLIER) > MAX_DELAY ? MAX_DELAY : (sleep_time * DELAY_MULTIPLIER);
				info("Retry #%d downloading from %s\n", current_retry, url);
				continue;
			} else {
				warn("Maximum number of retries reached\n");
			}
		} else {
			info("Download retries is disabled\n");
		}
		/* we ran out of retries, return an error */
		return -ECOMM;
	}

	return ret;
}

/*
 * Download a single file SYNCHRONOUSLY
 *
 * Returns: Zero on success or a standard < 0 status code on errors.
 */
int swupd_curl_get_file(const char *url, char *filename)
{
	return retry_download_loop(url, filename, NULL, false);
}

/*
 * Download a single file SYNCHRONOUSLY to a memory struct
 *
 * Returns: Zero on success or a standard < 0 status code on errors.
 */
int swupd_curl_get_file_memory(const char *url, struct curl_file_data *file_data)
{
	return retry_download_loop(url, NULL, file_data, false);
}

static CURLcode swupd_curl_set_security_opts(CURL *curl)
{
	CURLcode curl_ret = CURLE_OK;

	curl_ret = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "HIGH");
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	if (capath) {
		curl_ret = curl_easy_setopt(curl, CURLOPT_CAPATH, capath);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	}

	curl_ret = swupd_curl_set_optional_client_cert(curl);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

exit:
	return curl_ret;
}

CURLcode swupd_curl_set_basic_options(CURL *curl, const char *url, bool fail_on_error)
{
	static bool use_ssl = true;

	CURLcode curl_ret = CURLE_OK;

	curl_ret = curl_easy_setopt(curl, CURLOPT_URL, url);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
	if (curl_ret != CURLE_OK && curl_ret != CURLE_UNSUPPORTED_PROTOCOL) {
		goto exit;
	}

	curl_ret = curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1);
	if (curl_ret != CURLE_OK && curl_ret != CURLE_UNKNOWN_OPTION) {
		goto exit;
	}

	curl_easy_setopt(curl, CURLOPT_USERAGENT, PACKAGE "/" VERSION);
	// No error checking needed, this is not critical information

	if (update_server_port > 0) {
		curl_ret = curl_easy_setopt(curl, CURLOPT_PORT, update_server_port);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	}

	if (strncmp(url, "https://", 8) == 0) {
		//TODO: Fix "SECURITY HOLE since we can't SSL pin arbitrary servers"
		curl_ret = swupd_curl_set_security_opts(curl);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	} else {
		if (use_ssl) {
			use_ssl = false;
		}
	}

	curl_ret = swupd_curl_set_timeouts(curl);
	if (curl_ret != CURLE_OK) {
		goto exit;
	}

	if (fail_on_error) {
		/* Avoid downloading HTML files for error responses if the HTTP code is >= 400 */
		curl_ret = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
		if (curl_ret != CURLE_OK) {
			goto exit;
		}
	}

exit:
	return curl_ret;
}
