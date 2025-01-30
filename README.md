## SPZ 404
This is a modified version of the [SPZ library](https://github.com/nianticlabs/spz), enhanced for better performance and usability.

## Features
- **Enhanced Compression**: The original zlib was replaced with ZSTD for faster compression and decompression speeds.
- **Build System**: Improved builds through tuned CMake configuration.
- **Console Utilities**: Command-line tool added for compressing and decompressing `.spz` files.
- **Improved Performance**: The default `march` target is set to `x86-64-v3`, along with various small fixes and optimizations for better performance.
- **Python Bindings**: Python bindings have been implemented using `pybind11`.

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

## File Format
The .spz format utilizes a zstd-compressed stream of data, consisting of a 16-byte header followed by the Gaussian data. This data is organized by attribute in the following order: positions, scales, rotations, alphas, colors, and spherical harmonics.

### Header

```c
struct PackedGaussiansHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t numPoints;
  uint8_t shDegree;
  uint8_t fractionalBits;
  uint8_t flags;
  uint8_t reserved;
};
```

All values are little-endian.

1. **magic**: This is always 0x5053474e
2. **version**: Currently, the only valid version is 2
3. **numPoints**: The number of gaussians
4. **shDegree**: The degree of spherical harmonics. This must be between 0 and 3 (inclusive).
5. **fractionalBits**: The number of bits used to store the fractional part of coordinates in
   the fixed-point encoding.
6. **flags**: A bit field containing flags.
   - `0x1`: whether the splat was trained with [antialiasing](https://niujinshuchong.github.io/mip-splatting/).
7. **reserved**: Reserved for future use. Must be 0.


### Positions

Positions are represented as `(x, y, z)` coordinates, each as a 24-bit fixed point signed integer.
The number of fractional bits is determined by the `fractionalBits` field in the header.

### Scales

Scales are represented as `(x, y, z)` components, each represented as an 8-bit log-encoded integer.

### Rotation

Rotations are represented as the `(x, y, z)` components of the normalized rotation quaternion. The
`w` component can be derived from the others and is not stored. Each components is encoded as an
8-bit signed integer.

### Alphas

Alphas are represented as 8-bit unsigned integers.

### Colors

Colors are stored as `(r, g, b)` values, where each color component is represented as an
unsigned 8-bit integer.

### Spherical Harmonics

Depending on the degree of spherical harmonics for the splat, this can contain 0 (for degree 0),
9 (for degree 1), 24 (for degree 2), or 45 (for degree 3) coefficients per gaussian.

The coefficients for a gaussian are organized such that the color channel is the inner (faster
varying) axis, and the coefficient is the outer (slower varying) axis, i.e. for degree 1,
the order of the 9 values is:
```
sh1n1_r, sh1n1_g, sh1n1_b, sh10_r, sh10_g, sh10_b, sh1p1_r, sh1p1_g, sh1p1_b
```

Each coefficient is represented as an 8-bit signed integer. Additional quantization can be performed
to attain a higher compression ratio. This library currently uses 5 bits of precision for degree 0
and 4 bits of precision for degrees 1 and 2, but this may be changed in the future without breaking
backwards compatibility.

## Installation for Python Environment
```bash
git clone https://github.com/404-Repo/spz
cd spz
pip install .
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
