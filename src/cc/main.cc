#include "cc/splat-types.h"
#include "load-spz.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct ProgramOptions {
  bool compress = false;
  bool decompress = false;
  std::string inputFilename;
  std::string outputFilename;
  int compressionLevel = 3;
  bool includeNormals = false;
  std::string programName;
  int workers = 3;
};

void printUsage(const std::string &programName) {
  std::cout
      << "Usage:\n"
      << "  " << programName
      << " -e|-d -i <input_file> -o <output_file> [-c <compression_level>] [-w "
         "<workers>] [-n]\n\n"
      << "Options:\n"
      << "  -e                     Compress the input file to SPZ format.\n"
      << "  -d                     Decompress the input SPZ file to PLY "
         "format.\n"
      << "  -i <input_file>        Specify the input file.\n"
      << "  -o <output_file>       Specify the output file.\n"
      << "  -c <compression_level> (Optional) Specify the compression level "
         "(1-22). Default is 3.\n"
      << "  -w <workers>           (Optional) Specify the number of worker "
         "threads. Default is the number of hardware threads.\n"
      << "  -n, -normals           (Optional, Decompress only) Include normals "
         "from the output PLY file.\n"
      << "  -h, --help             Show this help message.\n";
}

bool handleError(const std::string &message, const std::string &programName) {
  std::cerr << "Error: " << message << "\n";
  printUsage(programName);
  return false;
}

bool parseArguments(int argc, char *argv[], ProgramOptions &options) {
  if (argc == 0) {
    return handleError("No arguments provided.", "program");
  }

  options.programName = argv[0];

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-e") {
      options.compress = true;
    } else if (arg == "-d") {
      options.decompress = true;
    } else if (arg == "-i" && i + 1 < argc) {
      options.inputFilename = argv[++i];
    } else if (arg == "-o" && i + 1 < argc) {
      options.outputFilename = argv[++i];
    } else if (arg == "-c" && i + 1 < argc) {
      options.compressionLevel = std::atoi(argv[++i]);
      if (options.compressionLevel < 1 || options.compressionLevel > 22) {
        return handleError("Compression level must be between 1 and 22.",
                           options.programName);
      }
    } else if (arg == "-w" && i + 1 < argc) {
      options.workers = std::atoi(argv[++i]);
      if (options.workers < 1) {
        return handleError("Number of workers must be at least 1.",
                           options.programName);
      }
    } else if (arg == "-n" || arg == "-normals") {
      options.includeNormals = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage(options.programName);
      exit(0);
    } else {
      return handleError("Unknown or incomplete option: " + arg,
                         options.programName);
    }
  }

  if (options.compress && options.decompress) {
    return handleError(
        "Specify either -e (compress) or -d (decompress), but not both.",
        options.programName);
  }
  if (!options.compress && !options.decompress) {
    return handleError("Specify either -e (compress) or -d (decompress).",
                       options.programName);
  }
  if (options.inputFilename.empty()) {
    return handleError("Input file not specified. Use -i <input_file>.",
                       options.programName);
  }
  if (options.outputFilename.empty()) {
    return handleError("Output file not specified. Use -o <output_file>.",
                       options.programName);
  }

  return true;
}

int compressFile(const ProgramOptions &options) {
  std::cout << "Compressing " << options.inputFilename << " to "
            << options.outputFilename << " with compression level "
            << options.compressionLevel << " using " << options.workers
            << " worker(s)...\n";

  if (!fs::exists(options.inputFilename)) {
    std::cerr << "Input file does not exist: " << options.inputFilename << "\n";
    return 1;
  }

  uintmax_t inputSize = fs::file_size(options.inputFilename);
  std::cout << "Input file size: " << inputSize << " bytes.\n";

  // Load Gaussian cloud from PLY file
  spz::GaussianCloud cloud = spz::loadSplatFromPly(options.inputFilename);
  if (cloud.numPoints == 0) {
    std::cerr << "Failed to load Gaussian cloud from " << options.inputFilename
              << "\n";
    return 1;
  }
  std::cout << "Loaded Gaussian cloud from " << options.inputFilename
            << " with " << cloud.numPoints << " points.\n";

  auto start = std::chrono::high_resolution_clock::now();
  std::vector<uint8_t> data;

  if (!spz::saveSpz(cloud, &data, options.compressionLevel, options.workers)) {
    std::cerr << "Failed to encode Gaussian cloud to SPZ format\n";
    return 1;
  }

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::high_resolution_clock::now() - start)
                      .count();
  std::cout << "Compression completed in " << duration << " ms.\n";

  // Write compressed data to output file
  std::ofstream outFile(options.outputFilename, std::ios::binary);
  if (!outFile) {
    std::cerr << "Failed to open output file: " << options.outputFilename
              << "\n";
    return 1;
  }
  outFile.write(reinterpret_cast<const char *>(data.data()), data.size());
  if (!outFile) {
    std::cerr << "Failed to write data to output file: "
              << options.outputFilename << "\n";
    return 1;
  }

  std::cout << "Compressed data saved to " << options.outputFilename << "\n";

  // Calculate and display size reduction
  double sizeReduction =
      inputSize > 0
          ? (static_cast<double>(inputSize - data.size()) / inputSize) * 100.0
          : 0.0;
  double compressionFactor =
      inputSize > 0 ? static_cast<double>(inputSize) / data.size() : 0.0;

  std::cout << std::fixed << std::setprecision(2)
            << "Size reduction: " << sizeReduction << "%\n"
            << "Compressed file is " << compressionFactor
            << " times smaller than the original.\n";

  return 0;
}

int decompressFile(const ProgramOptions &options) {
  std::cout << "Decompressing " << options.inputFilename << " to "
            << options.outputFilename;
  if (options.includeNormals) {
    std::cout << " with normals\n";
  } else {
    std::cout << " without normals\n";
  }

  if (!fs::exists(options.inputFilename)) {
    std::cerr << "Input file does not exist: " << options.inputFilename << "\n";
    return 1;
  }

  std::ifstream inFile(options.inputFilename, std::ios::binary);
  if (!inFile) {
    std::cerr << "Failed to open input file: " << options.inputFilename << "\n";
    return 1;
  }

  std::vector<uint8_t> data((std::istreambuf_iterator<char>(inFile)),
                            std::istreambuf_iterator<char>());
  inFile.close();

  auto start = std::chrono::high_resolution_clock::now();
  spz::GaussianCloud cloud = spz::loadSpz(data);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::high_resolution_clock::now() - start)
                      .count();
  std::cout << "Decompression completed in " << duration << " ms.\n";

  if (cloud.numPoints == 0) {
    std::cerr << "Failed to decompress SPZ file or empty Gaussian cloud.\n";
    return 1;
  }

  if (spz::saveSplatToPly(cloud, options.outputFilename,
                          options.includeNormals)) {
    std::cout << "Successfully saved Gaussian cloud to "
              << options.outputFilename << "\n";
  } else {
    std::cerr << "Failed to save Gaussian cloud to " << options.outputFilename
              << "\n";
    return 1;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  ProgramOptions options;

  if (!parseArguments(argc, argv, options)) {
    return 1;
  }

  try {
    return options.compress ? compressFile(options) : decompressFile(options);
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }
}
