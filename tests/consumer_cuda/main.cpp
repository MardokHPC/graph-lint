#include <graphlint/cuda_source.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // A runner has no card: the branch that needs a handle is only taken on request,
    // so what this binary proves is the installed package, not the driver.
    if (argc > 1) {
        const cudaGraph_t handle = nullptr;
        graphlint::Graph captured;
        std::string error;
        std::vector<cudaGraphNode_t> handles;
        if (!graphlint::from_cuda(handle, graphlint::SourceKind::Built, captured, error,
                                  &handles)) {
            std::fprintf(stderr, "consumer_cuda: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    // The header-only core comes with the same package, so the reader's output must
    // round trip through the JSON the report CLI reads.
    const std::string document = graphlint::dump_graph(graphlint::Graph{});
    graphlint::Graph parsed;
    graphlint::GraphError error;
    if (!graphlint::parse_graph(document, parsed, error) || !parsed.nodes.empty()) {
        std::fprintf(stderr, "consumer_cuda: empty graph did not round trip\n");
        return 1;
    }
    if (graphlint::dump_graph(parsed) != document) {
        std::fprintf(stderr, "consumer_cuda: round trip is not byte-identical\n");
        return 1;
    }
    std::printf("graphlint_cuda linked, json round trip ok\n");
    return 0;
}
