#include "v3_formula_cap.h"

#include <iostream>
#include <string>

int main() {
    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(std::cin, line)) {
        ++line_number;
        if (line.empty()) continue;
        try {
            const dsm::v3::Json input = dsm::v3::Json::parse(line);
            if (!input.isObject() || input.objectValue().size() != 2U ||
                input.find("policy") == nullptr ||
                input.find("request") == nullptr) {
                throw dsm::v3::V3FormulaCapError(
                    "oracle helper input field set drift");
            }
            const dsm::v3::V3FormulaCapMaterialization result =
                dsm::v3::materializeV3FormulaCaps(
                    input.at("policy"), input.at("request"));
            std::cout << result.canonical_bytes;
            if (!std::cout) {
                std::cerr << "formula-cap oracle output failure\n";
                return 3;
            }
        } catch (const std::exception& error) {
            std::cerr << "formula-cap oracle line " << line_number << ": "
                      << error.what() << '\n';
            return 2;
        }
    }
    if (!std::cin.eof()) {
        std::cerr << "formula-cap oracle input failure\n";
        return 3;
    }
    return 0;
}
