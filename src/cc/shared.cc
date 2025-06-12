#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#include "load-spz.h"

enum spz_error_code : std::uint8_t {
  SPZ_SUCCESS = 0,
  SPZ_ERROR_INVALID_PARAMS = 1,
  SPZ_ERROR_MEMORY_ALLOCATION = 2,
  SPZ_ERROR_COMPRESSION_FAILED = 3,
  SPZ_ERROR_DECOMPRESSION_FAILED = 4,
  SPZ_ERROR_INTERNAL = 5
};

extern "C" {

static inline bool is_valid_compression_level(int level) {
  return (level >= 1 && level <= 22);
}

static inline int internal_compress_spz(const uint8_t *input, int inputSize,
                                        int compressionLevel, int workers,
                                        uint8_t **outputPtr, int *outputSize) {
  if (!input || !outputPtr || !outputSize || inputSize <= 0) {
    return SPZ_ERROR_INVALID_PARAMS;
  }

  if (!is_valid_compression_level(compressionLevel)) {
    return SPZ_ERROR_INVALID_PARAMS;
  }

  try {
    std::vector<uint8_t> compressedData;
    compressedData.reserve(static_cast<size_t>(inputSize));
    std::span<const uint8_t> inSpan(input, static_cast<size_t>(inputSize));
    if (!spz::compress(inSpan, compressionLevel, workers, compressedData)) {
      return SPZ_ERROR_COMPRESSION_FAILED;
    }
    size_t compSize = compressedData.size();
    if (compSize == 0) {
      return SPZ_ERROR_COMPRESSION_FAILED;
    }
    uint8_t *buffer = reinterpret_cast<uint8_t *>(std::malloc(compSize));
    if (!buffer) {
      return SPZ_ERROR_MEMORY_ALLOCATION;
    }

    std::memcpy(buffer, compressedData.data(), compSize);

    *outputPtr = buffer;
    *outputSize = static_cast<int>(compSize);
    return SPZ_SUCCESS;
  } catch (const std::bad_alloc &) {
    return SPZ_ERROR_MEMORY_ALLOCATION;
  } catch (...) {
    return SPZ_ERROR_INTERNAL;
  }
}

#ifdef __EMSCRIPTEN__
EXPORT int compress_spz(const uint8_t *input, int inputSize,
                        int compressionLevel, uint8_t **outputPtr,
                        int *outputSize) {
  int workers = 1;
  return internal_compress_spz(input, inputSize, compressionLevel, workers,
                               outputPtr, outputSize);
}
#else
EXPORT int compress_spz(const uint8_t *input, int inputSize,
                        int compressionLevel, int workers, uint8_t **outputPtr,
                        int *outputSize) {
  if (workers <= 0) {
    workers = 1;
  }
  return internal_compress_spz(input, inputSize, compressionLevel, workers,
                               outputPtr, outputSize);
}
#endif

EXPORT int decompress_spz(const uint8_t *input, int inputSize,
                          int includeNormals, uint8_t **outputPtr,
                          int *outputSize) {
  if (!input || !outputPtr || !outputSize || inputSize <= 0) {
    return SPZ_ERROR_INVALID_PARAMS;
  }

  try {
    std::vector<uint8_t> decompressedData;
    bool normalsFlag = (includeNormals != 0);
    std::span<const uint8_t> inSpan(input, static_cast<size_t>(inputSize));
    if (!spz::decompress(inSpan, normalsFlag, decompressedData)) {
      return SPZ_ERROR_DECOMPRESSION_FAILED;
    }

    size_t decompSize = decompressedData.size();

    uint8_t *buffer = reinterpret_cast<uint8_t *>(std::malloc(decompSize));
    if (!buffer) {
      return SPZ_ERROR_MEMORY_ALLOCATION;
    }

    std::memcpy(buffer, decompressedData.data(), decompSize);
    *outputPtr = buffer;
    *outputSize = static_cast<int>(decompSize);
    return SPZ_SUCCESS;
  } catch (const std::bad_alloc &) {
    return SPZ_ERROR_MEMORY_ALLOCATION;
  } catch (...) {
    return SPZ_ERROR_INTERNAL;
  }
}

EXPORT const char *get_error_string_spz(int errorCode) {
  switch (errorCode) {
  case SPZ_SUCCESS:
    return "Success";
  case SPZ_ERROR_INVALID_PARAMS:
    return "Invalid parameters provided";
  case SPZ_ERROR_MEMORY_ALLOCATION:
    return "Memory allocation failed";
  case SPZ_ERROR_COMPRESSION_FAILED:
    return "Compression operation failed";
  case SPZ_ERROR_DECOMPRESSION_FAILED:
    return "Decompression operation failed";
  case SPZ_ERROR_INTERNAL:
    return "Internal error occurred";
  default:
    return "Unknown error";
  }
}

EXPORT void free_buffer_spz(uint8_t *buffer) {
  if (buffer) {
    std::free(buffer);
  }
}
} // EXTERN C
