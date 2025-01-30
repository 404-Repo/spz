#include "load-spz.h"

#include <zstd.h>

#ifdef ANDROID
#include <android/log.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace spz {

class MemBuf : public std::streambuf {
public:
  MemBuf(const uint8_t *begin, const uint8_t *end) {
    char *pBegin = const_cast<char *>(reinterpret_cast<const char *>(begin));
    char *pEnd = const_cast<char *>(reinterpret_cast<const char *>(end));
    this->setg(pBegin, pBegin, pEnd);
  }
};

namespace {

#ifdef SPZ_ENABLE_LOGS
#ifdef ANDROID
static constexpr char LOG_TAG[] = "SPZ";
template <class... Args> static void SpzLog(const char *fmt, Args &&...args) {
  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, fmt,
                      std::forward<Args>(args)...);
}
#else
template <class... Args> static void SpzLog(const char *fmt, Args &&...args) {
  printf(fmt, std::forward<Args>(args)...);
  printf("\n");
  fflush(stdout);
}
#endif // ANDROID

template <class... Args> static void SpzLog(const char *fmt) {
  SpzLog("%s", fmt);
}
#else
// No-op versions when logging is disabled
template <class... Args> static void SpzLog(const char *fmt, Args &&...args) {}

template <class... Args> static void SpzLog(const char *fmt) {}
#endif // SPZ_ENABLE_LOGS

// Scale factor for DC color components. To convert to RGB, we should multiply
// by 0.282, but it can be useful to represent base colors that are out of range
// if the higher spherical harmonics bands bring them back into range so we
// multiply by a smaller value.
constexpr float colorScale = 0.15f;

int degreeForDim(int dim) {
  if (dim < 3)
    return 0;
  if (dim < 8)
    return 1;
  if (dim < 15)
    return 2;
  return 3;
}

int dimForDegree(int degree) {
  switch (degree) {
  case 0:
    return 0;
  case 1:
    return 3;
  case 2:
    return 8;
  case 3:
    return 15;
  default:
    SpzLog("[SPZ: ERROR] Unsupported SH degree: %d\n", degree);
    return 0;
  }
}

uint8_t toUint8(float x) {
  return static_cast<uint8_t>(std::clamp(std::round(x), 0.0f, 255.0f));
}

// Quantizes to 8 bits, the round to nearest bucket center. 0 always maps to a
// bucket center.
uint8_t quantizeSH(float x, int bucketSize) {
  int q = static_cast<int>(std::round(x * 128.0f) + 128.0f);
  q = (q + bucketSize / 2) / bucketSize * bucketSize;
  return static_cast<uint8_t>(std::clamp(q, 0, 255));
}

float unquantizeSH(uint8_t x) {
  return (static_cast<float>(x) - 128.0f) / 128.0f;
}

float sigmoid(float x) { return 1 / (1 + std::exp(-x)); }

float invSigmoid(float x) { return std::log(x / (1.0f - x)); }

template <typename T> size_t countBytes(const std::vector<T> &vec) {
  return vec.size() * sizeof(vec[0]);
}

