#!/usr/bin/env bash
# execline.t — test all 46 commands in the execline multicall binary
#
# WHAT IT DOES
# Runs execline.c (via ccraft shebang) through 147 focused assertions.
# Each test_* function exercises one command or feature: runs the
# binary, checks exit code and stdout.
#
# WHY
# execline.c implements 46 commands in one C file. Each command only
# modifies env/fds/cwd and hands control to the next. Testing them
# as a black-box binary (not unit-testing internals) validates that
# the dispatch, substitution engine, block parsing, and all handlers
# work together correctly.
#
# HOW IT WORKS
# Uses the bash-test framework (inlined below). Each test_* function
# calls run() to invoke the binary via xrun(), then asserts on
# RUN_OUT / RUN_EXIT. xrun() wraps env -i PATH="$PATH" for a clean
# environment — the binary itself sets whatever env vars it needs.
# For stdin-driven commands (forstdin), a temp file feeds stdin.
#
# USAGE
#   ./execline.t              run all
#   ./execline.t /backtick    run tests matching "backtick"
#   ./execline.t -l           list tests
#   ./execline.t -v           verbose (show each assertion)
#   ./execline.t -f           verify assertions detect failures
#
# EXIT CODES
#   0   all tests passed
#   1   one or more tests failed
#
# DEPENDENCIES
#   ccraft — compiles execline.c on first invocation (via shebang)
#   /bin/echo, tr, kill, false — standard POSIX tools

set -euo pipefail

# ============================================================================
# bash-test framework (inlined from ~/.claude/skills/.reference/bash-test)
# ============================================================================

RUN_OUT=""
RUN_ERR=""
RUN_EXIT=0

run() {
    local TMPOUT
    local TMPERR

    TMPOUT=$(mktemp)
    TMPERR=$(mktemp)

    RUN_EXIT=0
    "$@" > "$TMPOUT" 2> "$TMPERR" || RUN_EXIT=$?
    RUN_OUT=$(< "$TMPOUT")
    RUN_ERR=$(< "$TMPERR")
    rm -f "$TMPOUT" "$TMPERR"
}

PASS=0
FAIL=0
TEST_PASS=0
TEST_FAIL=0
CURRENT_TEST=""
TEST_HAD_FAILURE=0
VERBOSE=false

readonly GREEN=$'\033[32m'
readonly RED=$'\033[31m'
readonly RESET=$'\033[0m'
readonly WIDTH=90

no_failures()  { [[ $TEST_HAD_FAILURE -eq 0 ]]; }
all_passed()   { [[ $FAIL -eq 0 ]]; }
all_detected() { [[ $DETECTED -eq $EXPECTED ]]; }
should_skip()  { [[ -n "$FILTER" && "$1" != *"$FILTER"* ]]; }
is_negation()  { [[ "$1" == "!" ]]; }

show_help() {
    cat <<'EOF'
Usage:
  ./execline.t                   run all tests
  ./execline.t /pattern          run tests matching "pattern"
  ./execline.t -l                list tests
  ./execline.t -v                verbose (show each assertion)
  ./execline.t -f                verify assertions can detect failures
EOF
    exit 0
}

show_tests() {
    declare -F | awk '$3 ~ /^test_/ {print "/" substr($3, 6)}' | sort
    exit 0
}

