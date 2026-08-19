# ZXing DataMatrix Scanner

A native camera workstation for reading multiple GS1 DataMatrix codes, validating a box, and sending the aggregation result to a local WMS API.

The application uses OpenCV for camera capture, ZXing-C++ for barcode decoding, ImGui/GLFW for the operator interface, and a small HTTP/JSON client for backend communication.

## Features

- live camera preview with selectable camera index;
- detection and de-duplication of multiple DataMatrix codes;
- GS1 payload normalization;
- automatic and manual aggregation modes;
- order information and aggregation status from a local API;
- test-mode aggregation and reset flow;
- keyboard shortcuts for camera selection and scan reset.

## Architecture

The scanner is intentionally a single native executable. `main.cpp` owns capture, decoding, UI state and API calls; ImGui is pinned as a Git submodule. The application expects a compatible service at `http://127.0.0.1:8000` with the camera aggregation endpoints used in the source.

## Requirements

The verified development target is macOS with:

- a C++17 compiler;
- CMake 3.16 or newer;
- OpenCV;
- ZXing-C++ 2.3 or network access for CMake to fetch the pinned source;
- GLFW;
- pkg-config;
- camera permission for the terminal or built executable.

Install dependencies with Homebrew:

```bash
brew install cmake opencv zxing-cpp glfw pkg-config
```

If ZXing-C++ 2.3 is not installed, CMake downloads and builds the pinned `v2.3.0` source automatically. This avoids silently compiling against older system packages with an incompatible API.

## Checkout

Clone with the pinned ImGui dependency:

```bash
git clone --recurse-submodules https://github.com/kirillbelykh/zxing_datamatrix.git
cd zxing_datamatrix
```

For an existing clone:

```bash
git submodule update --init --recursive
```

## Build and run

```bash
cmake -S . -B build
cmake --build build
./build/scanner
```

The backend should be running before an aggregation is sent. The camera preview can still start without it, but API-dependent actions will report an error.

## Operator controls

- `0`–`9`: switch camera index;
- `C`: clear the current scan;
- `Space`: reset the current aggregation state;
- `Enter`: send a ready box in manual mode;
- `Q`: close the application.

## Repository layout

| Path | Responsibility |
|---|---|
| `main.cpp` | Scanner, UI and backend integration |
| `imgui/` | Pinned ImGui submodule |
| `httplib.h`, `json.hpp` | Header-only HTTP and JSON dependencies |
| `scripts/` | Convenience installers/build scripts for macOS and Windows experiments |
| `CMakeLists.txt` | Reproducible primary build |

## Limitations

- macOS is the currently verified platform; the Windows helper is experimental;
- camera handling currently uses the AVFoundation capture backend;
- the backend URL and device identifier are compiled into the application;
- no automated camera test is provided because successful decoding requires real hardware and printed codes.