#define CHECK(x)                                                               \
  {                                                                            \
    if (!(x)) {                                                                \
      SpzLog("[SPZ: ERROR] Check failed: %s:%d: %s", __FILE__, __LINE__, #x);  \
      return false;                                                            \
    }                                                                          \
  }

#define CHECK_GE(x, y) CHECK((x) >= (y))
#define CHECK_LE(x, y) CHECK((x) <= (y))
#define CHECK_EQ(x, y) CHECK((x) == (y))

bool checkSizes(const GaussianCloud &g) {
  CHECK_GE(g.numPoints, 0);
  CHECK_GE(g.shDegree, 0);
  CHECK_LE(g.shDegree, 3);
  CHECK_EQ(g.positions.size(), g.numPoints * 3);
  CHECK_EQ(g.scales.size(), g.numPoints * 3);
  CHECK_EQ(g.rotations.size(), g.numPoints * 4);
  CHECK_EQ(g.alphas.size(), g.numPoints);
  CHECK_EQ(g.colors.size(), g.numPoints * 3);
  CHECK_EQ(g.sh.size(), g.numPoints * dimForDegree(g.shDegree) * 3);
  return true;
}

bool checkSizes(const PackedGaussians &packed, int numPoints, int shDim,
                bool usesFloat16) {
  CHECK_EQ(packed.positions.size(), numPoints * 3 * (usesFloat16 ? 2 : 3));
  CHECK_EQ(packed.scales.size(), numPoints * 3);
  CHECK_EQ(packed.rotations.size(), numPoints * 3);
  CHECK_EQ(packed.alphas.size(), numPoints);
  CHECK_EQ(packed.colors.size(), numPoints * 3);
  CHECK_EQ(packed.sh.size(), numPoints * shDim * 3);
  return true;
}

constexpr uint8_t FlagAntialiased = 0x1;

struct PackedGaussiansHeader {
  uint32_t magic = 0x5053474e; // NGSP = Niantic gaussian splat
  uint32_t version = 2;
  uint32_t numPoints = 0;
  uint8_t shDegree = 0;
  uint8_t fractionalBits = 0;
  uint8_t flags = 0;
  uint8_t reserved = 0;
};

bool decompressZstd(const uint8_t *compressed, size_t size,
                    std::vector<uint8_t> *out) {
  // Estimate decompressed size
  size_t const decompressedSize = ZSTD_getFrameContentSize(
      reinterpret_cast<const void *>(compressed), size);
  if (decompressedSize == ZSTD_CONTENTSIZE_ERROR) {
    SpzLog("[SPZ: ERROR] Not a valid Zstd frame.");
    return false;
  }
  if (decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
    // Fallback to a heuristic or a maximum size
    SpzLog("[SPZ: WARNING] Decompressed size unknown, using a heuristic.");
    // ~ allocate 6x the compressed size
    out->resize(size * 6);
    size_t const ret =
        ZSTD_decompress(out->data(), out->size(), compressed, size);
    if (ZSTD_isError(ret)) {
      SpzLog("[SPZ: ERROR] Zstd decompression error: %s",
             ZSTD_getErrorName(ret));
      return false;
    }
    out->resize(ret);
    return true;
  } else {
    out->resize(decompressedSize);
    size_t const ret =
        ZSTD_decompress(out->data(), out->size(), compressed, size);
    if (ZSTD_isError(ret)) {
      SpzLog("[SPZ: ERROR] Zstd decompression error: %s",
             ZSTD_getErrorName(ret));
      return false;
    }
    return ret == decompressedSize;
  }
}

bool compressZstd(const uint8_t *data, size_t size, std::vector<uint8_t> *out,
                  int compressionLevel = 1, int workers = 1) {
  constexpr auto ZSTD_MIN_COMPRESSION_LEVEL = 1;
  size_t const bound = ZSTD_compressBound(size);
  out->resize(bound);

  // Ensure compressionLevel is at least 1
  int effectiveCompressionLevel =
      std::max(compressionLevel, ZSTD_MIN_COMPRESSION_LEVEL);

  ZSTD_CCtx *cctx = ZSTD_createCCtx();
  if (!cctx) {
    SpzLog("[SPZ: ERROR] Failed to create ZSTD compression context");
    return false;
  }

  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel,
                         effectiveCompressionLevel);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers);

  size_t const compressedSize =
      ZSTD_compress2(cctx, out->data(), bound, data, size);
  ZSTD_freeCCtx(cctx);

  if (ZSTD_isError(compressedSize)) {
    SpzLog("[SPZ: ERROR] Zstd compression error: %s",
           ZSTD_getErrorName(compressedSize));
    return false;
  }
  out->resize(compressedSize);
  return true;
}

} // namespace

PackedGaussians packGaussians(const GaussianCloud &g) {
  if (!checkSizes(g)) {
    return {};
  }
  const int numPoints = g.numPoints;
  const int shDim = dimForDegree(g.shDegree);

  // Use 12 bits for the fractional part of coordinates (~0.25 millimeter
  // resolution). In the future we can use different values on a per-splat basis
  // and still be compatible with the decoder.
  PackedGaussians packed = {
      .numPoints = g.numPoints,
      .shDegree = g.shDegree,
      .fractionalBits = 12,
      .antialiased = g.antialiased,
  };
  packed.positions.resize(numPoints * 3 * 3);
  packed.scales.resize(numPoints * 3);
  packed.rotations.resize(numPoints * 3);
  packed.alphas.resize(numPoints);
  packed.colors.resize(numPoints * 3);
  packed.sh.resize(numPoints * shDim * 3);

  // Store coordinates as 24-bit fixed point values.
  const float scale = (1 << packed.fractionalBits);
  for (size_t i = 0; i < numPoints * 3; i++) {
    const int32_t fixed32 =
        static_cast<int32_t>(std::round(g.positions[i] * scale));
    packed.positions[i * 3 + 0] = fixed32 & 0xff;
    packed.positions[i * 3 + 1] = (fixed32 >> 8) & 0xff;
    packed.positions[i * 3 + 2] = (fixed32 >> 16) & 0xff;
  }

  for (size_t i = 0; i < numPoints * 3; i++) {
    packed.scales[i] = toUint8((g.scales[i] + 10.0f) * 16.0f);
  }

  for (size_t i = 0; i < numPoints; i++) {
    // Normalize the quaternion, make w positive, then store xyz. w can be
    // derived from xyz. NOTE: These are already in xyzw order.
    Quat4f q = normalized(quat4f(&g.rotations[i * 4]));
    q = times(q, (q[3] < 0 ? -127.5f : 127.5f));
    q = plus(q, Quat4f{127.5f, 127.5f, 127.5f, 127.5f});
    packed.rotations[i * 3 + 0] = toUint8(q[0]);
    packed.rotations[i * 3 + 1] = toUint8(q[1]);
    packed.rotations[i * 3 + 2] = toUint8(q[2]);
  }

  for (size_t i = 0; i < numPoints; i++) {
    // Apply sigmoid activation to alpha
    packed.alphas[i] = toUint8(sigmoid(g.alphas[i]) * 255.0f);
  }

  for (size_t i = 0; i < numPoints * 3; i++) {
    // Convert SH DC component to wide RGB (allowing values that are a bit above
    // 1 and below 0).
    packed.colors[i] =
        toUint8(g.colors[i] * (colorScale * 255.0f) + (0.5f * 255.0f));
  }

  if (g.shDegree > 0) {
    // Spherical harmonics quantization parameters. The data format uses 8 bits
    // per coefficient, but when packing, we can quantize to fewer bits for
    // better compression.
    constexpr int sh1Bits = 5;
    constexpr int shRestBits = 4;
    const int shPerPoint = dimForDegree(g.shDegree) * 3;
    for (size_t i = 0; i < numPoints * shPerPoint; i += shPerPoint) {
      size_t j = 0;
      for (; j < 9; j++) { // There are 9 coefficients for degree 1
        packed.sh[i + j] = quantizeSH(g.sh[i + j], 1 << (8 - sh1Bits));
      }
      for (; j < shPerPoint; j++) {
        packed.sh[i + j] = quantizeSH(g.sh[i + j], 1 << (8 - shRestBits));
      }
    }
  }

  return packed;
}

