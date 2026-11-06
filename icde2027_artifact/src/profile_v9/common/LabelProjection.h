#ifndef LABEL_PROJECTION_H
#define LABEL_PROJECTION_H

#include "Utils.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A validated, query-specific projection of a versioned explicit global label
// map into the executor's fixed A--E runtime slots.  Entries remain sorted by
// external vertex ID so graph loading can validate exact raw-vertex coverage
// without building another hash table.
struct ExplicitRuntimeLabelMap {
    std::vector<std::pair<int, Label>> labels_by_external_id;
    std::vector<std::uint32_t> selected_global_classes;
    std::array<std::size_t, kLabelCount> runtime_class_counts{};
    std::size_t invalid_vertex_count = 0;
    std::size_t global_label_cardinality = 0;

    std::string global_label_map_file_sha256;
    std::string global_label_map_mapping_sha256;
    std::string query_label_projection_file_sha256;
    std::string query_file_sha256;
};

// Load both artifacts and validate their schema, identities, class
// cardinality/distribution, and A--E bijection.  The global map JSON is the
// deterministic schema emitted by benchmark/m4_workload_metadata.py.
bool loadExplicitRuntimeLabelMap(
    const std::string& global_label_map_file,
    const std::string& query_label_projection_file,
    const std::string& query_file,
    ExplicitRuntimeLabelMap& result,
    std::string& error);

#endif // LABEL_PROJECTION_H
