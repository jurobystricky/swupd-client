#!/usr/bin/env bats

load "../testlib"

test_setup() {

	create_test_environment "$TEST_NAME"
	create_bundle -n test-bundle -d /usr/bin/test "$TEST_NAME"

}

@test "ADD012: Adding a bundle containing a directory" {

	run sudo sh -c "$SWUPD bundle-add $SWUPD_OPTS test-bundle"

	assert_status_is 0
	assert_dir_exists "$TARGETDIR"/usr/bin/test
	expected_output=$(cat <<-EOM
		Loading required manifests...
		No packs need to be downloaded
		Starting download of remaining update content. This may take a while...
		Installing bundle(s) files...
		Calling post-update helper scripts.
		Successfully installed 1 bundle
	EOM
	)
	assert_is_output "$expected_output"

} 