UnpackedGaussian PackedGaussian::unpack(bool usesFloat16,
                                        int fractionalBits) const {
  UnpackedGaussian result;
  if (usesFloat16) {
    // Decode legacy float16 format. We can remove this at some point as it was
    // never released.
    const auto *halfData = reinterpret_cast<const Half *>(position.data());
    for (size_t i = 0; i < 3; i++) {
      result.position[i] = halfToFloat(halfData[i]);
    }
  } else {
    // Decode 24-bit fixed point coordinates
    float scale = 1.0 / (1 << fractionalBits);
    for (size_t i = 0; i < 3; i++) {
      int32_t fixed32 = position[i * 3 + 0];
      fixed32 |= position[i * 3 + 1] << 8;
      fixed32 |= position[i * 3 + 2] << 16;
      fixed32 |= (fixed32 & 0x800000) ? 0xff000000 : 0; // sign extension
      result.position[i] = static_cast<float>(fixed32) * scale;
    }
  }

  for (size_t i = 0; i < 3; i++) {
    result.scale[i] = (scale[i] / 16.0f - 10.0f);
  }

  const uint8_t *r = &rotation[0];
  Vec3f xyz =
      plus(times(Vec3f{static_cast<float>(r[0]), static_cast<float>(r[1]),
                       static_cast<float>(r[2])},
                 1.0f / 127.5f),
           Vec3f{-1, -1, -1});
  std::copy(xyz.data(), xyz.data() + 3, &result.rotation[0]);
  // Compute the real component - we know the quaternion is normalized and w is
  // non-negative
  result.rotation[3] = std::sqrt(std::max(0.0f, 1.0f - squaredNorm(xyz)));

  result.alpha = invSigmoid(alpha / 255.0f);

  for (size_t i = 0; i < 3; i++) {
    result.color[i] = ((color[i] / 255.0f) - 0.5f) / colorScale;
  }

  for (size_t i = 0; i < 15; i++) {
    result.shR[i] = unquantizeSH(shR[i]);
    result.shG[i] = unquantizeSH(shG[i]);
    result.shB[i] = unquantizeSH(shB[i]);
  }

  return result;
}

PackedGaussian PackedGaussians::at(int i) const {
  PackedGaussian result;
  int positionBits = usesFloat16() ? 6 : 9;
  int start3 = i * 3;
  const auto *p = &positions[i * positionBits];
  std::copy(p, p + positionBits, result.position.data());
  std::copy(&scales[start3], &scales[start3 + 3], result.scale.data());
  std::copy(&rotations[start3], &rotations[start3 + 3], result.rotation.data());
  std::copy(&colors[start3], &colors[start3 + 3], result.color.data());
  result.alpha = alphas[i];

  int shDim = dimForDegree(shDegree);
  const auto *sh = &this->sh[i * shDim * 3];
  for (int j = 0; j < shDim; ++j, sh += 3) {
    result.shR[j] = sh[0];
    result.shG[j] = sh[1];
    result.shB[j] = sh[2];
  }
  for (int j = shDim; j < 15; ++j) {
    result.shR[j] = 128;
    result.shG[j] = 128;
    result.shB[j] = 128;
  }

  return result;
}

UnpackedGaussian PackedGaussians::unpack(int i) const {
  return at(i).unpack(usesFloat16(), fractionalBits);
}

bool PackedGaussians::usesFloat16() const {
  return positions.size() == numPoints * 3 * 2;
}

