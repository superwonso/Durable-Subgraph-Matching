#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t edgeHash(std::int64_t source, std::int64_t target, std::uint64_t seed) {
    const auto source_bits = static_cast<std::uint64_t>(source);
    const auto target_bits = static_cast<std::uint64_t>(target);
    return splitmix64(source_bits ^ splitmix64(target_bits + seed));
}

std::uint64_t parseUnsigned(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed);
    if (consumed != text.size()) throw std::invalid_argument(name);
    return static_cast<std::uint64_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::cerr
            << "Usage: " << argv[0]
            << " <input> <output> <numerator> <denominator> [seed]\n"
               "All occurrences of a selected directed (src,dst) history are"
               " retained together.\n";
        return 1;
    }

    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
    std::uint64_t seed = 42;
    try {
        numerator = parseUnsigned(argv[3], "numerator");
        denominator = parseUnsigned(argv[4], "denominator");
        if (argc == 6) seed = parseUnsigned(argv[5], "seed");
        if (denominator == 0 || numerator > denominator) {
            throw std::invalid_argument("sampling fraction");
        }
    } catch (const std::exception&) {
        std::cerr << "Error: require 0 <= numerator <= denominator and denominator > 0.\n";
        return 1;
    }

    std::ifstream input(argv[1]);
    std::ofstream output(argv[2]);
    if (!input.is_open() || !output.is_open()) {
        std::cerr << "Error: could not open input or output file.\n";
        return 2;
    }

    std::uint64_t input_rows = 0;
    std::uint64_t output_rows = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream parser(line);
        std::int64_t source = 0;
        std::int64_t target = 0;
        std::int64_t snapshot = 0;
        std::string trailing;
        if (!(parser >> source >> target >> snapshot) || (parser >> trailing)) {
            std::cerr << "Error: malformed row " << (input_rows + 1) << ".\n";
            return 3;
        }
        ++input_rows;
        if (edgeHash(source, target, seed) % denominator >= numerator) continue;
        output << source << ' ' << target << ' ' << snapshot << '\n';
        ++output_rows;
    }
    if (!output.good()) {
        std::cerr << "Error: output write failed.\n";
        return 4;
    }
    std::cout << "Sampled " << output_rows << " of " << input_rows
              << " rows (" << numerator << '/' << denominator
              << ") while preserving complete directed edge histories.\n";
    return 0;
}
