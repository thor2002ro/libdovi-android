#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gradle_root="$(cd "$repo/../.." && pwd)"
android_ndk_version="$(sed -n 's/^androidNdkVersion=//p' "$repo/gradle.properties" | head -n 1 | tr -d '\r')"
package_version="$(sed -n 's/^VERSION_NAME=//p' "$repo/gradle.properties" | head -n 1 | tr -d '\r')"
download_root=""
gradle_launcher=""

cleanup() {
    if [[ -n "$download_root" ]]; then
        case "$download_root" in
            /tmp/libdovi-android-sdk.*|"${TMPDIR:-/tmp}"/libdovi-android-sdk.*)
                rm -rf -- "$download_root"
                ;;
            *) echo "Refusing to remove unsafe SDK download path: $download_root" >&2 ;;
        esac
    fi
    if [[ -n "$gradle_launcher" ]]; then
        case "$gradle_launcher" in
            "$gradle_root"/.gradlew-libdovi-wsl.*) rm -f -- "$gradle_launcher" ;;
            *) echo "Refusing to remove unsafe Gradle launcher: $gradle_launcher" >&2 ;;
        esac
    fi
}
trap cleanup EXIT INT TERM

[[ -n "$android_ndk_version" ]] || {
    echo "androidNdkVersion is missing from $repo/gradle.properties" >&2
    exit 1
}
[[ -n "$package_version" ]] || {
    echo "VERSION_NAME is missing from $repo/gradle.properties" >&2
    exit 1
}

[[ "$(uname -s)" == Linux ]] || {
    echo "This script must run inside WSL or another Linux environment." >&2
    exit 1
}

required_tools=(java git curl unzip sed sha256sum)

missing_tools() {
    local missing=()
    local tool
    for tool in "${required_tools[@]}"; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    ((${#missing[@]})) && printf '%s\n' "${missing[@]}"
}

install_missing_tools() {
    local missing=()
    mapfile -t missing < <(missing_tools)
    ((${#missing[@]} == 0)) && return

    echo "Missing WSL tools: ${missing[*]}"
    echo "Installing missing packages may ask for your WSL sudo password."
    sudo -v || {
        echo "Sudo authentication failed." >&2
        exit 1
    }

    if command -v apt-get >/dev/null 2>&1; then
        local apt_packages=()
        local tool
        for tool in "${missing[@]}"; do
            case "$tool" in
                java) apt_packages+=(openjdk-21-jdk) ;;
                sha256sum) apt_packages+=(coreutils) ;;
                *) apt_packages+=("$tool") ;;
            esac
        done
        mapfile -t apt_packages < <(printf '%s\n' "${apt_packages[@]}" | sort -u)
        sudo apt-get update
        sudo apt-get install -y "${apt_packages[@]}" ||
            sudo apt-get install -y "${apt_packages[@]/openjdk-21-jdk/default-jdk}"
    elif command -v pacman >/dev/null 2>&1; then
        local pacman_packages=()
        local tool
        for tool in "${missing[@]}"; do
            case "$tool" in
                java) pacman_packages+=(jdk21-openjdk) ;;
                sha256sum) pacman_packages+=(coreutils) ;;
                *) pacman_packages+=("$tool") ;;
            esac
        done
        mapfile -t pacman_packages < <(printf '%s\n' "${pacman_packages[@]}" | sort -u)
        sudo pacman -Sy --needed --noconfirm "${pacman_packages[@]}"
    else
        echo "Unsupported WSL distribution. Install these tools manually: ${missing[*]}" >&2
        exit 1
    fi

    mapfile -t missing < <(missing_tools)
    ((${#missing[@]} == 0)) || {
        echo "Still missing WSL tools after installation: ${missing[*]}" >&2
        exit 1
    }
}

install_rust() {
    if ! command -v rustup >/dev/null 2>&1; then
        echo "Installing Rust with rustup."
        curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs |
            sh -s -- -y --profile minimal
        # shellcheck disable=SC1091
        source "$HOME/.cargo/env"
    fi

    command -v cargo >/dev/null 2>&1 || {
        echo "cargo is unavailable after installing rustup." >&2
        exit 1
    }

    local installed_targets
    installed_targets="$(rustup target list --installed)"
    local target
    for target in \
        thumbv7neon-linux-androideabi \
        aarch64-linux-android \
        i686-linux-android \
        x86_64-linux-android; do
        grep -Fxq "$target" <<< "$installed_targets" || rustup target add "$target"
    done
}

android_package_installed() {
    local sdk_root="$1"
    case "$2" in
        platforms\;android-*) [[ -f "$sdk_root/platforms/${2#platforms;}/android.jar" ]] ;;
        build-tools\;*) [[ -x "$sdk_root/build-tools/${2#build-tools;}/aapt" ]] ;;
        ndk\;*) [[ -f "$sdk_root/ndk/${2#ndk;}/source.properties" ]] ;;
        *) return 1 ;;
    esac
}

