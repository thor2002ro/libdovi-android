#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jni_source="$repository_root/native/src/dovi_jni.cpp"
public_header="$repository_root/native/include/dovi.h"
capi_header="$repository_root/native/src/libdovi_capi.h"
build_script="$repository_root/scripts/build-native.sh"

normalized_signature() {
    local symbol="$1"
    awk -v symbol="$symbol" '
        index($0, symbol) { capture = 1 }
        capture { printf "%s", $0 }
        capture && /\) \{/ { exit }
    ' "$jni_source" | tr -d '[:space:]'
}

assert_signature() {
    local symbol="$1"
    local expected="$2"
    local actual
    actual="$(normalized_signature "$symbol")"
    if [ "$actual" != "$expected" ]; then
        echo "JNI signature mismatch for $symbol: $actual" >&2
        exit 1
    fi
}

assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeAbiVersion' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeAbiVersion(JNIEnv*,jobject){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeCapabilities' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeCapabilities(JNIEnv*,jobject){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeInspectSample' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeInspectSample(JNIEnv*env,jobject,jbyteArrayinput_array,jintframing,jintnal_length_size,jintsource_base_presentation,jbyteArraysupplemental_array,jintArrayinfo_array){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSample' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSample(JNIEnv*env,jobject,jbyteArrayinput_array,jintframing,jintnal_length_size,jintsource_base_presentation,jbyteArraysupplemental_array,jinttarget,jintrepair_flags,jbyteArrayoutput_array,jlongArrayoutput_size_array,jintArrayinfo_array){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeWriteAv1T35' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeWriteAv1T35(JNIEnv*env,jobject,jbyteArrayrpu_array,jintrpu_format,jbooleancomplete_obu,jbyteArrayoutput_array,jlongArrayoutput_size_array){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeSetMpvRequest' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeSetMpvRequest(JNIEnv*env,jobject,jinttarget,jintrepair_flags,jlongArraygeneration_array){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeResetMpvError' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeResetMpvError(JNIEnv*,jobject,jlonggeneration){'
assert_signature \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeConsumeMpvError' \
    'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeConsumeMpvError(JNIEnv*,jobject,jlonggeneration){'

transform_start="$(grep -n 'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSample' "$jni_source" | cut -d: -f1)"
transform_end="$(awk -v start="$transform_start" 'NR > start && /^JNIEXPORT jint JNICALL/ { print NR; exit }' "$jni_source")"
transform_end="${transform_end:-999999}"

long_validation="$(awk -v start="$transform_start" -v end="$transform_end" \
    'NR > start && NR < end && /valid_long_destination\(env, output_size_array, 1\)/ { print NR; exit }' \
    "$jni_source")"
info_validation="$(awk -v start="$transform_start" -v end="$transform_end" \
    'NR > start && NR < end && /valid_int_destination\(env, info_array, 5\)/ { print NR; exit }' \
    "$jni_source")"
core_call="$(awk -v start="$transform_start" -v end="$transform_end" \
    'NR > start && NR < end && /dovi_transform_sample\(/ { print NR; exit }' \
    "$jni_source")"
info_write="$(awk -v start="$transform_start" -v end="$transform_end" \
    'NR > start && NR < end && /write_transform_info\(/ { print NR; exit }' \
    "$jni_source")"
size_write="$(awk -v start="$transform_start" -v end="$transform_end" \
    'NR > start && NR < end && /write_output_size\(/ { print NR; exit }' \
    "$jni_source")"

test -n "$long_validation"
test -n "$info_validation"
test -n "$core_call"
test -n "$info_write"
test -n "$size_write"
test "$long_validation" -lt "$core_call"
test "$info_validation" -lt "$core_call"
test "$info_write" -lt "$size_write"

inspect_start="$(grep -n 'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeInspectSample' "$jni_source" | cut -d: -f1)"
inspect_call="$(awk -v start="$inspect_start" 'NR > start && /dovi_inspect_sample\(/ { print NR; exit }' "$jni_source")"
inspect_validation="$(awk -v start="$inspect_start" -v end="$inspect_call" \
    'NR > start && NR < end && /valid_int_destination\(env, info_array, 7\)/ { print NR; exit }' \
    "$jni_source")"
test -n "$inspect_validation"

av1_start="$(grep -n 'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeWriteAv1T35' "$jni_source" | cut -d: -f1)"
av1_call="$(awk -v start="$av1_start" 'NR > start && /dovi_write_av1_t35\(/ { print NR; exit }' "$jni_source")"
av1_validation="$(awk -v start="$av1_start" -v end="$av1_call" \
    'NR > start && NR < end && /valid_long_destination\(env, output_size_array, 1\)/ { print NR; exit }' \
    "$jni_source")"
test -n "$av1_validation"

grep -q '^typedef int32_t dovi_status;$' "$public_header"
grep -q '^#define DOVI_ABI_VERSION 3u$' "$public_header"
grep -q 'dovi_record_mpv_error_v3(uint64_t generation, dovi_status status)' "$public_header"
grep -q 'dovi_get_mpv_request(dovi_transform_request\* request)' "$public_header"
grep -q 'Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSample' "$jni_source"
grep -q 'int32_t dovi_rpu_get_profile' "$capi_header"
grep -q 'int32_t dovi_rpu_get_el_type' "$capi_header"
grep -q 'int32_t dovi_rpu_has_mapping' "$capi_header"
grep -q 'int32_t dovi_rpu_has_cmv40_metadata' "$capi_header"
grep -q -- '-Wl,-soname,libjellyfin_dovi.so' "$build_script"
unstable_type='DoviRpu''DataHeader'
if grep -q "$unstable_type" "$capi_header"; then
    echo "Unstable upstream header mirror remains" >&2
    exit 1
fi

echo "Native source contracts validated"
