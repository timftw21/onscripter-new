# Project status

This file records the maintained build and platform state. Release-by-release
history belongs in Git, issues, and release notes rather than this document.

## Supported release targets

- Current release: 1.3 (`20260824-new`).
- Windows 10 or newer, x86-64, built in MSYS2 UCRT64.
- Android 11 or newer (API 30), arm64. Exercised on 15 and 16; see the
  device-coverage note in `Resources/Docs/Android.md` for what the floor has and
  has not been tested against.
- The SDL3/SDL3_GPU renderer is the only supported renderer.

The obsolete SDL2-based Xcode project has been removed so it can no longer
produce misleading builds. The configure-based macOS path is best-effort until
a current SDL3 build is exercised in CI.

## Windows prerequisites

Install MSYS2, open its UCRT64 shell, and install:

```sh
pacman -S --needed base-devel \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-nasm \
  mingw-w64-ucrt-x86_64-pkgconf
```

Then build from the repository root:

```sh
./configure --release-build --strip-binary --std=gnu++23
make -j8
```

`configure` downloads source archives for the pinned dependency graph, verifies
their SHA-256 hashes, and builds static libraries below `DerivedData`.

## Android builds

The Android engine is built with NDK 29 through `configure` and GNU Make, then
staged into the Gradle application. Gradle rejects missing native binaries and
preserves their original timestamps so C/C++ changes cannot be hidden by a
newer staging copy. Windows NDK extraction requires `bsdtar` or 7-Zip because
Info-ZIP can corrupt executable line endings on that host. The Java/native
startup contract, scoped-storage layout, and Android Studio workflow are
documented in `Resources/Docs/Android.md`.

The launcher supports user-selected game folders and preserves the release-1.2
app-scoped save location for upgrades. Selected roots use the engine's
`ONScripter-RU/SaveData/<game id>` layout; folder changes restart the native
engine in a clean process. Android lifecycle recovery suspends presentation
while the surface is absent and retries swapchain rebinding until it succeeds.
The Android canvas is derived only from the live window surface, so rotation and
multi-window changes cannot be overwritten by whole-display geometry.
Vulkan device creation requests only the baseline features the renderer uses,
avoiding device-specific failures on GPUs without SDL's optional clip-distance,
depth-clamping, indirect-first-instance, or anisotropy features.
Background lifecycle signals now suspend presentation and idle the main loop,
and low-memory signals evict rebuildable CPU caches and unused pooled GPU
images on the render thread. The opt-in `perf-overlay` reports frame pacing,
CPU, resident memory, and image-pool usage. Android touch input uses the
engine's published wait context for backlog-only controls, including precise
two-finger scrolling, momentum, and scrollbar-arrow paging.
Host-side Android tests lock down the selected-root and legacy app-scoped path
mapping.

## Other host builds

A C++23 compiler, GNU Make, CMake, Meson, Ninja, NASM, `pkg-config`, and normal
POSIX development tools are required:

```sh
./configure --release-build --std=gnu++23
make -j8
```

Cross-compilation options are listed by `./configure --help`. Platform support
should only be claimed after a clean build and runtime smoke test on that target.

## Dependency policy

Active dependencies are pinned in `Dependencies/pkgs` with cryptographic hashes.
The aggregate package stamp depends on every active recipe and patch, so a
recipe change forces version evaluation instead of silently retaining an old
static library. Configure copies only recipes and tooling into build trees;
downloaded sources and installed libraries stay target-local so native and
cross-compiled archives cannot contaminate one another. Android package stamps
also include the ABI, API floor, and NDK wrapper version, so changing the
toolchain cannot silently reuse incompatible static archives. Dependency
archives use immutable upstream release assets and remain hash-verified so an
HTML error response cannot enter a build. Patch releases should be reviewed
monthly and security fixes expedited. Major upgrades of FFmpeg, Lua, or SDL
still require game-data and media regression testing. The current audited lines
are FFmpeg 8.1, Lua 5.5, and SDL3; retired SDL2 recipes were
removed.

## Verification

The repository has a CMake/CTest native test project and Windows build/test CI.
Standalone tests cover archive indexes, command-line validation, serialized
state, regular expressions, and multidimensional variables, including
zero-index transient array references. Their assertions remain enabled in
Release configurations so checks and setup expressions cannot be compiled out.
Linux CI runs those tests under
ASan/UBSan and runs four libFuzzer targets on pushes and a larger weekly budget.
The CMake harness supplies the same host-platform macros as the production
configure path, and source hygiene checks the complete changed range rather
than a shallow checkout snapshot. SLRE operator-length parsing and character-set
matching are bounded by the supplied expression and input sizes, including
malformed trailing escapes and empty inputs exercised by the regression suite
and fuzzer. Static analysis remains part of the release audit.

The synthetic compressed-script fixture verifies public-release startup and
orderly shutdown without copyrighted assets. It is not a representative game
regression suite. A release is not fully verified until it has:

1. passed a clean Windows UCRT64 build;
2. passed compiler warnings and static analysis;
3. passed the native tests and sanitizer/fuzz CI;
4. started with legal Umineko Project data;
5. exercised saves, text, audio, video, menus, and archive loading; and
6. completed the built-in SDL3 benchmark on representative hardware.

That missing deterministic game corpus is the main limit on aggressive renderer
or ownership refactors and on claims of maximal optimization.
