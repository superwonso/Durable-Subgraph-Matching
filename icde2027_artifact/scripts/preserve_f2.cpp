// p00 preparation only: retain raw input rows whose directed edge is in F2.
// No generated vertices/edges, no query execution, original row order/CRLF kept.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

static uint64_t key(uint32_t u, uint32_t v) {
    return (uint64_t(u) << 32) | v;
}
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: preserve_f2 RAW F2 OUT");
        std::ifstream retained(argv[2]);
        std::ifstream raw(argv[1], std::ios::binary);
        if (!retained || !raw) throw std::runtime_error("cannot open input");
        std::unordered_set<uint64_t> edges;
        uint32_t u, v;
        int64_t t;
        while (retained >> u >> v >> t) edges.insert(key(u,v));
        std::ofstream out(argv[3], std::ios::binary);
        if (!out) throw std::runtime_error("cannot open output");
        std::string line;
        uint64_t rows = 0;
        while (std::getline(raw, line)) {
            const bool terminated = !raw.eof();
            std::istringstream stream(line);
            if (!(stream >> u >> v >> t)) throw std::runtime_error("raw input must contain three numeric columns");
            if (edges.count(key(u,v))) {
                out.write(line.data(), line.size());
                if (terminated) out.put('\n');
                ++rows;
            }
        }
        if (!out) throw std::runtime_error("output write failed");
        std::cout << "retained_edges=" << edges.size() << " retained_rows=" << rows << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