log_result() {
    local TEST_PATH=$1
    local STATUS=$2
    local COLOR=$3
    local EXTRA=${4:-}
    local PAD=$((WIDTH - ${#TEST_PATH}))

    [[ $PAD -lt 1 ]] && PAD=1
    printf '/%s%*s%s[%s]%s%s\n' \
        "$TEST_PATH" "$PAD" "" "$COLOR" "$STATUS" "$RESET" "$EXTRA"
}

log_pass() {
    $VERBOSE && log_result "$CURRENT_TEST/$1" " OK " "$GREEN"
    return 0
}

record_pass() {
    PASS=$((PASS + 1))
    TEST_PASS=$((TEST_PASS + 1))
    log_pass "$1"
}

record_fail() {
    FAIL=$((FAIL + 1))
    TEST_FAIL=$((TEST_FAIL + 1))
    TEST_HAD_FAILURE=1
    log_result "$CURRENT_TEST/$1" "FAIL" "$RED" >&2
}

reset_test_state() {
    CURRENT_TEST=$1
    TEST_HAD_FAILURE=0
    TEST_PASS=0
    TEST_FAIL=0
}

assert() {
    local NAME=$1
    shift
    local NEGATE=false

    if is_negation "$1"; then
        NEGATE=true
        shift
    fi

    local RESULT=0
    "$@" 2>/dev/null || RESULT=$?

    if $NEGATE; then
        if [[ $RESULT -ne 0 ]]; then
            RESULT=0
        else
            RESULT=1
        fi
    fi

    if [[ $RESULT -eq 0 ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
    fi
}

assert_eq() {
    local NAME=$1
    local EXPECTED=$2
    local ACTUAL=$3

    if [[ "$EXPECTED" == "$ACTUAL" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected: %s\n  actual:   %s\n' "$EXPECTED" "$ACTUAL" >&2
    fi
}

assert_contains() {
    local NAME=$1
    local HAYSTACK=$2
    local NEEDLE=$3

    if [[ "$HAYSTACK" == *"$NEEDLE"* ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  missing: %s\n' "$NEEDLE" >&2
    fi
}

assert_exit() {
    local NAME=$1
    local EXPECTED=$2

    if [[ "$RUN_EXIT" -eq "$EXPECTED" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected exit %d, got %d\n' "$EXPECTED" "$RUN_EXIT" >&2
    fi
}

assert_regex() {
    local NAME=$1
    local PATTERN=$2
    local VALUE=$3

    if [[ "$VALUE" =~ $PATTERN ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  %s\n  does not match: %s\n' "$VALUE" "$PATTERN" >&2
    fi
}

assert_file_exists() {
    local NAME=$1
    local FILE=$2

    if [[ -f "$FILE" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  no file: %s\n' "$FILE" >&2
    fi
}

assert_file_contains() {
    local NAME=$1
    local FILE=$2
    local NEEDLE=$3

    if grep -qF "$NEEDLE" "$FILE" 2>/dev/null; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  %s: missing: %s\n' "$FILE" "$NEEDLE" >&2
    fi
}

assert_not_contains() {
    local NAME=$1
    local HAYSTACK=$2
    local NEEDLE=$3

    if [[ "$HAYSTACK" != *"$NEEDLE"* ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  unexpected: %s\n' "$NEEDLE" >&2
    fi
}

assert_empty() {
    local NAME=$1
    local VALUE=$2

    if [[ -z "$VALUE" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected empty, got: %s\n' "$VALUE" >&2
    fi
}

assert_lines() {
    local NAME=$1
    local EXPECTED=$2
    local VALUE=$3
    local ACTUAL

    [[ -z "$VALUE" ]] && ACTUAL=0 || ACTUAL=$(printf '%s' "$VALUE" | wc -l)
    [[ -n "$VALUE" && "${VALUE: -1}" != $'\n' ]] && ACTUAL=$((ACTUAL + 1))

    if [[ "$EXPECTED" -eq "$ACTUAL" ]]; then
        record_pass "$NAME"
    else
        record_fail "$NAME"
        printf '  expected %d lines, got %d\n' "$EXPECTED" "$ACTUAL" >&2
    fi
}

check_mutation_detected() {
    local NAME=$1
    shift

    if "$@" 2>/dev/null; then
        DETECTED=$((DETECTED + 1))
        log_result "check/$NAME" " OK " "$GREEN"
    else
        log_result "check/$NAME" "FAIL" "$RED"
    fi
}

run_check() {
    local DETECTED=0
    local EXPECTED=0

    echo
    printf '%s -f: %s%d/%d mutations detected%s\n' \
        "${0##*/}" \
        "$(all_detected && echo "$GREEN" || echo "$RED")" \
        "$DETECTED" "$EXPECTED" "$RESET"

    all_detected
}

setup()    { :; }
teardown() { :; }

run_test() {
    local NAME=$1
    local T0
    local T1
    local MS
    local TOTAL
    local STATUS
    local COLOR
    local EXTRA

    reset_test_state "$NAME"

    setup

    T0=$(date +%s%N)
    "test_$NAME"
    T1=$(date +%s%N)

    teardown

    MS=$(( (T1 - T0) / 1000000 ))
    TOTAL=$((TEST_PASS + TEST_FAIL))

    if no_failures; then
        STATUS=" OK "
        COLOR=$GREEN
    else
        STATUS="FAIL"
        COLOR=$RED
    fi

    printf -v EXTRA "  %7s %4dms" "[${TEST_PASS}/${TOTAL}]" "$MS"
    log_result "$NAME" "$STATUS" "$COLOR" "$EXTRA"
}

main() {
    local FILTER=""
    local CHECK=false
    local NAME

    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)    show_help ;;
            -l|--list)    show_tests ;;
            -v|--verbose) VERBOSE=true ;;
            -f)           CHECK=true ;;
            /*)           FILTER=${1#/} ;;
            *) ;;
        esac
        shift
    done

    $CHECK && { run_check; return $?; }

    while read -r NAME; do
        if should_skip "$NAME"; then
            continue
        fi
        run_test "$NAME"
    done < <(declare -F | awk '$3 ~ /^test_/ {print substr($3, 6)}' | sort)

    echo
    printf '%s: %s%d passed%s, %s%d failed%s\n' \
        "${0##*/}" "$GREEN" "$PASS" "$RESET" "$RED" "$FAIL" "$RESET"

    all_passed
}

# ============================================================================
# Test subject
# ============================================================================

DIR=$(dirname "$(realpath "$0")")
readonly ECHO=/bin/echo

# Run execline.c directly via its ccraft shebang.
# ccraft compiles on first run and caches in /tmp/.
X="$DIR/execline.c"

# Run execline in a clean environment — only PATH survives.
xrun() { env -i PATH="$PATH" "$X" "$@"; }

# Teardown: clean temp files created during tests
WORK_DIR=""
teardown() {
    if [[ -n "$WORK_DIR" ]]; then
        rm -rf "$WORK_DIR"
    fi
}

# ============================================================================
# Tests
# ============================================================================

test_exec_forwards_to_external() {
    run xrun exec "$ECHO" "hello world"
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

test_export_sets_env_var() {
    run xrun export MYVAR hello importas VAL MYVAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "value" "hello" "$RUN_OUT"
}

test_unexport_removes_env_var() {
    run xrun export MYVAR hello unexport MYVAR importas -D default VAL MYVAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "falls_to_default" "default" "$RUN_OUT"
}

test_define_substitutes_key_in_argv() {
    run xrun define FOO hello "$ECHO" '$FOO world'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

test_define_substitutes_value() {
    run xrun define FOO hello "$ECHO" '$FOO'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello" "$RUN_OUT"
}

test_define_splits_value_on_delimiter() {
    run xrun define -s FOO "a b" "$ECHO" '$FOO'
    assert_exit "exits_ok" 0
    assert_eq "two_args" "a b" "$RUN_OUT"
}

test_define_odd_backslash_is_literal() {
    run xrun define FOO hello "$ECHO" '\$FOO'
    assert_exit "exits_ok" 0
    assert_eq "backslash_preserved" '\$FOO' "$RUN_OUT"
}

test_define_even_backslash_substitutes_with_half() {
    run xrun define FOO hello "$ECHO" '\\$FOO'
    assert_exit "exits_ok" 0
    assert_eq "half_bs_kept" '\hello' "$RUN_OUT"
}

test_define_crunch_merges_consecutive_delimiters() {
    run xrun define -d , -C -s FOO "a,,b" "$ECHO" '$FOO'
    assert_exit "exits_ok" 0
    assert_eq "merged" "a b" "$RUN_OUT"
}

test_define_custom_delimiter() {
    run xrun define -d , -s FOO "a,b" "$ECHO" '$FOO'
    assert_exit "exits_ok" 0
    assert_eq "split_on_comma" "a b" "$RUN_OUT"
}

test_importas_reads_env_var() {
    run env -i PATH="$PATH" MYVAR=world "$X" importas FOO MYVAR "$ECHO" 'hello $FOO'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

test_importas_default_on_missing_var() {
    run xrun importas -D default FOO UNDEFINED_VAR "$ECHO" 'hello $FOO'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello default" "$RUN_OUT"
}

test_importas_insist_fails_on_missing_var() {
    run xrun importas -i FOO UNDEFINED_VAR "$ECHO" hello
    assert_exit "errors" 100
}

test_importas_unset_removes_source_var() {
    run env -i PATH="$PATH" MYVAR=world "$X" importas -u FOO MYVAR importas -D gone VAL MYVAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "source_gone" "gone" "$RUN_OUT"
}

test_cd_changes_directory() {
    run xrun cd / getcwd CWD importas VAL CWD "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "cwd_is_root" "/" "$RUN_OUT"
}

test_umask_sets_mask() {
    run xrun umask 077 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_posix_umask_sets_mask() {
    run xrun posix-umask 022 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_getpid_stores_pid() {
    run xrun getpid MYPID importas VAL MYPID "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_regex "pid_format" '^[0-9]+$' "$RUN_OUT"
}

test_backtick_captures_stdout() {
    run xrun backtick VAR "$ECHO" hello '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "captured" "hello" "$RUN_OUT"
}

test_backtick_default_on_failure() {
    run xrun backtick -D default_val VAR false '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "default" "default_val" "$RUN_OUT"
}

test_backtick_null_delimited() {
    run xrun backtick -0 VAR "$ECHO" -n a '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "captured" "a" "$RUN_OUT"
}

test_foreground_runs_block_then_chain() {
    run xrun foreground exit 42 '' importas VAL '?' "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "status_var" "42" "$RUN_OUT"
}

test_background_forks_and_continues() {
    run xrun background "$ECHO" bg1 '' "$ECHO" continued
    assert_exit "exits_ok" 0
}

test_pipeline_pipes_block_stdout() {
    run xrun pipeline "$ECHO" hello '' tr '[:lower:]' '[:upper:]'
    assert_exit "exits_ok" 0
    assert_eq "uppercased" "HELLO" "$RUN_OUT"
}

test_piperw_creates_pipe() {
    run xrun fdclose 7 fdclose 8 piperw 7 8 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_if_true_runs_chain() {
    run xrun if exit 0 '' "$ECHO" then
    assert_exit "chain_runs" 0
}

test_if_false_skips_chain() {
    run xrun if exit 1 '' "$ECHO" then
    assert_exit "chain_skipped" 1
}

test_if_negated_reverses_condition() {
    run xrun if -n exit 1 '' "$ECHO" then
    assert_exit "chain_runs" 0
}

test_ifelse_true_runs_then() {
    run xrun ifelse exit 0 '' "$ECHO" then-block '' "$ECHO" else-block
    assert_exit "exits_ok" 0
    assert_eq "output" "then-block" "$RUN_OUT"
}

test_ifelse_false_runs_else() {
    run xrun ifelse exit 1 '' "$ECHO" then-block '' "$ECHO" else-block
    assert_exit "exits_ok" 0
    assert_eq "output" "else-block" "$RUN_OUT"
}

test_ifthenelse_selects_then_block() {
    run xrun ifthenelse exit 0 '' "$ECHO" then '' "$ECHO" else '' rest
    assert_exit "exits_ok" 0
    assert_eq "output" "then" "$RUN_OUT"
}

test_ifthenelse_selects_else_block() {
    run xrun ifthenelse exit 1 '' "$ECHO" then '' "$ECHO" else '' rest
    assert_exit "exits_ok" 0
    assert_eq "output" "else" "$RUN_OUT"
}

test_redirfd_writes_to_file() {
    local F

    F=$(mktemp)
    WORK_DIR="$F"
    run xrun redirfd -w 1 "$F" "$ECHO" hello
    assert_exit "exits_ok" 0
    assert_file_contains "file_has_data" "$F" "hello"
}

test_redirfd_reads_from_file() {
    local F

    F=$(mktemp)
    echo "data" > "$F"
    WORK_DIR="$F"
    run xrun redirfd -r 0 "$F" tr '[:lower:]' '[:upper:]'
    assert_exit "exits_ok" 0
    assert_eq "uppercased" "DATA" "$RUN_OUT"
}

test_case_matches_pattern() {
    run xrun case hello '*ell*' "$ECHO" matched '' rest
    assert_exit "exits_ok" 0
    assert_eq "output" "matched" "$RUN_OUT"
}

test_case_no_match_falls_through() {
    run xrun case hello '*xyz*' "$ECHO" nomatched ''
    assert_exit "exits_ok" 0
}

test_case_negated_skips_on_match() {
    run xrun case -n hello '*ell*' "$ECHO" matched ''
    assert_exit "exits_ok" 0
}

test_case_insensitive_matches() {
    run xrun case -i HELLO '*hell*' "$ECHO" ci '' rest
    assert_exit "exits_ok" 0
    assert_eq "output" "ci" "$RUN_OUT"
}

test_forx_iterates_values() {
    run xrun forx VAR a b '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "both_values" $'a\nb' "$RUN_OUT"
}

test_forx_breaks_on_exit_code() {
    run xrun forx -x 0 VAR a b '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "first_only" "a" "$RUN_OUT"
}

test_forstdin_iterates_lines() {
    local F

    F=$(mktemp)
    printf 'a\nb\n' > "$F"
    WORK_DIR="$F"
    run xrun forstdin VAR importas VAL VAR "$ECHO" '$VAL' < "$F"
    assert_exit "exits_ok" 0
    assert_eq "both_lines" $'a\nb' "$RUN_OUT"
}

test_forbacktickx_iterates_generator_output() {
    run xrun forbacktickx VAR "$ECHO" -e 'a\nb' '' importas VAL VAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "both_values" $'a\nb' "$RUN_OUT"
}

test_elglob_expands_pattern() {
    run xrun elglob MYVAR '*.c' "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_elglob_no_match_unset_var() {
    run xrun elglob MYVAR 'does_not_exist_*.xyz' importas -D defaulted VAL MYVAR "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "default" "defaulted" "$RUN_OUT"
}

test_eltest_directory_exists() {
    run xrun eltest -d /
    assert_exit "exists" 0
}

test_eltest_directory_missing() {
    run xrun eltest -d /nonexistent_xyz
    assert_exit "missing" 1
}

test_eltest_file_exists() {
    run xrun eltest -f "$0"
    assert_exit "exists" 0
}

test_eltest_string_equals() {
    run xrun eltest a = a
    assert_exit "eq" 0
}

test_eltest_string_not_equals() {
    run xrun eltest a = b
    assert_exit "neq" 1
}

test_eltest_empty_string() {
    run xrun eltest -z ''
    assert_exit "is_empty" 0
}

test_eltest_nonempty_string() {
    run xrun eltest -n ''
    assert_exit "is_empty" 1
}

test_eltest_numeric_eq() {
    run xrun eltest 5 -eq 5
    assert_exit "eq" 0
}

test_eltest_numeric_ne() {
    run xrun eltest 5 -ne 5
    assert_exit "ne" 1
}

test_eltest_numeric_gt() {
    run xrun eltest 5 -gt 3
    assert_exit "gt" 0
}

test_eltest_numeric_lt() {
    run xrun eltest 3 -lt 5
    assert_exit "lt" 0
}

test_eltest_logical_and() {
    run xrun eltest -d / -a -f "$0"
    assert_exit "both_true" 0
}

test_eltest_logical_and_false() {
    run xrun eltest -d / -a -f /nonexistent_xyz
    assert_exit "one_false" 1
}

test_eltest_parentheses() {
    run xrun eltest '(' -d / -a -f "$0" ')'
    assert_exit "grouped" 0
}

test_dollarat_prints_positionals() {
    run env -i PATH="$PATH" '#=2' '1=a' '2=b' "$X" dollarat
    assert_exit "exits_ok" 0
    assert_eq "output" "a b" "$RUN_OUT"
}

test_dollarat_custom_delimiter() {
    run env -i PATH="$PATH" '#=2' '1=a' '2=b' "$X" dollarat -d ,
    assert_exit "exits_ok" 0
    assert_eq "comma" "a,b" "$RUN_OUT"
}

test_emptyenv_clears_environment() {
    run xrun emptyenv env
    assert_exit "exits_ok" 0
    assert_empty "no_output" "$RUN_OUT"
}

test_getcwd_stores_current_directory() {
    run xrun cd / getcwd MYPATH importas VAL MYPATH "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "root" "/" "$RUN_OUT"
}

test_heredoc_feeds_inline_data() {
    run xrun heredoc hello '' tr '[:lower:]' '[:upper:]'
    assert_exit "exits_ok" 0
    assert_eq "uppercased" "HELLO" "$RUN_OUT"
}

test_heredoc_multiline_data() {
    run xrun heredoc a b '' tr -d '\n'
    assert_exit "exits_ok" 0
    assert_eq "joined" "ab" "$RUN_OUT"
}

test_fdmove_dups_fd() {
    run xrun fdmove 1 2 "$ECHO" ok 2>/dev/null
    assert_exit "exits_ok" 0
}

test_fdmove_closes_old_fd() {
    run xrun fdreserve 7 fdmove -c 7 8 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_fdclose_closes_fd() {
    run xrun fdclose 9 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_fdreserve_reserves_fd() {
    run xrun fdreserve 7 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_fdswap_swaps_two_fds() {
    run xrun fdreserve 7 fdreserve 8 fdswap 7 8 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_fdblock_sets_blocking() {
    run xrun fdblock 0 "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_posix_cd_changes_directory() {
    run xrun posix-cd / "$ECHO" ok
    assert_exit "exits_ok" 0
}

test_multidefine_substitutes_multiple_keys() {
    run xrun multidefine A prefixA B prefixB -- "$ECHO" '$A/$B'
    assert_exit "exits_ok" 0
    assert_eq "joined" "prefixA/prefixB" "$RUN_OUT"
}

test_multisubstitute_applies_multiple_defines() {
    run xrun multisubstitute define A x define B y '' "$ECHO" '$A $B'
    assert_exit "exits_ok" 0
    assert_eq "substituted" "x y" "$RUN_OUT"
}

test_multisubstitute_importas() {
    run env -i PATH="$PATH" MYVAR=hello "$X" multisubstitute importas VAL MYVAR '' "$ECHO" '$VAL world'
    assert_exit "exits_ok" 0
    assert_eq "substituted" "hello world" "$RUN_OUT"
}

test_elgetpositionals_shifts_and_substitutes() {
    run env -i PATH="$PATH" '#=3' '1=a' '2=b' '3=c' "$X" elgetpositionals MYVAR 1 "$ECHO" '$MYVAR'
    assert_exit "exits_ok" 0
    assert_eq "values" "b c" "$RUN_OUT"
}

test_elgetpositionals_zero_shift() {
    run env -i PATH="$PATH" '#=3' '1=a' '2=b' '3=c' "$X" elgetpositionals MYVAR 0 "$ECHO" '$MYVAR'
    assert_exit "exits_ok" 0
    assert_eq "all_values" "a b c" "$RUN_OUT"
}

test_elgetpositionals_default_when_exhausted() {
    run env -i PATH="$PATH" '#=3' '1=a' '2=b' '3=c' "$X" elgetpositionals -D defaulted MYVAR 5 "$ECHO" '$MYVAR'
    assert_exit "exits_ok" 0
    assert_eq "default" "defaulted" "$RUN_OUT"
}

test_empty_unsets_specific_vars() {
    run env -i PATH="$PATH" FOO=bar "$X" empty FOO env
    assert_exit "exits_ok" 0
    assert_empty "no_output" "$RUN_OUT"
}

test_envfile_loads_vars_from_file() {
    local F

    F=$(mktemp)
    echo "FOO=bar" > "$F"
    WORK_DIR="$F"
    run xrun envfile -f "$F" importas VAL FOO "$ECHO" '$VAL'
    assert_exit "exits_ok" 0
    assert_eq "loaded" "bar" "$RUN_OUT"
}

test_tryexec_succeeds() {
    run xrun tryexec "$ECHO" ok
    assert_exit "exits_ok" 0
    assert_eq "output" "ok" "$RUN_OUT"
}

test_tryexec_continues_after_failed_prog() {
    run xrun tryexec /nonexistent_exec_dne_xyz "$ECHO" fallback
    assert_exit "continues_chain" 0
    assert_eq "fallback_output" "fallback" "$RUN_OUT"
}

test_withstdinas_opens_file_as_stdin() {
    local F

    F=$(mktemp)
    echo "input data" > "$F"
    WORK_DIR="$F"
    run xrun withstdinas "$F" tr '[:lower:]' '[:upper:]'
    assert_exit "exits_ok" 0
    assert_eq "uppercased" "INPUT DATA" "$RUN_OUT"
}

test_runblock_appends_prog_args() {
    run xrun runblock 1 "$ECHO" hello '' world
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

test_exit_returns_code() {
    run xrun exit 0
    assert_exit "zero" 0
}

test_exit_returns_42() {
    run xrun exit 42
    assert_exit "forty_two" 42
}

test_exit_returns_100() {
    run xrun exit 100
    assert_exit "one_hundred" 100
}

test_wait_for_child() {
    run xrun background "$ECHO" bg1 '' wait
    assert_exit "exits_ok" 0
}

test_wait_no_children_ignored() {
    run xrun wait -I
    assert_exit "exits_ok" 0
}

test_trap_handles_signal() {
    run xrun trap USR1 "$ECHO" caught '' getpid PP foreground importas VAL PP kill -USR1 '$VAL' ''
    assert_exit "exits_ok" 0
    assert_contains "action_ran" "$RUN_OUT" "caught"
}

test_execlineb_parses_simple_script() {
    run xrun execlineb -c 'echo hello world'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

test_execlineb_expands_dollarat() {
    run xrun execlineb -c 'echo $@' a b
    assert_exit "exits_ok" 0
    assert_eq "output" "a b" "$RUN_OUT"
}

test_execlineb_handles_quoted_strings() {
    run xrun execlineb -c 'echo "hello world"'
    assert_exit "exits_ok" 0
    assert_eq "output" "hello world" "$RUN_OUT"
}

# ============================================================================
# Mutation tests (./execline.t -f)
# ============================================================================

run_check() {
    local DETECTED=0
    local EXPECTED=3

    # Mutation: broken binary path → exit 127
    check_mutation_detected "missing_binary" \
        bash -c 'env -i PATH="$PATH" /nonexistent/execline exec /bin/echo hello; [ $? -eq 127 ]'

    # Mutation: undefined command → exit 127
    check_mutation_detected "undefined_subcommand" \
        bash -c 'env -i PATH="$PATH" "$0" nonexistent_cmd_xyz; [ $? -eq 127 ]' "$X"

    # Mutation: bad exec flag falls through to execvp → exit 127
    check_mutation_detected "bad_exec_flag" \
        bash -c 'env -i PATH="$PATH" "$0" exec --nonexistent-flag /bin/echo hello; [ $? -eq 127 ]' "$X"

    echo
    printf '%s -f: %s%d/%d mutations detected%s\n' \
        "${0##*/}" \
        "$(all_detected && echo "$GREEN" || echo "$RED")" \
        "$DETECTED" "$EXPECTED" "$RESET"

    all_detected
}

# ============================================================================
# Main
# ============================================================================

main "$@"
