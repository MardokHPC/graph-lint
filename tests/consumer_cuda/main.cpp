#include <graphlint/cuda_source.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

// Defined in other.cpp, which includes the same header: the two objects must link.
std::string consumer_cuda_document_from_other_tu();

int main(int argc, char** argv) {
    // A runner has no card: the branch that needs a handle is only taken on request, so
    // what this binary proves is the installed header, not the driver.
    if (argc > 1) {
        const cudaGraph_t handle = nullptr;
        graphlint::Graph captured;
        std::string error;
        std::vector<cudaGraphNode_t> handles;
        const bool read = graphlint::from_cuda(handle, graphlint::SourceKind::Built, captured,
                                               error, &handles);
        const bool wrote = graphlint::write_graph_json(handle, "consumer_cuda.json", error);
        if (!read || !wrote) {
            std::fprintf(stderr, "consumer_cuda: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    // The same header carries the model's JSON, so the package's reader and the file the
    // report CLI reads must round trip byte for byte.
    const std::string document = consumer_cuda_document_from_other_tu();
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
    std::printf("graphlint header-only reader, json round trip ok\n");
    return 0;
}