GaussianCloud unpackGaussians(const PackedGaussians &packed) {
  const int numPoints = packed.numPoints;
  const int shDim = dimForDegree(packed.shDegree);
  const bool usesFloat16 = packed.usesFloat16();
  if (!checkSizes(packed, numPoints, shDim, usesFloat16)) {
    return {};
  }

  GaussianCloud result = {
      .numPoints = packed.numPoints,
      .shDegree = packed.shDegree,
      .antialiased = packed.antialiased,
  };
  result.positions.resize(numPoints * 3);
  result.scales.resize(numPoints * 3);
  result.rotations.resize(numPoints * 4);
  result.alphas.resize(numPoints);
  result.colors.resize(numPoints * 3);
  result.sh.resize(numPoints * shDim * 3);

  if (usesFloat16) {
    // Decode legacy float16 format. We can remove this at some point as it was
    // never released.
    const auto *halfData =
        reinterpret_cast<const Half *>(packed.positions.data());
    for (size_t i = 0; i < numPoints * 3; i++) {
      result.positions[i] = halfToFloat(halfData[i]);
    }
  } else {
    // Decode 24-bit fixed point coordinates
    float scale = 1.0 / (1 << packed.fractionalBits);
    for (size_t i = 0; i < numPoints * 3; i++) {
      int32_t fixed32 = packed.positions[i * 3 + 0];
      fixed32 |= packed.positions[i * 3 + 1] << 8;
      fixed32 |= packed.positions[i * 3 + 2] << 16;
      fixed32 |= (fixed32 & 0x800000) ? 0xff000000 : 0; // sign extension
      result.positions[i] = static_cast<float>(fixed32) * scale;
    }
  }

  for (size_t i = 0; i < numPoints * 3; i++) {
    result.scales[i] = packed.scales[i] / 16.0f - 10.0f;
  }

  for (size_t i = 0; i < numPoints; i++) {
    const uint8_t *r = &packed.rotations[i * 3];
    Vec3f xyz =
        plus(times(Vec3f{static_cast<float>(r[0]), static_cast<float>(r[1]),
                         static_cast<float>(r[2])},
                   1.0f / 127.5f),
             Vec3f{-1, -1, -1});
    std::copy(xyz.data(), xyz.data() + 3, &result.rotations[i * 4]);
    // Compute the real component - we know the quaternion is normalized and w
    // is non-negative
    result.rotations[i * 4 + 3] =
        std::sqrt(std::max(0.0f, 1.0f - squaredNorm(xyz)));
  }

  for (size_t i = 0; i < numPoints; i++) {
    result.alphas[i] = invSigmoid(packed.alphas[i] / 255.0f);
  }

  for (size_t i = 0; i < numPoints * 3; i++) {
    result.colors[i] = ((packed.colors[i] / 255.0f) - 0.5f) / colorScale;
  }

  for (size_t i = 0; i < packed.sh.size(); i++) {
    result.sh[i] = unquantizeSH(packed.sh[i]);
  }

  return result;
}

void serializePackedGaussians(const PackedGaussians &packed,
                              std::ostream &out) {
  PackedGaussiansHeader header = {
      .numPoints = static_cast<uint32_t>(packed.numPoints),
      .shDegree = static_cast<uint8_t>(packed.shDegree),
      .fractionalBits = static_cast<uint8_t>(packed.fractionalBits),
      .flags = static_cast<uint8_t>(packed.antialiased ? FlagAntialiased : 0),
  };
  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  out.write(reinterpret_cast<const char *>(packed.positions.data()),
            countBytes(packed.positions));
  out.write(reinterpret_cast<const char *>(packed.alphas.data()),
            countBytes(packed.alphas));
  out.write(reinterpret_cast<const char *>(packed.colors.data()),
            countBytes(packed.colors));
  out.write(reinterpret_cast<const char *>(packed.scales.data()),
            countBytes(packed.scales));
  out.write(reinterpret_cast<const char *>(packed.rotations.data()),
            countBytes(packed.rotations));
  out.write(reinterpret_cast<const char *>(packed.sh.data()),
            countBytes(packed.sh));
}

PackedGaussians deserializePackedGaussians(std::istream &in) {
  constexpr int maxPointsToRead = 10000000;

  PackedGaussiansHeader header;
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!in || header.magic != PackedGaussiansHeader().magic) {
    SpzLog("[SPZ ERROR] deserializePackedGaussians: header not found");
    return {};
  }
  if (header.version < 1 || header.version > 2) {
    SpzLog("[SPZ ERROR] deserializePackedGaussians: version not supported: %d",
           header.version);
    return {};
  }
  if (header.numPoints > maxPointsToRead) {
    SpzLog("[SPZ ERROR] deserializePackedGaussians: Too many points: %d",
           header.numPoints);
    return {};
  }
  if (header.shDegree > 3) {
    SpzLog("[SPZ ERROR] deserializePackedGaussians: Unsupported SH degree: %d",
           header.shDegree);
    return {};
  }
  const int numPoints = header.numPoints;
  const int shDim = dimForDegree(header.shDegree);
  const bool usesFloat16 = header.version == 1;
  PackedGaussians result = {.numPoints = numPoints,
                            .shDegree = header.shDegree,
                            .fractionalBits = header.fractionalBits,
                            .antialiased =
                                (header.flags & FlagAntialiased) != 0};
  result.positions.resize(numPoints * 3 * (usesFloat16 ? 2 : 3));
  result.scales.resize(numPoints * 3);
  result.rotations.resize(numPoints * 3);
  result.alphas.resize(numPoints);
  result.colors.resize(numPoints * 3);
  result.sh.resize(numPoints * shDim * 3);
  in.read(reinterpret_cast<char *>(result.positions.data()),
          countBytes(result.positions));
  in.read(reinterpret_cast<char *>(result.alphas.data()),
          countBytes(result.alphas));
  in.read(reinterpret_cast<char *>(result.colors.data()),
          countBytes(result.colors));
  in.read(reinterpret_cast<char *>(result.scales.data()),
          countBytes(result.scales));
  in.read(reinterpret_cast<char *>(result.rotations.data()),
          countBytes(result.rotations));
  in.read(reinterpret_cast<char *>(result.sh.data()), countBytes(result.sh));
  if (!in) {
    SpzLog("[SPZ ERROR] deserializePackedGaussians: read error");
    return {};
  }
  return result;
}

