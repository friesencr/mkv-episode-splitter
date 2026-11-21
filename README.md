# MKV Episode Splitter

A C++20 command-line tool for splitting multi-episode MKV files into separate
files without re-encoding. It can use chapters, black transitions, audio silence,
a time range, or fixed duration and size targets.

Features include automatic chapter grouping, variable chapter counts per episode,
intro/outro bumper handling, season/episode filenames, and preservation of audio
track titles, language metadata, and disposition flags. An interactive terminal
mode shows a proposed split plan before you start.

## Alpha downloads

Download the executable for your machine and `SHA256SUMS` from the same
[alpha release](https://github.com/friesencr/mkv-episode-splitter/releases).
Each executable includes FFmpeg.

| Platform | Executable |
| --- | --- |
| Linux x86-64 | `mkvsplit-cli-linux-x86_64` |
| Linux ARM64 | `mkvsplit-cli-linux-arm64` |
| macOS 15 or later on Apple Silicon | `mkvsplit-cli-macos-arm64` |
| Windows x64 | `mkvsplit-cli-windows-x86_64.exe` |

On Linux, run `sha256sum --check --ignore-missing SHA256SUMS`. On macOS, run
`shasum -a 256 mkvsplit-cli-macos-arm64` and compare the digest with its entry
in `SHA256SUMS`. Then make the downloaded file executable and run it. For example:

```sh
chmod +x mkvsplit-cli-macos-arm64
./mkvsplit-cli-macos-arm64 --help
```

On Windows, compare the PowerShell hash with the executable's entry in
`SHA256SUMS`, then run the CLI:

```powershell
Get-FileHash .\mkvsplit-cli-windows-x86_64.exe -Algorithm SHA256
.\mkvsplit-cli-windows-x86_64.exe --help
```

The macOS alpha uses an ad-hoc signature. The Windows alpha is unsigned.
For alpha feedback, include the expected and actual episode boundaries, audio
and subtitle track details, and any playback timing issues.

## Build

Requires CMake 3.20+, a C++20 compiler, pkg-config, and FFmpeg development libraries
(`libavformat`, `libavcodec`, and `libavutil`). Tests also require the
`ffmpeg` executable with the `libx264` encoder. Unix CLI tests use `ffprobe`
to compare packet timestamps and contents.

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential cmake pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The desktop launcher scripts target Linux. To build without tests, configure with
`-DBUILD_TESTING=OFF`.

## Usage

```sh
# Open the interactive configuration and preview
./build/mkvsplit-cli

# Inspect tracks and chapters without splitting
./build/mkvsplit-cli -i "episodes.mkv" --info

# Automatically choose a splitting strategy
./build/mkvsplit-cli -i "Show - S01E01E02E03.mkv" -m auto -o ./output

# Group four chapters into each episode, exporting bumpers separately
./build/mkvsplit-cli -i "episodes.mkv" -m chapters -g 4 \
  --lead-bumpers 1 --tail-bumpers 1 --bumpers separate

# Split into ten-minute chunks
./build/mkvsplit-cli -i "video.mkv" -m duration -d 600

# Extract a time range
./build/mkvsplit-cli -i "video.mkv" -m range -s 00:05:00 -e 00:15:00

./build/mkvsplit-cli --help
```

Output defaults to `./output`, relative to the current working directory. Use a
fresh output directory for each run to avoid colliding with existing filenames.

For a Linux desktop launcher, build into `build/`, then run:

```sh
bash install_desktop_launcher.sh
```

This installs a user-local desktop entry pointing to this checkout. Reinstall it
if you move the checkout. The launcher opens the interactive terminal mode and
accepts an MKV file from the desktop file manager.

## Limitations

Automatic episode detection is heuristic. Check the proposed plan, especially for
bonus features, unusual credits, and longer finales. Nearby black-frame checks
provide evidence for chapter boundaries; they do not move the chapter cuts.
Video scans can take time, and an expected episode count is not a guarantee that
all boundaries can be found.

Splitting copies compressed packets. Video starts depend on available keyframes,
so arbitrary requested times are not guaranteed to produce frame-exact cuts.
Size mode produces approximate sizes.

## Development

Tests generate synthetic media in `test_run_dir/` under their working directory;
no sample videos are required. CTest runs them in the build directory. GitHub
Actions builds and runs the suite in both Debug and Release configurations.

## Standalone release build

Extract the release asset ending in `-source.tar.gz`, or clone the repository
and run `git submodule update --init --depth 1`. The release source asset
includes the pinned FFmpeg source in `third_party/ffmpeg`. From the source
root on an x86-64 or ARM64 Linux host with Docker and BuildKit:

```sh
docker build -f packaging/Dockerfile --output type=local,dest=dist .
```

This produces a static executable for the host architecture, runs the tests, and
splits a fixture in an empty container. Both Linux targets use musl.

On Apple Silicon with Xcode Command Line Tools and Homebrew:

```sh
brew install cmake ninja pkgconf make ffmpeg
bash packaging/build-desktop.sh macos-arm64
```

To cross-build Windows x64 on Ubuntu 24.04:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config nasm curl \
  g++-mingw-w64-x86-64-posix libz-mingw-w64-dev
bash packaging/build-desktop.sh windows-x86_64
```

Each recipe writes its executable to `dist/`. The desktop recipes download and
checksum the libaom source for AV1 decoding. To relink a modified FFmpeg, edit
`third_party/ffmpeg` and rerun the recipe for your platform. License notices
accompany each release.

Dependabot checks the action revisions, Linux build image, and FFmpeg maintenance
branch weekly. To publish, push a tag matching the CMake version, such as
`v0.1.0-alpha.2`. GitHub Actions runs the packaged CLI on each target before
publishing, then downloads the public binaries and repeats the splits. It checks
the source and binary checksums and compares compressed video packets with the
original MKV. Alpha, beta, and rc tags publish as prereleases.

## License

MIT; see [LICENSE](LICENSE). FFmpeg is a separate dependency with its own license
terms, which depend on how it was built.
