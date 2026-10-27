# MKV Episode Splitter

A C++20 command-line tool for splitting multi-episode MKV files into separate
files without re-encoding. It can use chapters, black transitions, audio silence,
a time range, or fixed duration and size targets.

Features include automatic chapter grouping, variable chapter counts per episode,
intro/outro bumper handling, season/episode filenames, and preservation of audio
track titles, language metadata, and disposition flags. An interactive terminal
mode shows a proposed split plan before you start.

## Build

Requires CMake 3.20+, a C++20 compiler, pkg-config, and FFmpeg development libraries
(`libavformat`, `libavcodec`, `libavutil`, and `libavfilter`). Tests also require the
`ffmpeg` executable with the `libx264` encoder.

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential cmake pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Linux is the currently verified environment. The launcher scripts require Bash;
other platforms have not been verified. To build without tests, configure with
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

## License

MIT; see [LICENSE](LICENSE). FFmpeg is a separate dependency with its own license
terms, which depend on how it was built.