bool saveSpz(const GaussianCloud &g, std::vector<uint8_t> *out,
             int compressionLevel = 1, int workers = 1) {
  std::string data;
  {
    PackedGaussians packed = packGaussians(g);
    std::stringstream ss;
    serializePackedGaussians(packed, ss);
    data = ss.str();
  }
  return compressZstd(reinterpret_cast<const uint8_t *>(data.data()),
                      data.size(), out, compressionLevel);
}

PackedGaussians loadSpzPacked(const uint8_t *data, int size) {
  std::vector<uint8_t> decompressed;

  if (!decompressZstd(data, size, &decompressed)) {
    return {};
  }

  if (decompressed.empty()) {
    return {};
  }

  MemBuf memBuffer(decompressed.data(),
                   decompressed.data() + decompressed.size());
  std::istream stream(&memBuffer);

  return deserializePackedGaussians(stream);
}

PackedGaussians loadSpzPacked(const std::vector<uint8_t> &data) {
  return loadSpzPacked(data.data(), static_cast<int>(data.size()));
}

PackedGaussians loadSpzPacked(const std::string &filename) {
  std::ifstream in(filename, std::ios::binary | std::ios::ate);
  if (!in.good())
    return {};
  std::vector<uint8_t> data(in.tellg());
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char *>(data.data()), data.size());
  if (!in.good())
    return {};
  return loadSpzPacked(data);
}

GaussianCloud loadSpz(const std::vector<uint8_t> &data) {
  return unpackGaussians(loadSpzPacked(data));
}

bool saveSpz(const GaussianCloud &g, const std::string &filename,
             int compressionLevel = 1, int workers = 1) {
  std::vector<uint8_t> data;
  if (!saveSpz(g, &data, compressionLevel, workers))
    return false;
  std::ofstream out(filename, std::ios::binary | std::ios::out);
  out.write(reinterpret_cast<const char *>(data.data()), data.size());
  out.close();
  return out.good();
}

GaussianCloud loadSpz(const std::string &filename) {
  std::ifstream in(filename, std::ios::binary | std::ios::ate);
  if (!in.good()) {
    SpzLog("[SPZ ERROR] Unable to open: %s", filename.c_str());
    return {};
  }
  std::vector<uint8_t> data(in.tellg());
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char *>(data.data()), data.size());
  in.close();
  if (!in.good()) {
    SpzLog("[SPZ ERROR] Unable to load data from: %s", filename.c_str());
    return {};
  }
  return loadSpz(data);
}