install_android_sdk() {
    local sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}"
    local sdkmanager="$sdk_root/cmdline-tools/latest/bin/sdkmanager"
    local packages=(
        "platforms;android-36"
        "build-tools;36.0.0"
        "ndk;$android_ndk_version"
    )
    local missing=()
    local package
    for package in "${packages[@]}"; do
        android_package_installed "$sdk_root" "$package" || missing+=("$package")
    done

    if ((${#missing[@]})); then
        if [[ ! -x "$sdkmanager" ]]; then
            if [[ -e "$sdk_root/cmdline-tools/latest" ]]; then
                echo "Incomplete Android command-line tools installation: $sdk_root/cmdline-tools/latest" >&2
                exit 1
            fi
            download_root="$(mktemp -d -t libdovi-android-sdk.XXXXXXXX)"
            curl -fsSL \
                https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip \
                -o "$download_root/commandline-tools.zip"
            unzip -q "$download_root/commandline-tools.zip" -d "$download_root/unpacked"
            mkdir -p "$sdk_root/cmdline-tools"
            mv "$download_root/unpacked/cmdline-tools" "$sdk_root/cmdline-tools/latest"
        fi

        echo "Installing Android SDK packages: ${missing[*]}"
        set +o pipefail
        yes | "$sdkmanager" --sdk_root="$sdk_root" --licenses >/dev/null
        yes | "$sdkmanager" --sdk_root="$sdk_root" "${missing[@]}"
        set -o pipefail
    fi

    export ANDROID_HOME="$sdk_root"
    export ANDROID_SDK_ROOT="$sdk_root"
    export ANDROID_NDK_HOME="$sdk_root/ndk/$android_ndk_version"
    export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
}

validate_outputs() {
    local aar
    aar="$(find "$repo/OUTPUT/maven" -type f -name 'libdovi-android-*.aar' -printf '%T@ %p\n' |
        sort -nr | head -n 1 | cut -d' ' -f2-)"
    [[ -n "$aar" ]] || {
        echo "Missing published libdovi AAR under $repo/OUTPUT/maven" >&2
        exit 1
    }

    local abi
    local aar_hash
    local relative
    local native_file
    local native_hash
    local sdk_file
    local pkg_config_version
    for abi in armeabi-v7a arm64-v8a x86 x86_64; do
        for relative in \
            libjellyfin_dovi.so \
            include/dovi.h \
            lib/pkgconfig/jellyfin-dovi.pc; do
            native_file="$repo/OUTPUT/native/$abi/$relative"
            [[ -f "$native_file" ]] || {
                echo "Missing native build artifact: $native_file" >&2
                exit 1
            }
        done
        pkg_config_version="$(sed -n 's/^Version:[[:space:]]*//p' "$repo/OUTPUT/native/$abi/lib/pkgconfig/jellyfin-dovi.pc" | head -n 1)"
        [[ -n "$pkg_config_version" ]] || {
            echo "Missing pkg-config Version: $repo/OUTPUT/native/$abi/lib/pkgconfig/jellyfin-dovi.pc" >&2
            exit 1
        }
        [[ "$pkg_config_version" == "$package_version" ]] || {
            echo "Unexpected pkg-config Version for $abi: $pkg_config_version" >&2
            exit 1
        }
        [[ -s "$repo/OUTPUT/native/$abi/UPSTREAM_SHA" ]] || {
            echo "Missing upstream SHA: $repo/OUTPUT/native/$abi/UPSTREAM_SHA" >&2
            exit 1
        }

        for relative in \
            lib/libjellyfin_dovi.so \
            include/dovi.h \
            lib/pkgconfig/jellyfin-dovi.pc; do
            sdk_file="$repo/OUTPUT/sdk/$abi/$relative"
            [[ -f "$sdk_file" ]] || {
                echo "Missing published SDK artifact: $sdk_file" >&2
                exit 1
            }
        done
        pkg_config_version="$(sed -n 's/^Version:[[:space:]]*//p' "$repo/OUTPUT/sdk/$abi/lib/pkgconfig/jellyfin-dovi.pc" | head -n 1)"
        [[ -n "$pkg_config_version" ]] || {
            echo "Missing pkg-config Version: $repo/OUTPUT/sdk/$abi/lib/pkgconfig/jellyfin-dovi.pc" >&2
            exit 1
        }
        [[ "$pkg_config_version" == "$package_version" ]] || {
            echo "Unexpected published pkg-config Version for $abi: $pkg_config_version" >&2
            exit 1
        }

        native_hash="$(sha256sum "$repo/OUTPUT/native/$abi/libjellyfin_dovi.so" | cut -d' ' -f1)"
        [[ "$native_hash" == \
            "$(sha256sum "$repo/OUTPUT/sdk/$abi/lib/libjellyfin_dovi.so" | cut -d' ' -f1)" ]] || {
            echo "Native and SDK libraries differ for $abi" >&2
            exit 1
        }

        aar_hash="$(unzip -p "$aar" "jni/$abi/libjellyfin_dovi.so" | sha256sum | cut -d' ' -f1)"
        [[ "$aar_hash" == "$native_hash" ]] || {
            echo "AAR and native libraries differ for $abi" >&2
            exit 1
        }
    done
}

install_missing_tools
install_rust
install_android_sdk

"$repo/scripts/build-native.sh"

[[ -f "$gradle_root/gradlew" ]] || {
    echo "Missing parent Gradle wrapper: $gradle_root/gradlew" >&2
    exit 1
}
gradle_launcher="$(mktemp "$gradle_root/.gradlew-libdovi-wsl.XXXXXXXX")"
tr -d '\r' < "$gradle_root/gradlew" > "$gradle_launcher"
chmod +x "$gradle_launcher"
"$gradle_launcher" \
    --no-daemon \
    --max-workers=1 \
    -p "$repo" \
    publishLocalArtifacts

validate_outputs
echo "Saved libdovi AAR, Maven repository, and native SDK under $repo/OUTPUT"
