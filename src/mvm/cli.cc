#include "mvm/cli.hh"

#include "mvm/error.hh"
#include <cctype>
#include <cstdlib>
#include <limits>

namespace mvm {
namespace {

llvm::Expected<int> parseOptLevel(const std::string &value) {
    char *end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 3) {
        return makeError("invalid optimization level `" + value +
                         "`, expected 1-3\n");
    }
    if (parsed == 0) {
        return makeError("managed mode requires -O1 or higher; -O0 is not "
                         "supported\n");
    }
    return static_cast<int>(parsed);
}

llvm::Expected<std::uint64_t> parseHeapSize(const std::string &value) {
    if (value.empty()) {
        return makeError("invalid heap size ``, expected a positive byte size\n");
    }

    std::size_t numericLength = 0;
    while (numericLength < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[numericLength]))) {
        ++numericLength;
    }
    if (numericLength == 0) {
        return makeError("invalid heap size `" + value +
                         "`, expected a positive byte size\n");
    }

    char *end = nullptr;
    auto parsed = std::strtoull(value.substr(0, numericLength).c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0) {
        return makeError("invalid heap size `" + value +
                         "`, expected a positive byte size\n");
    }

    std::uint64_t multiplier = 1;
    auto suffix = value.substr(numericLength);
    if (!suffix.empty()) {
        if (suffix == "K" || suffix == "k") {
            multiplier = 1024ull;
        } else if (suffix == "M" || suffix == "m") {
            multiplier = 1024ull * 1024ull;
        } else if (suffix == "G" || suffix == "g") {
            multiplier = 1024ull * 1024ull * 1024ull;
        } else {
            return makeError("invalid heap size suffix in `" + value +
                             "`, expected K, M, or G\n");
        }
    }

    if (parsed > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return makeError("heap size `" + value + "` is too large\n");
    }

    return parsed * multiplier;
}

}  // namespace

llvm::Expected<Options> parseCommandLine(int argc, char **argv) {
    Options options;
    bool collectProgramArgs = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (collectProgramArgs) {
            options.programArgs.push_back(arg);
            continue;
        }

        if (arg == "--") {
            collectProgramArgs = true;
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
            return options;
        }

        if (arg == "--dump-ir") {
            options.dumpIR = true;
            continue;
        }

        if (arg == "--entry") {
            if (i + 1 >= argc) {
                return makeError("missing symbol after `--entry`\n");
            }
            options.entrySymbol = argv[++i];
            continue;
        }

        if (arg == "--heap-size" || arg == "-Xm") {
            if (i + 1 >= argc) {
                return makeError("missing value after `" + arg + "`\n");
            }
            auto heapSizeOrErr = parseHeapSize(argv[++i]);
            if (!heapSizeOrErr) {
                return heapSizeOrErr.takeError();
            }
            options.heapSizeBytes = *heapSizeOrErr;
            continue;
        }

        if (arg == "-O") {
            if (i + 1 >= argc) {
                return makeError("missing value after `-O`\n");
            }
            auto optLevelOrErr = parseOptLevel(argv[++i]);
            if (!optLevelOrErr) {
                return optLevelOrErr.takeError();
            }
            options.optLevel = *optLevelOrErr;
            continue;
        }

        if (arg.size() == 3 && arg[0] == '-' && arg[1] == 'O') {
            auto optLevelOrErr = parseOptLevel(arg.substr(2));
            if (!optLevelOrErr) {
                return optLevelOrErr.takeError();
            }
            options.optLevel = *optLevelOrErr;
            continue;
        }

        if (arg.rfind("-Xm", 0) == 0 && arg.size() > 3) {
            auto heapSizeOrErr = parseHeapSize(arg.substr(3));
            if (!heapSizeOrErr) {
                return heapSizeOrErr.takeError();
            }
            options.heapSizeBytes = *heapSizeOrErr;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            return makeError("unknown option `" + arg + "`\n");
        }

        if (!options.inputPath.empty()) {
            return makeError("multiple input files are not supported\n");
        }
        options.inputPath = arg;
    }

    if (options.inputPath.empty()) {
        return makeError("missing input bitcode file\n");
    }

    return options;
}

void printUsage(llvm::raw_ostream &out, const char *argv0) {
    out << "Usage: " << argv0 << " [options] <input.bc> [-- program-args...]\n"
        << "\n"
        << "Options:\n"
        << "  --entry <symbol>  Override the entry symbol\n"
        << "  --dump-ir         Print the verified/optimized module before JIT\n"
        << "  --heap-size <n>   Set the managed heap limit in bytes or K/M/G units\n"
        << "  -Xm<size>         JVM-style managed heap limit, for example `-Xm8M`\n"
        << "  -O1..-O3          Select the LLVM optimization level\n"
        << "  -h, --help        Show this help text\n";
}

}  // namespace mvm