GaussianCloud parseSplatFromStream(std::istream &in) {
  GaussianCloud result = {};

  if (!in.good()) {
    SpzLog("[SPZ ERROR] Unable to read from input stream.");
    return result;
  }

  std::string line;
  std::getline(in, line);
  if (line != "ply") {
    SpzLog("[SPZ ERROR] Input data is not a .ply file.");
    return result;
  }

  std::getline(in, line);
  if (line != "format binary_little_endian 1.0") {
    SpzLog("[SPZ ERROR] Unsupported .ply format.");
    return result;
  }

  std::getline(in, line);
  if (line.find("element vertex ") != 0) {
    SpzLog("[SPZ ERROR] Missing vertex count.");
    return result;
  }

  int numPoints = 0;
  try {
    numPoints = std::stoi(line.substr(std::strlen("element vertex ")));
  } catch (const std::exception &e) {
    SpzLog("[SPZ ERROR] Invalid vertex count.");
    return result;
  }

  if (numPoints <= 0 || numPoints > 10 * 1024 * 1024) {
    SpzLog("[SPZ ERROR] Invalid vertex count: %d", numPoints);
    return result;
  }

  SpzLog("[SPZ] Loading %d points", numPoints);
  std::unordered_map<std::string, int> fields; // name -> index
  for (int i = 0;; i++) {
    if (!std::getline(in, line)) {
      SpzLog("[SPZ ERROR] Unexpected end of header.");
      return result;
    }

    if (line == "end_header")
      break;

    if (line.find("property float ") != 0) {
      SpzLog("[SPZ ERROR] Unsupported property data type: %s", line.c_str());
      return result;
    }
    std::string name = line.substr(std::strlen("property float "));
    fields[name] = i;
  }

  // Returns the index for a given field name, ensuring the name exists.
  const auto index = [&fields](const std::string &name) {
    const auto &itr = fields.find(name);
    if (itr == fields.end()) {
      SpzLog("[SPZ ERROR] Missing field: %s", name.c_str());
      return -1;
    }
    return itr->second;
  };

  const std::vector<int> positionIdx = {index("x"), index("y"), index("z")};
  const std::vector<int> scaleIdx = {index("scale_0"), index("scale_1"),
                                     index("scale_2")};
  const std::vector<int> rotIdx = {index("rot_1"), index("rot_2"),
                                   index("rot_3"), index("rot_0")};
  const std::vector<int> alphaIdx = {index("opacity")};
  const std::vector<int> colorIdx = {index("f_dc_0"), index("f_dc_1"),
                                     index("f_dc_2")};

  // Check that only valid indices were returned.
  auto checkIndices = [&](const std::vector<int> &idxVec) -> bool {
    for (auto idx : idxVec) {
      if (idx < 0) {
        return false;
      }
    }
    return true;
  };

  if (!checkIndices(positionIdx) || !checkIndices(scaleIdx) ||
      !checkIndices(rotIdx) || !checkIndices(alphaIdx) ||
      !checkIndices(colorIdx)) {
    return result;
  }

  // Spherical harmonics are optional and variable in size (depending on degree)
  std::vector<int> shIdx;
  for (int i = 0; i < 45; i++) {
    const auto &itr = fields.find("f_rest_" + std::to_string(i));
    if (itr == fields.end())
      break;
    shIdx.push_back(itr->second);
  }
  const int shDim = static_cast<int>(shIdx.size() / 3);

  // If spherical harmonics fields are present, ensure they are complete
  if (shIdx.size() % 3 != 0) {
    SpzLog("[SPZ ERROR] Incomplete spherical harmonics fields.");
    return result;
  }

  std::vector<float> values;
  values.resize(numPoints * fields.size());

  in.read(reinterpret_cast<char *>(values.data()),
          values.size() * sizeof(float));
  if (!in.good()) {
    SpzLog("[SPZ ERROR] Unable to load data from input stream.");
    return result;
  }

  result.numPoints = numPoints;
  result.shDegree = degreeForDim(shDim);
  result.positions.reserve(numPoints * 3);
  result.scales.reserve(numPoints * 3);
  result.rotations.reserve(numPoints * 4);
  result.alphas.reserve(numPoints * 1);
  result.colors.reserve(numPoints * 3);
  result.sh.reserve(numPoints * shDim * 3);

  for (size_t i = 0; i < static_cast<size_t>(numPoints); i++) {
    size_t vertexOffset = i * fields.size();

    // Position
    result.positions.push_back(values[vertexOffset + positionIdx[0]]);
    result.positions.push_back(values[vertexOffset + positionIdx[1]]);
    result.positions.push_back(values[vertexOffset + positionIdx[2]]);

    // Scale
    result.scales.push_back(values[vertexOffset + scaleIdx[0]]);
    result.scales.push_back(values[vertexOffset + scaleIdx[1]]);
    result.scales.push_back(values[vertexOffset + scaleIdx[2]]);

    // Rotation
    result.rotations.push_back(values[vertexOffset + rotIdx[0]]);
    result.rotations.push_back(values[vertexOffset + rotIdx[1]]);
    result.rotations.push_back(values[vertexOffset + rotIdx[2]]);
    result.rotations.push_back(values[vertexOffset + rotIdx[3]]);

    // Alpha
    result.alphas.push_back(values[vertexOffset + alphaIdx[0]]);

    // Color
    result.colors.push_back(values[vertexOffset + colorIdx[0]]);
    result.colors.push_back(values[vertexOffset + colorIdx[1]]);
    result.colors.push_back(values[vertexOffset + colorIdx[2]]);

    // Spherical Harmonics
    for (int j = 0; j < shDim; j++) {
      result.sh.push_back(values[vertexOffset + shIdx[j]]);
      result.sh.push_back(values[vertexOffset + shIdx[j + shDim]]);
      result.sh.push_back(values[vertexOffset + shIdx[j + 2 * shDim]]);
    }
  }

  return result;
}

GaussianCloud loadSplatFromPly(const std::string &filename) {
  SpzLog("[SPZ] Loading: %s", filename.c_str());
  std::ifstream in(filename, std::ios::binary);
  if (!in.is_open()) {
    SpzLog("[SPZ ERROR] Unable to open: %s", filename.c_str());
    return {};
  }

  GaussianCloud cloud = parseSplatFromStream(in);
  in.close();
  return cloud;
}

GaussianCloud loadSplatFromMemory(const std::vector<uint8_t> &buffer) {
  if (buffer.empty()) {
    SpzLog("[SPZ ERROR] Memory buffer is empty.");
    return {};
  }

  MemBuf memBuf(buffer.data(), buffer.data() + buffer.size());

  std::istream inStream(&memBuf);
  GaussianCloud cloud = parseSplatFromStream(inStream);
  return cloud;
}

