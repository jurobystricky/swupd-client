/*
 *   Software Updater - client side
 *
 *      Copyright © 2018 Intel Corporation.
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
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "swupd.h"

static void print_help(void)
{
	print(
	    "Usage:\n"
	    "   swupd clean [OPTION...]\n\n"
	    "\n"
	    "Remove cached content used for updates from state directory.\n"
	    "\n"
	    "Options:\n"
	    "   --all                   Remove all the content including recent metadata\n"
	    "   --dry-run               Just print files that would be removed\n"
	    "\n");
	global_print_help();
}

static struct {
	int all;
	int dry_run;
} options;

static struct {
	int files_removed;
} stats;

static struct timespec now;

static const struct option prog_opts[] = {
	{ "help", no_argument, 0, 'h' },
	{ "all", no_argument, &options.all, 1 },
	{ "dry-run", no_argument, &options.dry_run, 1 },
};

static const struct global_options opts = {
	prog_opts,
	sizeof(prog_opts) / sizeof(struct option),
	NULL,
	print_help,
};

static bool parse_options(int argc, char **argv)
{
	int optind = global_parse_options(argc, argv, &opts);

	if (optind < 0) {
		return false;
	}

	if (argc > optind) {
		error("unexpected arguments\n\n");
		return false;
	}

	return true;
}

typedef bool(remove_predicate_func)(const char *dir, const struct dirent *entry);

/* Remove files from path for which pred returns true.
 * Currently it doesn't recursively remove directories.
 */
static enum swupd_code remove_if(const char *path, bool dry_run, remove_predicate_func pred)
{
	int ret = SWUPD_OK;
	DIR *dir;

	dir = opendir(path);
	if (!dir) {
		return SWUPD_COULDNT_LIST_DIR;
	}

	char *file = NULL;

	while (true) {
		free_string(&file);
		ret = SWUPD_OK;

		/* Reset errno to distinguish between a previous
		 * failure and the end of stream. */
		errno = 0;
		struct dirent *entry;
		entry = readdir(dir);
		if (!entry) {
			if (errno) {
				ret = SWUPD_COULDNT_LIST_DIR;
			}
			break;
		}

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}

		string_or_die(&file, "%s/%s", path, entry->d_name);

		struct stat stat;
		ret = lstat(file, &stat);
		if (ret != 0) {
			warn("couldn't access %s: %s\n", file, strerror(errno));
			continue;
		}

		if (!pred(path, entry)) {
			continue;
		}

		if (dry_run) {
			info("%s\n", file);
		} else {
			if (S_ISDIR(stat.st_mode)) {
				ret = rmdir(file);
			} else {
				ret = unlink(file);
			}
			if (ret != 0) {
				warn("couldn't remove file %s: %s\n", file, strerror(errno));
			}
		}
		if (ret == 0) {
			stats.files_removed++;
		}
	}

	closedir(dir);
	return ret;
}

static bool is_fullfile(const char UNUSED_PARAM *dir, const struct dirent *entry)
{
	return strlen(entry->d_name) == (SWUPD_HASH_LEN - 1);
}

static bool is_pack_indicator(const char UNUSED_PARAM *dir, const struct dirent *entry)
{
	static const char prefix[] = "pack-";
	static const char suffix[] = ".tar";
	static const size_t prefix_len = sizeof(prefix) - 1;
	static const size_t suffix_len = sizeof(suffix) - 1;

	const char *name = entry->d_name;
	size_t len = strlen(name);
	if (len < (prefix_len + suffix_len)) {
		return false;
	}
	return !memcmp(name, prefix, prefix_len) && !memcmp(name + len - suffix_len, suffix, suffix_len);
}

static bool is_all_digits(const char *s)
{
	for (; *s; s++) {
		if (!isdigit(*s)) {
			return false;
		}
	}
	return true;
}

static bool is_all_xdigits(const char *s)
{
	for (; *s; s++) {
		if (!isxdigit(*s)) {
			return false;
		}
	}
	return true;
}

static bool is_manifest(const char UNUSED_PARAM *dir, const struct dirent *entry)
{
	static const char prefix[] = "Manifest.";
	static const size_t prefix_len = sizeof(prefix) - 1;
	return !strncmp(entry->d_name, prefix, prefix_len);
}

static bool is_hashed_manifest(const char UNUSED_PARAM *dir, const struct dirent *entry)
{
	/* check that this has the manifest prefix */
	if (!is_manifest(dir, entry)) {
		return false;
	}

	int i;
	int start = sizeof("Manifest.");
	const char *ename = entry->d_name + start;

	/* check for the correct number of '.' characters (i.e.
	 * Manifest.bundlename.hashvalue). Note, this function returns false for
	 * iterative manifests (Manifest.bundlename.I.version) and delta manifests
	 * (Manifest.bundlename.D.version) because of the number of '.' characters
	 * in the file name. Expect 1 '.' because we are starting after the prefix
	 * 'Manifest.'
	 */
	for (i = 0; ename[i]; ename[i] == '.' ? i++ : *ename++) {
		/* this for loop is counting the occurrances of '.' by incrementing a
		 * counter (i) when the character is encountered. The 'i' variable is
		 * used as an index into the string as well. The pointer to the string
		 * is advanced when a '.' is not encountered, so the index into the
		 * original string is num_dots + num_not_dots.
		 */
	}
	if (i != 1) {
		return false;
	}

	/* grab hash suffix by splitting on last '.' then iterating past it (+ 1) */
	return is_all_xdigits(strrchr(entry->d_name + start, '.') + 1);
}

