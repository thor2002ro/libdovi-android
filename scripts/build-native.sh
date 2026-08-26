#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_root="$repository_root/patches/libdovi"
native_source_root="$repository_root/native"
output_root="$repository_root/OUTPUT/native"
android_api="${ANDROID_API:-24}"
package_version="$(sed -n 's/^VERSION_NAME=//p' "$repository_root/gradle.properties" | head -n 1)"
temp_base="${TMPDIR:-/tmp}"

if [ -z "$package_version" ]; then
    echo "VERSION_NAME is missing from $repository_root/gradle.properties" >&2
    exit 1
fi

source_root="$(mktemp -d -t libdovi-android.XXXXXXXX)"
case "$source_root" in
    /tmp/libdovi-android.*|"$temp_base"/libdovi-android.*) ;;
    *) echo "Unsafe temporary source path: $source_root" >&2; exit 2 ;;
esac

cleanup() {
    case "$source_root" in
        /tmp/libdovi-android.*|"$temp_base"/libdovi-android.*)
            rm -rf -- "$source_root"
            ;;
        *)
            echo "Refusing to remove unsafe temporary source path: $source_root" >&2
            ;;
    esac
}
trap cleanup EXIT INT TERM

ndk_root="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$ndk_root" ]; then
    echo "ANDROID_NDK_HOME or ANDROID_NDK_ROOT must point to an Android NDK." >&2
    exit 1
fi

case "$(uname -s)" in
    Linux) host_tag=linux-x86_64 ;;
    Darwin) host_tag=darwin-x86_64 ;;
    *) echo "Unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

# A complete native publication resolves upstream exactly once, then reuses this
# clone for every ABI below.
dovi_tool="$source_root/dovi_tool"
git clone --depth 1 --branch main https://github.com/quietvoid/dovi_tool.git "$dovi_tool"
source_sha="$(git -C "$dovi_tool" rev-parse HEAD)"

for patch in "$patch_root"/*.patch; do
    git -C "$dovi_tool" apply --check "$patch"
    git -C "$dovi_tool" apply "$patch"
done

export CARGO_TARGET_DIR="$source_root/target"
for android_abi in armeabi-v7a arm64-v8a x86 x86_64; do
    case "$android_abi" in
        armeabi-v7a) rust_target=armv7-linux-androideabi ; ndk_target=armv7a-linux-androideabi ;;
        arm64-v8a) rust_target=aarch64-linux-android ; ndk_target=aarch64-linux-android ;;
        x86) rust_target=i686-linux-android ; ndk_target=i686-linux-android ;;
        x86_64) rust_target=x86_64-linux-android ; ndk_target=x86_64-linux-android ;;
        *) exit 2 ;;
    esac

    clang="$ndk_root/toolchains/llvm/prebuilt/$host_tag/bin/${ndk_target}${android_api}-clang"
    clangxx="$ndk_root/toolchains/llvm/prebuilt/$host_tag/bin/${ndk_target}${android_api}-clang++"
    if [ ! -x "$clang" ]; then
        echo "Missing Android Clang: $clang" >&2
        exit 1
    fi
    if [ ! -x "$clangxx" ]; then
        echo "Missing Android Clang++: $clangxx" >&2
        exit 1
    fi

    cargo_target_env="${rust_target^^}"
    cargo_target_env="${cargo_target_env//-/_}"
    export "CARGO_TARGET_${cargo_target_env}_LINKER=$clang"
    cargo build \
        --manifest-path "$dovi_tool/dolby_vision/Cargo.toml" \
        --locked \
        --release \
        --features capi \
        --target "$rust_target"

    static_library="$CARGO_TARGET_DIR/$rust_target/release/libdolby_vision.a"
    if [ ! -f "$static_library" ]; then
        echo "Expected static libdovi archive not found: $static_library" >&2
        exit 1
    fi

    native_root="$output_root/$android_abi"
    mkdir -p "$native_root/include" "$native_root/lib/pkgconfig"
    "$clangxx" \
        -std=c++17 \
        -fPIC \
        -fvisibility=hidden \
        -static-libstdc++ \
        -I"$native_source_root/include" \
        -I"$native_source_root/src" \
        "$native_source_root/src/dovi.cpp" \
        "$native_source_root/src/dovi_jni.cpp" \
        "$native_source_root/src/dovi_mpv_state.cpp" \
        -shared \
        -Wl,-soname,libjellyfin_dovi.so \
        -Wl,--whole-archive "$static_library" -Wl,--no-whole-archive \
        -Wl,--exclude-libs,ALL \
        -llog -ldl -lm -latomic \
        -o "$native_root/libjellyfin_dovi.so"

    cp "$native_source_root/include/dovi.h" "$native_root/include/dovi.h"

    cat > "$native_root/lib/pkgconfig/jellyfin-dovi.pc" <<EOF
prefix=$native_root
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: jellyfin-dovi
Description: Jellyfin Dolby Vision RPU conversion bridge
Version: $package_version
Libs: -L\${libdir} -ljellyfin_dovi
Cflags: -I\${includedir}
EOF

    printf '%s\n' "$source_sha" > "$native_root/UPSTREAM_SHA"
done