bool saveSplatToPly(const GaussianCloud &data, const std::string &filename,
                    bool includeNormals = true) {
  const int N = data.numPoints;
  CHECK_EQ(data.positions.size(), N * 3);
  CHECK_EQ(data.scales.size(), N * 3);
  CHECK_EQ(data.rotations.size(), N * 4);
  CHECK_EQ(data.alphas.size(), N);
  CHECK_EQ(data.colors.size(), N * 3);
  const int shDim = static_cast<int>(data.sh.size() / N / 3);

  // Calculate the number of properties per vertex
  // Base properties: x, y, z
  // Optional normals: nx, ny, nz
  // Color: f_dc_0, f_dc_1, f_dc_2
  // SH coefficients: shDim * 3
  // Additional properties: opacity, scale_0, scale_1, scale_2, rot_0, rot_1,
  // rot_2, rot_3
  int baseProperties = 3;
  int normalsProperties = includeNormals ? 3 : 0;
  int colorProperties = 3;
  int shProperties = shDim * 3;
  int additionalProperties = 1 + 3 + 4; // opacity + scales + rotations
  int D = baseProperties + normalsProperties + colorProperties + shProperties +
          additionalProperties;

  std::vector<float> values(N * D, 0.0f);
  int outIdx = 0, i3 = 0, i4 = 0;
  for (int i = 0; i < N; i++) {
    // Position (x, y, z)
    values[outIdx++] = data.positions[i3 + 0];
    values[outIdx++] = data.positions[i3 + 1];
    values[outIdx++] = data.positions[i3 + 2];

    // Normals (nx, ny, nz): conditionally include them as zero
    if (includeNormals) {
      values[outIdx++] = 0.0f; // nx
      values[outIdx++] = 0.0f; // ny
      values[outIdx++] = 0.0f; // nz
    }

    // Color (r, g, b): DC component for spherical harmonics
    values[outIdx++] = data.colors[i3 + 0];
    values[outIdx++] = data.colors[i3 + 1];
    values[outIdx++] = data.colors[i3 + 2];

    // Spherical harmonics: Interleave so the coefficients are the
    // fastest-changing axis and the channel (r, g, b) is slower-changing axis.
    for (int j = 0; j < shDim; j++) {
      values[outIdx++] = data.sh[(i * shDim + j) * 3];
    }
    for (int j = 0; j < shDim; j++) {
      values[outIdx++] = data.sh[(i * shDim + j) * 3 + 1];
    }
    for (int j = 0; j < shDim; j++) {
      values[outIdx++] = data.sh[(i * shDim + j) * 3 + 2];
    }

    // Alpha
    values[outIdx++] = data.alphas[i];

    // Scale (sx, sy, sz)
    values[outIdx++] = data.scales[i3 + 0];
    values[outIdx++] = data.scales[i3 + 1];
    values[outIdx++] = data.scales[i3 + 2];

    // Rotation (qw, qx, qy, qz)
    values[outIdx++] = data.rotations[i4 + 3];
    values[outIdx++] = data.rotations[i4 + 0];
    values[outIdx++] = data.rotations[i4 + 1];
    values[outIdx++] = data.rotations[i4 + 2];

    i3 += 3;
    i4 += 4;
  }
  CHECK_EQ(outIdx, static_cast<int>(values.size()));

  std::ofstream out(filename, std::ios::binary);
  if (!out.good()) {
    SpzLog("[SPZ ERROR] Unable to open for writing: %s", filename.c_str());
    return false;
  }

  out << "ply\n";
  out << "format binary_little_endian 1.0\n";
  out << "element vertex " << N << "\n";
  out << "property float x\n";
  out << "property float y\n";
  out << "property float z\n";

  if (includeNormals) {
    out << "property float nx\n";
    out << "property float ny\n";
    out << "property float nz\n";
  }

  out << "property float f_dc_0\n";
  out << "property float f_dc_1\n";
  out << "property float f_dc_2\n";
  for (int i = 0; i < shDim * 3; i++) {
    out << "property float f_rest_" << i << "\n";
  }

  out << "property float opacity\n";
  out << "property float scale_0\n";
  out << "property float scale_1\n";
  out << "property float scale_2\n";
  out << "property float rot_0\n";
  out << "property float rot_1\n";
  out << "property float rot_2\n";
  out << "property float rot_3\n";
  out << "end_header\n";
  out.write(reinterpret_cast<char *>(values.data()),
            values.size() * sizeof(float));
  out.close();

  if (!out.good()) {
    SpzLog("[SPZ ERROR] Failed to write to: %s", filename.c_str());
    return false;
  }

  return true;
}

