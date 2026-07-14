#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kLabelCount = 5;
constexpr std::uint32_t kDefaultSeed = 42;

struct DirectedEdge {
    int from = 0;
    int to = 0;

    bool operator<(const DirectedEdge& other) const {
        return from < other.from || (from == other.from && to < other.to);
    }

    bool operator==(const DirectedEdge& other) const {
        return from == other.from && to == other.to;
    }
};

struct QueryVertexToken {
    std::string identity;
    int label = 0;
};

bool parseLabel(const std::string& text, int& label) {
    if (text.size() != 1 || text[0] < 'A' || text[0] > 'E') return false;
    label = text[0] - 'A' + 1;
    return true;
}

bool parseQueryVertex(const std::string& token, QueryVertexToken& result) {
    const std::size_t separator = token.rfind(':');
    if (separator == std::string::npos) {
        result.identity = token;
        return parseLabel(token, result.label);
    }
    if (separator == 0 || separator + 1 >= token.size()) return false;
    result.identity = token.substr(0, separator);
    return parseLabel(token.substr(separator + 1), result.label);
}

bool parseSeed(const char* text, std::uint32_t& seed) {
    if (text == nullptr || *text == '\0' || *text == '-') return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed == 0 || text[consumed] != '\0' ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        seed = static_cast<std::uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseTemporalTriple(
    const std::string& line,
    long long& from,
    long long& to,
    long long& timestamp) {
    const char* cursor = line.data();
    const char* end = cursor + line.size();
    auto skip_space = [&]() {
        while (cursor < end &&
               std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
    };
    auto parse_integer = [&](long long& value) {
        skip_space();
        if (cursor == end) return false;
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == cursor) return false;
        cursor = parsed.ptr;
        return true;
    };

    if (!parse_integer(from) || !parse_integer(to) || !parse_integer(timestamp)) {
        return false;
    }
    skip_space();
    return cursor == end;
}

bool readTemporalEdges(
    const std::string& path,
    std::vector<DirectedEdge>& edges,
    std::vector<int>& raw_vertices,
    std::vector<int>& active_vertices) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "Error: cannot open temporal graph: " << path << '\n';
        return false;
    }

    long long from = 0;
    long long to = 0;
    long long timestamp = 0;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (!parseTemporalTriple(line, from, to, timestamp)) {
            std::cerr << "Error: temporal graph line " << line_number
                      << " must contain exactly three integers.\n";
            return false;
        }
        if (from < 0 || to < 0 ||
            from > std::numeric_limits<int>::max() ||
            to > std::numeric_limits<int>::max()) {
            std::cerr << "Error: vertex ID is outside the supported int range.\n";
            return false;
        }
        // Labels are drawn over the complete raw endpoint universe, matching
        // ours/original. A self-loop is not a matchable arc and is not emitted
        // as a Turbo vertex, but its endpoint still consumes its stable
        // seed-ordered RNG position before active labels are selected.
        raw_vertices.push_back(static_cast<int>(from));
        raw_vertices.push_back(static_cast<int>(to));
        if (from == to) continue;
        edges.push_back({static_cast<int>(from), static_cast<int>(to)});
    }
    if (input.bad()) {
        std::cerr << "Error: failed while reading temporal graph " << path << '\n';
        return false;
    }
    if (raw_vertices.empty()) {
        std::cerr << "Error: temporal graph has no vertex endpoint.\n";
        return false;
    }

    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::sort(raw_vertices.begin(), raw_vertices.end());
    raw_vertices.erase(
        std::unique(raw_vertices.begin(), raw_vertices.end()), raw_vertices.end());
    active_vertices.reserve(edges.size() * 2);
    for (const DirectedEdge& edge : edges) {
        active_vertices.push_back(edge.from);
        active_vertices.push_back(edge.to);
    }
    std::sort(active_vertices.begin(), active_vertices.end());
    active_vertices.erase(
        std::unique(active_vertices.begin(), active_vertices.end()), active_vertices.end());
    if (active_vertices.empty()) {
        std::cerr << "Error: temporal graph has no active non-self vertex.\n";
        return false;
    }
    return true;
}

