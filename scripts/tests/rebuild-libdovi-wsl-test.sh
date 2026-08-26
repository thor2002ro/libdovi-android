#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test_root="$(mktemp -d -t libdovi-build-script-tests.XXXXXXXX)"

cleanup() {
    rm -rf -- "$test_root"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

make_executable() {
    local path="$1"
    shift
    printf '%s\n' '#!/usr/bin/env bash' "$@" > "$path"
    chmod +x "$path"
}

create_fixture() {
    local fixture="$1"
    mkdir -p \
        "$fixture/dependencies/libdovi-android/scripts" \
        "$fixture/dependencies/libdovi-android/OUTPUT" \
        "$fixture/fake-bin" \
        "$fixture/android-sdk/platforms/android-36" \
        "$fixture/android-sdk/build-tools/36.0.0" \
        "$fixture/android-sdk/ndk/29.0.14206865"

    cp "$repository_root/rebuild-libdovi-wsl.sh" \
        "$fixture/dependencies/libdovi-android/rebuild-libdovi-wsl.sh"
    printf '%s\n' 'VERSION_NAME=0.1.0-SNAPSHOT' 'androidNdkVersion=29.0.14206865' \
        > "$fixture/dependencies/libdovi-android/gradle.properties"
    touch "$fixture/android-sdk/platforms/android-36/android.jar"
    touch "$fixture/android-sdk/ndk/29.0.14206865/source.properties"
    make_executable "$fixture/android-sdk/build-tools/36.0.0/aapt" 'exit 0'

    make_executable "$fixture/fake-bin/uname" 'echo Linux'
    make_executable "$fixture/fake-bin/java" 'exit 0'
    make_executable "$fixture/fake-bin/git" 'exit 0'
    make_executable "$fixture/fake-bin/curl" 'exit 0'
    make_executable "$fixture/fake-bin/rustup" \
        'if [[ "${1:-}" == target && "${2:-}" == list ]]; then' \
        '  printf "%s (installed)\n" armv7-linux-androideabi aarch64-linux-android i686-linux-android x86_64-linux-android' \
        'fi'
    make_executable "$fixture/fake-bin/cargo" 'exit 0'

    make_executable "$fixture/dependencies/libdovi-android/scripts/build-native.sh" \
        'echo native >> "$BUILD_LOG"' \
        'for abi in armeabi-v7a arm64-v8a x86 x86_64; do' \
        '  root="$(cd "$(dirname "$0")/.." && pwd)/OUTPUT/native/$abi"' \
        '  mkdir -p "$root/include" "$root/lib/pkgconfig"' \
        '  touch "$root/libjellyfin_dovi.so" "$root/include/dovi.h"' \
        '  printf "%s\n" "Name: jellyfin-dovi" "Description: test package" > "$root/lib/pkgconfig/jellyfin-dovi.pc"' \
        '  if [[ -z "${OMIT_PC_VERSION:-}" ]]; then echo "Version: 0.1.0-SNAPSHOT" >> "$root/lib/pkgconfig/jellyfin-dovi.pc"; fi' \
        '  printf "%s\n" "Libs: -L\${libdir} -ljellyfin_dovi" "Cflags: -I\${includedir}" >> "$root/lib/pkgconfig/jellyfin-dovi.pc"' \
        '  echo abc123 > "$root/UPSTREAM_SHA"' \
        'done'
}

create_gradle_fixture() {
    local fixture="$1"
    local omit_abi="${2:-}"
    local mismatch_aar_abi="${3:-}"
    make_executable "$fixture/gradlew" \
        'echo gradle >> "$BUILD_LOG"' \
        'repo=""' \
        'while (($#)); do' \
        '  if [[ "$1" == -p ]]; then repo="$2"; shift 2; else shift; fi' \
        'done' \
        'maven="$repo/OUTPUT/maven/io/github/thor2002ro/libdovi-android/0.1.0-SNAPSHOT"' \
        'mkdir -p "$maven"' \
        'for abi in armeabi-v7a arm64-v8a x86 x86_64; do' \
        '  [[ "$abi" == "${OMIT_SDK_ABI:-}" ]] && continue' \
        '  root="$repo/OUTPUT/sdk/$abi"' \
        '  mkdir -p "$root/include" "$root/lib/pkgconfig"' \
        '  cp "$repo/OUTPUT/native/$abi/libjellyfin_dovi.so" "$root/lib/libjellyfin_dovi.so"' \
        '  cp "$repo/OUTPUT/native/$abi/include/dovi.h" "$root/include/dovi.h"' \
        '  cp "$repo/OUTPUT/native/$abi/lib/pkgconfig/jellyfin-dovi.pc" "$root/lib/pkgconfig/jellyfin-dovi.pc"' \
        'done' \
        'aar_stage="$repo/aar-stage"' \
        'for abi in armeabi-v7a arm64-v8a x86 x86_64; do' \
        '  mkdir -p "$aar_stage/jni/$abi"' \
        '  cp "$repo/OUTPUT/native/$abi/libjellyfin_dovi.so" "$aar_stage/jni/$abi/libjellyfin_dovi.so"' \
        'done' \
        'if [[ -n "${MISMATCH_AAR_ABI:-}" ]]; then echo changed > "$aar_stage/jni/$MISMATCH_AAR_ABI/libjellyfin_dovi.so"; fi' \
        '(cd "$aar_stage" && jar cf "$maven/libdovi-android-0.1.0-SNAPSHOT.aar" .)' \
        'rm -rf "$aar_stage"'
    export OMIT_SDK_ABI="$omit_abi"
    export MISMATCH_AAR_ABI="$mismatch_aar_abi"
}

run_wrapper() {
    local fixture="$1"
    shift
    PATH="$fixture/fake-bin:$PATH" \
        ANDROID_SDK_ROOT="$fixture/android-sdk" \
        BUILD_LOG="$fixture/build.log" \
        "$fixture/dependencies/libdovi-android/rebuild-libdovi-wsl.sh" "$@"
}

successful_fixture="$test_root/success"
create_fixture "$successful_fixture"
create_gradle_fixture "$successful_fixture"
success_output="$(run_wrapper "$successful_fixture")"
[[ "$(cat "$successful_fixture/build.log")" == $'native\ngradle' ]] || \
    fail "native compilation and Gradle publication did not run in order"
[[ "$success_output" == *"Saved libdovi AAR, Maven repository, and native SDK"* ]] || \
    fail "success output did not identify the published artifacts"

missing_version_fixture="$test_root/missing-version"
create_fixture "$missing_version_fixture"
create_gradle_fixture "$missing_version_fixture"
export OMIT_PC_VERSION=1
if missing_version_output="$(run_wrapper "$missing_version_fixture" 2>&1)"; then
    fail "wrapper accepted pkg-config metadata without a version"
fi
unset OMIT_PC_VERSION
[[ "$missing_version_output" == *"Missing pkg-config Version"* ]] || \
    fail "missing version failure was not actionable: $missing_version_output"

incomplete_fixture="$test_root/incomplete"
create_fixture "$incomplete_fixture"
create_gradle_fixture "$incomplete_fixture" x86
if incomplete_output="$(run_wrapper "$incomplete_fixture" 2>&1)"; then
    fail "wrapper accepted an incomplete native SDK"
fi
[[ "$incomplete_output" == *"Missing published SDK artifact"* ]] || \
    fail "incomplete SDK failure was not actionable: $incomplete_output"

mismatched_aar_fixture="$test_root/mismatched-aar"
create_fixture "$mismatched_aar_fixture"
create_gradle_fixture "$mismatched_aar_fixture" '' arm64-v8a
if mismatched_aar_output="$(run_wrapper "$mismatched_aar_fixture" 2>&1)"; then
    fail "wrapper accepted an AAR containing a changed JNI library"
fi
[[ "$mismatched_aar_output" == *"AAR and native libraries differ for arm64-v8a"* ]] || \
    fail "AAR mismatch failure was not actionable: $mismatched_aar_output"

echo "libdovi WSL build wrapper contracts passed"