static std::vector<uint8_t> cloudToByteBuffer(const GaussianCloud &cloud,
                                              bool includeNormals = false) {
  const int N = cloud.numPoints;
  // The spherical harmonic dimension (number of SH coefficients per channel).
  const int shDim = static_cast<int>(cloud.sh.size() / N / 3);

  // For each point, we store:
  //   3 floats (positions)
  // + optionally 3 floats (normals)
  // + 3 floats (color)
  // + (shDim * 3) floats (SH)
  // + 1 float (opacity)
  // + 3 floats (scale)
  // + 4 floats (rotation)

  const int baseProperties = 3;                        // x,y,z
  const int normalProperties = includeNormals ? 3 : 0; // nx,ny,nz
  const int colorProperties = 3;                       // f_dc_0, f_dc_1, f_dc_2
  const int shProperties = shDim * 3; // shDim for R, shDim for G, shDim for B
  const int alphaProperties = 1;      // opacity
  const int scaleProperties = 3;      // scale_0, scale_1, scale_2
  const int rotationProperties = 4;   // rot_0..rot_3 (qw, qx, qy, qz)

  const int floatsPerPoint = baseProperties + normalProperties +
                             colorProperties + shProperties + alphaProperties +
                             scaleProperties + rotationProperties;
  const size_t totalFloats = static_cast<size_t>(N) * floatsPerPoint;

  std::vector<uint8_t> out;
  out.resize(totalFloats * sizeof(float));

  auto *ptr = reinterpret_cast<float *>(out.data());

  int i3 = 0; // index for positions/scales/colors in sets of 3
  int i4 = 0; // index for rotations in sets of 4

  for (int i = 0; i < N; i++) {
    // 1) positions: x, y, z
    *ptr++ = cloud.positions[i3 + 0];
    *ptr++ = cloud.positions[i3 + 1];
    *ptr++ = cloud.positions[i3 + 2];

    // 2) optional normals: nx=0, ny=0, nz=0
    if (includeNormals) {
      *ptr++ = 0.0f;
      *ptr++ = 0.0f;
      *ptr++ = 0.0f;
    }

    // 3) color: f_dc_0, f_dc_1, f_dc_2
    *ptr++ = cloud.colors[i3 + 0];
    *ptr++ = cloud.colors[i3 + 1];
    *ptr++ = cloud.colors[i3 + 2];

    for (int sh_i = 0; sh_i < shDim; sh_i++) {
      *ptr++ = cloud.sh[(i * shDim + sh_i) * 3 + 0]; // R
    }
    for (int sh_i = 0; sh_i < shDim; sh_i++) {
      *ptr++ = cloud.sh[(i * shDim + sh_i) * 3 + 1]; // G
    }
    for (int sh_i = 0; sh_i < shDim; sh_i++) {
      *ptr++ = cloud.sh[(i * shDim + sh_i) * 3 + 2]; // B
    }

    // 5) alpha (opacity)
    *ptr++ = cloud.alphas[i];

    // 6) scales: scale_0, scale_1, scale_2
    *ptr++ = cloud.scales[i3 + 0];
    *ptr++ = cloud.scales[i3 + 1];
    *ptr++ = cloud.scales[i3 + 2];

    // 7) rotation: rot_0..rot_3 = (qw, qx, qy, qz)
    // saveSplatToPly does: rot_0=qw, rot_1=qx, rot_2=qy, rot_3=qz
    float qw = cloud.rotations[i4 + 3];
    float qx = cloud.rotations[i4 + 0];
    float qy = cloud.rotations[i4 + 1];
    float qz = cloud.rotations[i4 + 2];

    *ptr++ = qw;
    *ptr++ = qx;
    *ptr++ = qy;
    *ptr++ = qz;

    i3 += 3;
    i4 += 4;
  }

  return out;
}

bool compress(const std::span<const uint8_t> rawData, int compressionLevel, int workers,
              std::vector<uint8_t> &output) {
  MemBuf memBuf(rawData.data(), rawData.data() + rawData.size());
  std::istream inStream(&memBuf);

  GaussianCloud g = parseSplatFromStream(inStream);
  if (g.numPoints == 0) {
    SpzLog("[SPZ: ERROR] Parsed GaussianCloud is empty.");
    return false;
  }

  PackedGaussians packed = packGaussians(g);

  std::stringstream ss;
  serializePackedGaussians(packed, ss);
  const std::string uncompressed = ss.str();

  if (!compressZstd(reinterpret_cast<const uint8_t *>(uncompressed.data()),
                    uncompressed.size(), &output, compressionLevel, workers)) {
    SpzLog("[SPZ: ERROR] Zstd compression failed.");
    return false;
  }

  return true;
}

bool decompress(const std::span<const uint8_t> input, bool includeNormals,
                std::vector<uint8_t> &output) {
  std::vector<uint8_t> decompressed;
  if (!decompressZstd(input.data(), input.size(), &decompressed) ||
      decompressed.empty()) {
    return false;
  }

  MemBuf memBuffer(decompressed.data(),
                   decompressed.data() + decompressed.size());
  std::istream stream(&memBuffer);
  PackedGaussians packed = deserializePackedGaussians(stream);

  if (packed.numPoints == 0 && packed.positions.empty()) {
    return false;
  }

  GaussianCloud cloud = unpackGaussians(packed);

  std::vector<uint8_t> body = cloudToByteBuffer(cloud, includeNormals);

  std::string header;
  header.reserve(512);

  header += "ply\n";
  header += "format binary_little_endian 1.0\n";

  header += "element vertex ";
  header += std::to_string(cloud.numPoints);
  header += "\n";

  header += "property float x\n";
  header += "property float y\n";
  header += "property float z\n";

  if (includeNormals) {
    header += "property float nx\n";
    header += "property float ny\n";
    header += "property float nz\n";
  }

  header += "property float f_dc_0\n";
  header += "property float f_dc_1\n";
  header += "property float f_dc_2\n";

  int shDim = 0;
  if (cloud.numPoints > 0) {
    shDim = static_cast<int>(cloud.sh.size()) / (cloud.numPoints * 3);
  }
  for (int i = 0; i < shDim * 3; ++i) {
    header += "property float f_rest_";
    header += std::to_string(i);
    header += "\n";
  }

  header += "property float opacity\n"
            "property float scale_0\n"
            "property float scale_1\n"
            "property float scale_2\n"
            "property float rot_0\n"
            "property float rot_1\n"
            "property float rot_2\n"
            "property float rot_3\n"
            "end_header\n";

  output.resize(header.size() + body.size());
  std::memcpy(output.data(), header.data(), header.size());
  std::memcpy(output.data() + header.size(), body.data(), body.size());

  return true;
}

} // namespace spz