bool readQuery(
    const std::string& path,
    std::vector<int>& labels,
    std::vector<DirectedEdge>& edges) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "Error: cannot open query graph: " << path << '\n';
        return false;
    }

    std::unordered_map<std::string, int> vertex_ids;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream parser(line);
        std::string from_text;
        std::string to_text;
        std::string extra;
        if (!(parser >> from_text)) continue;
        if (!(parser >> to_text) || (parser >> extra)) {
            std::cerr << "Error: query line " << line_number
                      << " must contain exactly two vertex tokens.\n";
            return false;
        }

        QueryVertexToken from_token;
        QueryVertexToken to_token;
        if (!parseQueryVertex(from_text, from_token) ||
            !parseQueryVertex(to_text, to_token)) {
            std::cerr << "Error: query labels must be A-E; use id:A for repeated labels.\n";
            return false;
        }

        auto intern = [&](const QueryVertexToken& token) -> int {
            const auto found = vertex_ids.find(token.identity);
            if (found != vertex_ids.end()) {
                if (labels[static_cast<std::size_t>(found->second)] != token.label) return -1;
                return found->second;
            }
            const int id = static_cast<int>(labels.size());
            vertex_ids.emplace(token.identity, id);
            labels.push_back(token.label);
            return id;
        };

        const int from_id = intern(from_token);
        const int to_id = intern(to_token);
        if (from_id < 0 || to_id < 0) {
            std::cerr << "Error: a query vertex identity was assigned conflicting labels.\n";
            return false;
        }
        if (from_id == to_id) {
            std::cerr << "Error: query self-loops are not supported.\n";
            return false;
        }
        edges.push_back({from_id, to_id});
    }
    if (edges.empty()) {
        std::cerr << "Error: query graph has no edge.\n";
        return false;
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return true;
}

bool writeDataGraph(
    const std::string& path,
    const std::vector<int>& raw_vertices,
    const std::vector<int>& active_vertices,
    const std::vector<DirectedEdge>& external_edges,
    std::uint32_t seed) {
    std::unordered_map<int, int> compact_ids;
    compact_ids.reserve(active_vertices.size() * 2);
    for (std::size_t i = 0; i < active_vertices.size(); ++i) {
        compact_ids.emplace(active_vertices[i], static_cast<int>(i));
    }

    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(1, kLabelCount);
    std::vector<int> raw_labels(raw_vertices.size());
    for (int& label : raw_labels) label = distribution(generator);
    std::vector<int> labels;
    labels.reserve(active_vertices.size());
    std::size_t raw_index = 0;
    for (int active_id : active_vertices) {
        while (raw_index < raw_vertices.size() && raw_vertices[raw_index] < active_id) {
            ++raw_index;
        }
        if (raw_index >= raw_vertices.size() || raw_vertices[raw_index] != active_id) {
            std::cerr << "Error: active vertex is absent from the raw label universe.\n";
            return false;
        }
        labels.push_back(raw_labels[raw_index]);
    }

    std::ofstream output(path);
    if (!output) {
        std::cerr << "Error: cannot create converted data graph: " << path << '\n';
        return false;
    }
    output << "t # 0\n"
           << active_vertices.size() << ' ' << external_edges.size() << ' '
           << kLabelCount << '\n';
    for (std::size_t i = 0; i < labels.size(); ++i) {
        output << "v " << i << ' ' << labels[i] << '\n';
    }
    for (const DirectedEdge& edge : external_edges) {
        output << "e " << compact_ids.at(edge.from) << ' '
               << compact_ids.at(edge.to) << '\n';
    }
    output << "t # -1\n";
    return static_cast<bool>(output);
}

bool writeQueryGraph(
    const std::string& path,
    const std::vector<int>& labels,
    const std::vector<DirectedEdge>& edges) {
    std::ofstream output(path);
    if (!output) {
        std::cerr << "Error: cannot create converted query graph: " << path << '\n';
        return false;
    }
    output << "t # 0\n"
           << labels.size() << ' ' << edges.size() << ' '
           << kLabelCount << " 1\n";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        output << "v " << i << ' ' << labels[i] << '\n';
    }
    for (const DirectedEdge& edge : edges) {
        output << "e " << edge.from << ' ' << edge.to << " 1\n";
    }
    output << "t # -1\n";
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <temporal.txt> <query.txt> <data.turbo> <query.turbo> [seed]\n";
        return 2;
    }

    std::uint32_t seed = kDefaultSeed;
    if (argc == 6 && !parseSeed(argv[5], seed)) {
        std::cerr << "Error: seed must be a 32-bit unsigned integer.\n";
        return 2;
    }

    std::vector<DirectedEdge> data_edges;
    std::vector<int> raw_vertices;
    std::vector<int> active_vertices;
    std::vector<int> query_labels;
    std::vector<DirectedEdge> query_edges;
    if (!readTemporalEdges(argv[1], data_edges, raw_vertices, active_vertices) ||
        !readQuery(argv[2], query_labels, query_edges) ||
        !writeDataGraph(
            argv[3], raw_vertices, active_vertices, data_edges, seed) ||
        !writeQueryGraph(argv[4], query_labels, query_edges)) {
        return 1;
    }

    std::cout << "Converted static directed graph: " << active_vertices.size()
              << " vertices, " << data_edges.size()
              << " timestamp-collapsed arcs; query: " << query_labels.size()
              << " vertices, " << query_edges.size()
              << " arcs; random label seed=" << seed << ".\n";
    return 0;
}
