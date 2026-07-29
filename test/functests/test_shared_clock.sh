# Exercise the optimized shared clock and the legacy timestamp-file fallback.

init()
{
	typeset testsuite="$1"
	PLATFORM=$(platform)
	if [ -z "$PLATFORM" ]; then
		echo "$testsuite: unknown platform! quitting"
		return 1
	fi
	echo "# PLATFORM=$PLATFORM"
	return 0
}

run()
{
	init
	run_testcase shared_clock_end_to_end
	run_testcase legacy_timestamp_updates
}

shared_clock_end_to_end()
{
	typeset fakelib="${FAKETIME_TESTLIB:-../src/libfaketime.so.1}"
	LD_PRELOAD="$fakelib" \
	FAKETIME_SHARED_CLOCK=1 \
	FAKETIME_SHARED_CLOCK_INITIAL_NS=946684800000000000 \
	FAKETIME_DONT_FAKE_MONOTONIC=1 \
	./shared_clock_test
}

legacy_timestamp_updates()
{
	typeset fakelib="${FAKETIME_TESTLIB:-../src/libfaketime.so.1}"
	typeset stamp="/dev/shm/libfaketime-legacy-test-$$"
	typeset before after

	printf '%s\n' '@2001-01-01 00:00:00' > "$stamp" || return 1
	before=$(LD_PRELOAD="$fakelib" FAKETIME_TIMESTAMP_FILE="$stamp" \
		FAKETIME_NO_CACHE=1 date -u +%Y) || return 1
	printf '%s\n' '@2002-01-01 00:00:00' > "$stamp" || return 1
	after=$(LD_PRELOAD="$fakelib" FAKETIME_TIMESTAMP_FILE="$stamp" \
		FAKETIME_NO_CACHE=1 date -u +%Y) || return 1
	rm -f "$stamp"
	asserteq "$before:$after" "2001:2002" \
		"legacy timestamp file should remain dynamically updateable"
}
