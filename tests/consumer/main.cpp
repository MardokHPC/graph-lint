#include <graphlint/analysis.hpp>
#include <graphlint/config.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <cstdio>
#include <string>

int main() {
    // CI proves the package, so the installed headers must compile and link
    // together: empty graph -> dump -> parse -> topology is the smallest pass.
    const std::string document = graphlint::dump_graph(graphlint::Graph{});
    graphlint::Graph parsed;
    graphlint::GraphError error;
    if (!graphlint::parse_graph(document, parsed, error) || !parsed.nodes.empty()) {
        std::fprintf(stderr, "consumer: empty graph did not round trip\n");
        return 1;
    }
    if (graphlint::dump_graph(parsed) != document) {
        std::fprintf(stderr, "consumer: round trip is not byte-identical\n");
        return 1;
    }
    graphlint::Topology topo;
    if (!graphlint::topology(parsed, topo, error) || topo.depth != 0) {
        std::fprintf(stderr, "consumer: empty graph has no depth 0\n");
        return 1;
    }
    std::printf("graphlint %s\n", GRAPHLINT_VERSION_STRING);
    return 0;
}
