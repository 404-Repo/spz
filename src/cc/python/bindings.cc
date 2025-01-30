#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>

#include <span>
#include <vector>
#include <stdexcept>

#include <load-spz.h>

namespace py = pybind11;

PYBIND11_MODULE(pyspz, m) {
    m.doc() = "Python bindings for SPZ compression/decompression";

    m.def("compress",
          [](const py::buffer &rawData, int compressionLevel = 1, int workers = 1) {
              py::buffer_info buf = rawData.request();
              if (buf.format != py::format_descriptor<uint8_t>::format()) {
                  throw std::runtime_error("Expected buffer of type uint8");
              }

              const std::span<const uint8_t> data(
                  reinterpret_cast<const uint8_t*>(buf.ptr),
                  buf.size
              );

              std::vector<uint8_t> compressedData;
              bool success = spz::compress(data, compressionLevel, workers, compressedData);
              if (!success) {
                  throw std::runtime_error("Compression failed");
              }

              return py::bytes{
                  reinterpret_cast<const char*>(compressedData.data()),
                  compressedData.size()
              };
          },
          py::arg("raw_data"),
          py::arg("compression_level") = 1,
          py::arg("workers") = 1,
          "Compress PLY raw data with the specified compression level and number of workers."
    );


    m.def("decompress",
          [](const py::buffer &input, bool includeNormals) {
              py::buffer_info buf = input.request();
              if (buf.format != py::format_descriptor<uint8_t>::format()) {
                  throw std::runtime_error("Expected buffer of type uint8");
              }

              const std::span<const uint8_t> data(
                  reinterpret_cast<const uint8_t*>(buf.ptr),
                  buf.size
              );

              std::vector<uint8_t> decompressedData;
              bool success = spz::decompress(data, includeNormals, decompressedData);
              if (!success) {
                  throw std::runtime_error("Decompression failed");
              }

              return py::bytes{
                  reinterpret_cast<const char*>(decompressedData.data()),
                  decompressedData.size()
              };
          },
          py::arg("input"),
          py::arg("include_normals"),
          "Decompress input data. If include_normals is True, normals are included into the output."
    );
}
