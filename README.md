# Rawocado

Rawocado is a desktop RAW photo editor built with Dear ImGui, GLFW, and LibRaw.

## What It Does

- Loads RAW images (ARW, CR2, CR3, NEF, DNG, etc.)
- Real-time editing controls (light, color, effects, detail)
- Fast processing worker with viewport updates
- Export to PNG/JPG
- Bottom image strip with quick switching
- Per-image edit state while browsing imported images

## Status

Rawocado is a prototype. Features and behavior can change often.

## Build

This project is currently set up to build for Windows, using MinGW-w64 from WSL. If things go well it'll be multiplatform no problem though.

### Requirements

- `x86_64-w64-mingw32-g++-posix`
- `make`
- Vendor dependencies already present in `vendor/`

### Build Command

```bash
make -j4 build
```

Output:

- `RAWocado.exe`

## Project Layout

- `main.cpp` UI and app flow
- `processing.cpp` image pipeline, RAW decode, export, workers
- `processing.h` shared state and APIs
- `Makefile` build configuration