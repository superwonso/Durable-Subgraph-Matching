// Standalone stage-1 census unit test. No graph search or optimization pass.
// Root controls compilation/execution; this source does not change CMake.
#include "StructuralProfile.h"

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

static void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

int main() {
    try {
        using P = StructuralProfile;
        require(P::ceilLog2(0) == 0 && P::ceilLog2(1) == 0, "zero/one logarithm");
        require(P::ceilLog2(2) == 1 && P::ceilLog2(3) == 2, "ceil logarithm");
        require(P::ceilLog2(15) == 4 && P::ceilLog2(16) == 4 && P::ceilLog2(17) == 5,
                "logarithm around sixteen");
        require(P::ceilLog2(std::numeric_limits<std::uint64_t>::max()) == 64, "largest logarithm");
        P profile;
        const P::Context context{P::Backend::LegacyDag, 7, -1};
        const std::array<std::array<std::size_t, 2>, 12> pairs{{
            {{1, 1}}, {{2, 3}}, {{2, 4}}, {{2, 15}}, {{2, 16}}, {{2, 63}},
            {{2, 64}}, {{2, 255}}, {{2, 256}}, {{0, 7}}, {{7, 0}}, {{0, 0}}
        }};
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            profile.recordBinary(context, pairs[i][0], pairs[i][1], i < 9 ? 1 : 0, i % 2 != 0);
        }
        profile.recordOperand(context, P::Operand::TdBlock, 0);
        profile.recordOperand(context, P::Operand::TdBlock, 17);
        profile.recordOperand(context, P::Operand::OutNeighbor, 64);
        profile.recordCopy(context, P::Copy::NeighborMaterialization, 64, sizeof(int));
        profile.recordCopy(context, P::Copy::StructuralSeed, 17, sizeof(int));
        profile.recordDirectMembership({P::Backend::Direct, -1, 2});
        profile.recordDirectOutput({P::Backend::Direct, -1, 2}, 1);
        std::ostringstream out;
        profile.writeJson(out);
        const std::string json = out.str();
        require(json.find("\"overflow\":false,\"costs_valid\":true") != std::string::npos, "valid counters");
        require(json.find("\"calls\":12,\"merge_strategy_calls\":6,\"binary_search_strategy_calls\":6") != std::string::npos,
                "actual strategy counts");
        require(json.find("\"merge_cost_sum\":708,\"probe_cost_sum\":80") != std::string::npos, "exact cost sums");
        require(json.find("\"nonempty_merge_cost_sum\":694,\"nonempty_probe_cost_sum\":80") != std::string::npos,
                "nonempty cost sums");
        require(json.find("\"ratio_histogram\":[2,2,2,2,1]") != std::string::npos, "ratio boundaries");
        require(json.find("\"empty_left_only\":1,\"empty_right_only\":1,\"empty_both\":1,\"nonempty_calls\":9") != std::string::npos,
                "empty categories");
        require(json.find("\"zero_probe_cost_calls\":4") != std::string::npos, "zero model denominator calls");
        P overflow;
        const auto largest = std::numeric_limits<std::size_t>::max();
        overflow.recordBinary(context, largest, largest, 0, false);
        std::ostringstream overflow_out;
        overflow.writeJson(overflow_out);
        require(overflow_out.str().find("\"overflow\":true,\"costs_valid\":false") != std::string::npos,
                "overflow is fail-closed");
        std::cout << "{\"status\":\"PASS\",\"fixture\":" << json
                  << ",\"overflow_fixture\":" << overflow_out.str() << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Structural-profile counter test failed: " << error.what() << '\n';
        return 1;
    }
}
