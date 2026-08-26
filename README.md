# libdovi-android

Reusable Android AAR and native SDK packaging for the checked-out
[`quietvoid/dovi_tool`](https://github.com/quietvoid/dovi_tool) source.

## Upstream source resolution

Each complete native build clones the latest `quietvoid/dovi_tool` `main` once
into a validated temporary directory, so it requires network access. The
single resolved SHA is reused for every Android ABI and is recorded in
`OUTPUT/native/<abi>/UPSTREAM_SHA`. The temporary clone is removed on success,
failure, or interruption.

## Build outputs

Build all supported Android ABIs from that one resolved upstream checkout:

```bash
./scripts/build-native.sh
```

On Windows, run the complete build and publication through WSL:

```bat
rebuild-libdovi-wsl.bat
```

The wrapper checks the WSL tools, Rust Android targets, Android SDK, and pinned
NDK before building. It then publishes the AAR, Maven repository, and native
SDK under `OUTPUT` using the parent Jellyfin Android TV Gradle wrapper.

Native files are generated under `OUTPUT/native/<abi>`. Running
`publishLocalArtifacts` publishes
`io.github.thor2002ro:libdovi-android:<VERSION_NAME>` under `OUTPUT/maven` and
copies the JNI library, public header, and pkg-config file into
`OUTPUT/sdk/<abi>`. Generated archives and SDK output are not committed.
The stable public C contract is installed as `include/dovi.h`; upstream
libdovi numeric conversion modes are not exposed through that contract.

The shared object in the release AAR and the corresponding
`OUTPUT/sdk/<abi>/lib/libjellyfin_dovi.so` must have identical hashes.
