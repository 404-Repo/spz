## SPZ 404
This is a modified version of the [SPZ library](https://github.com/nianticlabs/spz), enhanced for better performance and usability.

## Features
- **Enhanced Compression**: The original zlib was replaced with ZSTD for faster compression and decompression speeds.
- **Build System**: Improved builds through tuned CMake configuration.
- **Console Utilities**: Command-line tool added for compressing and decompressing `.spz` files.
- **Improved Performance**: The default `march` target is set to `x86-64-v3`, along with various small fixes and optimizations for better performance.
- **Python Bindings**: Python bindings have been implemented using `pybind11`.
- **WASM Support**: WebAssembly support has been added to run compression and decompression in a web environment.

## Recommendations and Good to Know
In my tests for this project, Clang 19.1.7 generated significantly better code than GCC 14.2.1. I therefore recommend using Clang.\
The resulting file hash after decompression by SPZ will differ if the library is compiled with -O3 -march=x86-64-v3 versus using just -O2 or -O3, due to differences in instruction generation, floating-point operation handling, and compiler-specific optimizations.\
Furthermore, even when compiling with identical flags (e.g., -march=x86-64-v3), the file hash will still differ between GCC and Clang builds for the same reasons.

## Installation for Python Environment
To compile the project, you need a **C++20 compiler** such as Clang or GCC, along with CMake version 3.29.
```bash
git clone https://github.com/404-Repo/spz
cd spz
pip install .
```
If you prefer compiled package, you can download and install the WHL package from the [release page](https://github.com/404-Repo/spz/releases).

## WASM
The project includes a WebAssembly module demo, which you can find in [src/html/index.html](src/html/index.html).<br>
A full SPZ WASM example can be downloaded from the [release page](https://github.com/404-Repo/spz/releases).

## C++ Build shared libraries and console utilities
You can use [Ninja generator](https://ninja-build.org/) to speed up the build.
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build . -- -j$(nproc)
```

## C++ Interface
```C
std::vector<uint8_t> compress(const std::vector<uint8_t> &rawData, int compressionLevel);
std::vector<uint8_t> decompress(const std::vector<uint8_t> &input, bool includeNormals);
```

## Python Interface
```Python
def compress(raw_data: bytes, compression_level: int = 1, workers: int = 1) -> bytes:
    """
    Compresses the provided raw data.

    :param raw_data: Data to compress as a bytes object.
    :param compression_level: Level of compression (default is 1).
    :param workers: Number of worker threads to use (default is 1).
    :return: Compressed data as a bytes object.
    :raises RuntimeError: If compression fails.
    """

def decompress(input_data: bytes, include_normals: bool) -> bytes:
    """
    Decompresses the provided input data.

    :param input_data: Compressed data as a bytes object.
    :param include_normals: Whether to include normals in the decompressed data.
    :return: Decompressed data as a bytes object.
    :raises RuntimeError: If decompression fails.
    """
```

## Python example

```Python
import pyspz
import os
import time

def compress_ply(input_ply_path, compressed_path, compression_level=1, int workers=1):
    """
    Compresses a PLY file and saves the compressed data.
    """
    with open(input_ply_path, 'rb') as f:
        raw_data = f.read()

    start_time = time.perf_counter()
    compressed_data = pyspz.compress(raw_data, compression_level, workers)
    end_time = time.perf_counter()

    with open(compressed_path, 'wb') as f:
        f.write(compressed_data)

    print(f"Compressed {len(raw_data)} bytes to {len(compressed_data)} bytes "
          f"in {end_time - start_time:.2f} ms.")
    return compressed_data

def decompress_ply(compressed_path, decompressed_ply_path, include_normals=True):
    """
    Decompresses a file and saves the output as a PLY file.
    """
    with open(compressed_path, 'rb') as f:
        compressed_data = f.read()

    start_time = time.perf_counter()
    decompressed_data = pyspz.decompress(compressed_data, include_normals)
    end_time = time.perf_counter()

    with open(decompressed_ply_path, 'wb') as f:
        f.write(decompressed_data)

    print(f"Decompressed {len(compressed_data)} bytes to {len(decompressed_data)} bytes "
          f"in {end_time - start_time:.2f} ms.")
    return decompressed_data

def main():
    # File paths (update these paths as needed)
    input_ply = 'input.ply'
    compressed_file = 'compressed.spz'
    decompressed_ply = 'decompressed.ply'

    if not os.path.isfile(input_ply):
        print(f"Input file '{input_ply}' not found.")
        return

    compression_level = 3
    workers = 3
    compress_ply(input_ply, compressed_file, compression_level, workers)
    decompress_ply(compressed_file, decompressed_ply, include_normals=False)

if __name__ == "__main__":
    main()
```
