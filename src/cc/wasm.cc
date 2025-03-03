#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#include "load-spz.h"

extern "C" {

EXPORT int compress_spz(const uint8_t *input, int inputSize,
                        int compressionLevel, uint8_t **outputPtr,
                        int *outputSize) {
  if (!input || inputSize <= 0 || !outputPtr || !outputSize)
    return 1;

  std::vector<uint8_t> compressedData;
  std::span<const uint8_t> inSpan(input, inputSize);

  if (!spz::compress(inSpan, compressionLevel, 1, compressedData))
    return 2;

  size_t compSize = compressedData.size();
  *outputSize = static_cast<int>(compSize);
  *outputPtr = reinterpret_cast<uint8_t *>(std::malloc(compSize));
  if (!*outputPtr)
    return 3;

  std::memcpy(*outputPtr, compressedData.data(), compSize);
  return 0;
}

EXPORT int decompress_spz(const uint8_t *input, int inputSize,
                          int includeNormals, uint8_t **outputPtr,
                          int *outputSize) {
  if (!input || inputSize <= 0 || !outputPtr || !outputSize)
    return 1;

  std::vector<uint8_t> decompressedData;
  bool normalsFlag = (includeNormals != 0);
  std::span<const uint8_t> inSpan(input, inputSize);

  if (!spz::decompress(inSpan, normalsFlag, decompressedData))
    return 2;

  size_t decompSize = decompressedData.size();
  *outputSize = static_cast<int>(decompSize);
  *outputPtr = reinterpret_cast<uint8_t *>(std::malloc(decompSize));
  if (!*outputPtr)
    return 3;

  std::memcpy(*outputPtr, decompressedData.data(), decompSize);
  return 0;
}
}