static bool is_manifest_delta(const char UNUSED_PARAM *dir, const struct dirent *entry)
{
	static const char prefix[] = "Manifest-";
	static const size_t prefix_len = sizeof(prefix) - 1;
	return !strncmp(entry->d_name, prefix, prefix_len);
}

static char *read_mom_contents(int version)
{
	char *mom_path = NULL;
	string_or_die(&mom_path, "%s/%d/Manifest.MoM", state_dir, version);
	FILE *f = fopen(mom_path, "r");
	free_string(&mom_path);
	if (!f) {
		/* This is a best effort. */
		return NULL;
	}

	char *contents = NULL;
	int fd = fileno(f);
	if (fd == -1) {
		goto end;
	}

	int ret;
	struct stat stat;
	ret = fstat(fd, &stat);
	if (ret != 0) {
		goto end;
	}

	contents = malloc(stat.st_size + 1);
	ON_NULL_ABORT(contents);

	ret = fread(contents, stat.st_size, 1, f);
	if (ret != 1) {
		free(contents);
		contents = NULL;
	} else {
		contents[stat.st_size] = 0;
	}

end:
	fclose(f);
	return contents;
}

static enum swupd_code clean_staged_manifests(const char *path, bool dry_run, bool all)
{
	DIR *dir;

	dir = opendir(path);
	if (!dir) {
		return SWUPD_COULDNT_LIST_DIR;
	}

	/* NOTE: Currently Manifest files have their timestamp from generation
	 * preserved. */

	/* When --all is not used, keep the Manifests used by the current OS version. This
	 * ensures that a regular 'clean' won't make 'search' redownload files. */
	char *mom_contents = NULL;
	if (!all) {
		int current_version = get_current_version(path_prefix);
		if (current_version < 0) {
			warn("Unable to determine current OS version\n");
		} else {
			mom_contents = read_mom_contents(current_version);
		}
	}

	int ret = SWUPD_OK;
	while (true) {
		/* Reset errno to properly identify the end of stream. */
		errno = 0;
		struct dirent *entry;
		entry = readdir(dir);
		if (!entry) {
			if (errno) {
				ret = SWUPD_COULDNT_LIST_DIR;
			}
			break;
		}

		const char *name = entry->d_name;
		if (!strcmp(name, ".") || !strcmp(name, "..")) {
			continue;
		}
		if (!is_all_digits(name)) {
			continue;
		}

		char *version_dir;
		string_or_die(&version_dir, "%s/%s", state_dir, name);

		/* This is not precise: it may keep Manifest files that we don't use, and
		 * also will keep the previous version. If that extra precision is
		 * required we should parse the manifest. */
		if (mom_contents && strstr(mom_contents, name)) {
			/* Remove all hash-hint manifest files. */
			ret = remove_if(version_dir, dry_run, is_hashed_manifest);
		} else {
			/* Remove all manifest files, including hash-hints */
			ret = remove_if(version_dir, dry_run, is_manifest);
		}

		/* Remove empty dirs if possible. */
		(void)rmdir(version_dir);

		free_string(&version_dir);
		if (ret != 0) {
			break;
		}
	}

	free(mom_contents);
	closedir(dir);
	return ret;
}

enum swupd_code clean_main(int argc, char **argv)
{
	enum swupd_code ret = SWUPD_OK;
	const int steps_in_clean = 1;

	if (!parse_options(argc, argv)) {
		print_help();
		return SWUPD_INVALID_OPTION;
	}
	progress_init_steps("clean", steps_in_clean);

	ret = swupd_init(SWUPD_NO_NETWORK);
	if (ret != 0) {
		error("Failed swupd initialization, exiting now.\n");
		goto exit;
	}

	if (!options.all) {
		ret = clock_gettime(CLOCK_REALTIME, &now);
		if (ret != 0) {
			ret = SWUPD_TIME_UNKNOWN;
			error("couldn't read current time to decide what files to clean");
			goto end;
		}
	}

	/* NOTE: Delete specific file patterns to avoid disasters in case some paths are
	 * set incorrectly. */

	/* Staged files. */

	/* TODO: Consider having a mode for clean that parses the current manifest (if available)
	 * and keeping all the staged files of the current version. This helps recovering the
	 * current version. Or do it for the previous version to allow a rollback. */
	ret = clean_statedir(options.dry_run, options.all);
	/* TODO: Also print the bytes removed, need to take into account the hardlinks. */
	if (options.dry_run) {
		print("Would remove %d files.\n", stats.files_removed);
	} else {
		print("%d files removed.\n", stats.files_removed);
	}

end:
	swupd_deinit();

exit:
	progress_finish_steps("clean", ret);
	return ret;
}

/* clean_statedir will clean the state directory used by swupd (default to
 * /var/lib/swupd). It will remove all files except relevant manifests unless
 * all is set to true. Setting dry_run to true will print the files that would
 * be removed but will not actually remove them. */
enum swupd_code clean_statedir(bool dry_run, bool all)
{

	char *staged_dir = NULL;
	string_or_die(&staged_dir, "%s/staged", state_dir);
	int ret = remove_if(staged_dir, dry_run, is_fullfile);
	free_string(&staged_dir);
	if (ret != 0) {
		return ret;
	}

	/* Pack presence indicator files. */
	ret = remove_if(state_dir, dry_run, is_pack_indicator);
	if (ret != 0) {
		return ret;
	}

	/* Manifest delta files. */
	ret = remove_if(state_dir, dry_run, is_manifest_delta);
	if (ret != 0) {
		return ret;
	}

	/* NOTE: do not clean the state_dir/bundles directory */

	return clean_staged_manifests(state_dir, dry_run, all);
}
