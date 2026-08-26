#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test_root="$repository_root/native/tests"
source_root="$repository_root/native/src"
include_root="$repository_root/native/include"
output_root="$(mktemp -d -t dovi-tests.XXXXXXXX)"

case "$output_root" in
    /tmp/dovi-tests.*|"${TMPDIR:-/tmp}"/dovi-tests.*) ;;
    *) echo "Unsafe test output path: $output_root" >&2; exit 2 ;;
esac

cleanup() {
    case "$output_root" in
        /tmp/dovi-tests.*|"${TMPDIR:-/tmp}"/dovi-tests.*)
            rm -rf -- "$output_root"
            ;;
        *)
            echo "Refusing to remove unsafe test output path: $output_root" >&2
            ;;
    esac
}
trap cleanup EXIT INT TERM

"$test_root/validate-source-contracts.sh"

compiler_args=(
    -std=c++17
    -Wall
    -Wextra
    -Werror
    -I"$include_root"
    -I"$source_root"
    "$source_root/dovi.cpp"
    "$source_root/dovi_mpv_state.cpp"
    "$test_root/dovi_test.cpp"
)

fixture_args=()
if [ "$#" -eq 5 ]; then
    compiler_args+=(
        -DDOVI_REAL_LIBDOVI
        -Wl,--whole-archive "$1" -Wl,--no-whole-archive
        -ldl
        -lpthread
        -lm
    )
    fixture_args=("$2" "$3" "$4" "$5")
elif [ "$#" -ne 0 ]; then
    echo "usage: $0 [LIBDOVI_STATIC P7_INPUT EXPECTED_P81 P84_INPUT EXPECTED_P84]" >&2
    exit 2
fi

"${CXX:-c++}" "${compiler_args[@]}" -o "$output_root/dovi_test"

"$output_root/dovi_test" "${fixture_args[@]}"
