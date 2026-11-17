# zstd (decompression only)

`lib/common` and `lib/decompress` of Zstandard 1.5.7
(https://github.com/facebook/zstd), unmodified, BSD licence (`LICENSE`).
Used to read X4 Pro (full profile) XWA archives, whose text chunks and line
art are zstd frames (see `lib/Xwa`). The ESP32-C3 builds never call it, so
the linker leaves it out of their images.

To update: copy the same files from a newer release's `lib/` tree.
