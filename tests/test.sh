#!/bin/sh
# Behavioral tests for dware.
# Usage: tests/test.sh <dware-binary> <eff_large_wordlist.txt> [parallel-jobs]
set -u

if [ $# -lt 2 ]; then
    echo "usage: $0 <dware-binary> <eff_large_wordlist.txt> [parallel-jobs]" >&2
    exit 2
fi
bin=$1
list=$2
jobs=${3:-4}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cut -f2 "$list" > "$tmp/words"

failures=0
checks=0
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }

# run ARGS... : runs dware, leaving stdout/stderr in files and the exit status in $status.
run() {
    "$bin" "$@" > "$tmp/out" 2> "$tmp/err"
    status=$?
    checks=$((checks + 1))
}

# expect_words NAME COUNT SEPARATOR: checks stdout is one line of COUNT listed words.
expect_words() {
    name=$1 count=$2 sep=$3
    [ "$status" -eq 0 ] || { fail "$name: exit status $status"; return; }
    [ "$(wc -l < "$tmp/out")" -eq 1 ] || { fail "$name: expected exactly one line"; return; }
    awk -v sep="$sep" '{ n = split($0, w, sep); for (i = 1; i <= n; i++) print w[i] }' "$tmp/out" > "$tmp/split"
    [ "$(wc -l < "$tmp/split")" -eq "$count" ] || fail "$name: expected $count words, got $(wc -l < "$tmp/split")"
    if grep -vxFf "$tmp/words" "$tmp/split" > "$tmp/unknown"; then
        fail "$name: words not in the list: $(tr '\n' ' ' < "$tmp/unknown")"
    fi
}

# expect_usage_error NAME ARGS...
expect_usage_error() {
    name=$1
    shift
    run "$@"
    [ "$status" -eq 2 ] || fail "$name: expected exit status 2, got $status"
    [ -s "$tmp/out" ] && fail "$name: printed to stdout"
    [ -s "$tmp/err" ] || fail "$name: no explanation on stderr"
}

# Nominal output.
run
expect_words "default" 6 " "
[ -s "$tmp/err" ] && fail "default: unexpected stderr output"
run -n 1;  expect_words "-n 1" 1 " "
run -n 64; expect_words "-n 64" 64 " "
run -n 8 -s .;   expect_words "-s ." 8 "."
run -n 4 -s ::;  expect_words "-s ::" 4 "::"
run -n 2 -s 'abcdefghijklmnop'; expect_words "16-byte separator" 2 "abcdefghijklmnop"
run -n 3 -s '';
[ "$status" -eq 0 ] && grep -qxE '[a-z-]{9,27}' "$tmp/out" || fail "empty separator: unexpected output"
run -e;
expect_words "-e" 6 " "
[ -s "$tmp/err" ] || fail "-e: no entropy report on stderr"
run -h; { [ "$status" -eq 0 ] && [ -s "$tmp/out" ]; } || fail "-h: expected help on stdout"
run -V; { [ "$status" -eq 0 ] && [ -s "$tmp/out" ]; } || fail "-V: expected version on stdout"

# Off-nominal input.
expect_usage_error "-n 0" -n 0
expect_usage_error "-n 65" -n 65
expect_usage_error "-n -3" -n -3
expect_usage_error "-n abc" -n abc
expect_usage_error "-n 5x" -n 5x
expect_usage_error "-n empty" -n ''
expect_usage_error "-n huge" -n 99999999999999999999999
expect_usage_error "-n missing value" -n
expect_usage_error "17-byte separator" -s 'abcdefghijklmnopq'
expect_usage_error "unknown option" -x
expect_usage_error "positional argument" extra

# Unwritable output must fail loudly rather than print nothing and succeed.
if [ -w /dev/full ]; then
    "$bin" > /dev/full 2> "$tmp/err"
    status=$?
    checks=$((checks + 1))
    [ "$status" -eq 1 ] || fail "/dev/full: expected exit status 1, got $status"
    [ -s "$tmp/err" ] || fail "/dev/full: no explanation on stderr"
fi

# Randomness sanity: 200 runs x 64 words = 12800 draws. Expected distinct words is
# 7776 * (1 - e^(-12800/7776)) ~= 6280, and draws from the first half of the list
# ~= 6400 +/- 57 (1 sigma). The bounds below sit many sigmas out, so they only trip
# on a broken generator, not on chance.
seq 200 | xargs -P "$jobs" -I{} "$bin" -n 64 > "$tmp/many" 2> "$tmp/err"
checks=$((checks + 1))
[ "$(wc -l < "$tmp/many")" -eq 200 ] || fail "bulk: expected 200 passphrases"
tr ' ' '\n' < "$tmp/many" > "$tmp/many-words"
if grep -vxFf "$tmp/words" "$tmp/many-words" > /dev/null; then
    fail "bulk: produced words not in the list"
fi
awk 'NR == FNR { index_of[$1] = FNR; next }
     { seen[$1] = 1; if (index_of[$1] <= 3888) low++ }
     END { for (w in seen) distinct++; print distinct, low }' "$tmp/words" "$tmp/many-words" > "$tmp/stats"
read -r distinct low < "$tmp/stats"
[ "$distinct" -gt 5900 ] && [ "$distinct" -lt 6600 ] || fail "bulk: implausible distinct word count $distinct"
[ "$low" -gt 6000 ] && [ "$low" -lt 6800 ] || fail "bulk: lopsided first-half count $low of 12800"
[ "$(sort -u "$tmp/many" | wc -l)" -eq 200 ] || fail "bulk: repeated passphrase"

if [ "$failures" -ne 0 ]; then
    echo "$bin: $failures of $checks checks failed" >&2
    exit 1
fi
echo "$bin: all $checks checks passed"
