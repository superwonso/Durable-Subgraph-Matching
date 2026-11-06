#include "validated_graph_snapshot.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        const GraphValidationAccountingModelArtifactV1 artifact =
            graphValidationAccountingModelArtifactV1();
        if (!artifact.complete()) {
            throw std::runtime_error("graph accounting artifact is incomplete");
        }
        if (argc == 1) {
            std::cout << artifact.canonical_bytes;
            return 0;
        }
        if (argc != 2) {
            throw std::runtime_error(
                "usage: v3_graph_accounting_model_cli [output.json]");
        }
        std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open graph accounting output");
        }
        output.write(
            artifact.canonical_bytes.data(),
            static_cast<std::streamsize>(artifact.canonical_bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write graph accounting output");
        }
        std::cout << artifact.sha256 << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "graph accounting artifact error: "
                  << error.what() << '\n';
        return 1;
    }
}
