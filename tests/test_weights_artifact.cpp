// The two artifacts tools/weights.cu writes: a graph the linter reads and a table bound to it
// by fingerprint. Read with the same loader and the same path the report uses. Nothing here
// asserts a timing — the values are a property of the card that produced them.
#include <graphlint/analysis.hpp>
#include <graphlint/json.hpp>
#include <graphlint/weights.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef GRAPHLINT_MEASURED_JSON
#  error "GRAPHLINT_MEASURED_JSON must be the path of docs/bench/measured.json"
#endif
#ifndef GRAPHLINT_WEIGHTS_JSON
#  error "GRAPHLINT_WEIGHTS_JSON must be the path of docs/bench/weights.json"
#endif

namespace {

bool slurp(const char* path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string bytes;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    text = std::move(bytes);
    return true;
}

}  // namespace

TEST(WeightsArtifact, TableIsBoundToTheGraphItMeasured) {
    std::string measured_text;
    std::string weights_text;
    if (!slurp(GRAPHLINT_MEASURED_JSON, measured_text) ||
        !slurp(GRAPHLINT_WEIGHTS_JSON, weights_text)) {
        GTEST_SKIP() << "docs/bench/measured.json and weights.json are not both there: "
                        "build-device.bat bench writes them, it is opt-in";
    }
    EXPECT_EQ(measured_text.find('\r'), std::string::npos) << "the artifact is LF-only";
    EXPECT_EQ(weights_text.find('\r'), std::string::npos) << "the artifact is LF-only";

    graphlint::Graph graph;
    graphlint::GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(measured_text, graph, error)) << error.detail;
    EXPECT_EQ(graph.source, graphlint::SourceKind::Built);
    EXPECT_EQ(graph.weight_kind, graphlint::WeightKind::Levels);
    // The chain tools/weights.cu builds: eight kernels in a row, nothing implied.
    ASSERT_EQ(graph.nodes.size(), 8u);
    ASSERT_EQ(graph.edges.size(), 7u);

    graphlint::WeightsTable table;
    ASSERT_TRUE(graphlint::parse_weights(weights_text, table, error)) << error.detail;
    // The binding is the whole reason the two files are read together: without it a table from
    // another eight-node graph would apply silently.
    EXPECT_EQ(table.fingerprint, graphlint::graph_fingerprint(graph));
    ASSERT_EQ(table.ns.size(), graph.nodes.size());
    for (std::size_t id = 0; id < table.ns.size(); ++id) {
        // The lower bound of [0, 1e12] is the type; the upper one is a wrong unit, not a slow node.
        EXPECT_LE(table.ns[id], graphlint::k_max_weight_ns) << "node " << id;
    }
    // The load grows with the position and the id is the position, so the table must ascend: a
    // permuted map writes the same eight numbers in another order, and on a chain neither `path`
    // nor its sum can see that — so the producer's guard is repeated here.
    for (std::size_t id = 0; id + 1 < table.ns.size(); ++id) {
        EXPECT_LT(table.ns[id], table.ns[id + 1]) << "ids " << id << " and " << id + 1;
    }

    ASSERT_TRUE(graphlint::apply_weights(graph, table, error)) << error.detail;
    EXPECT_EQ(graph.weight_kind, graphlint::WeightKind::Nanoseconds);

    graphlint::Topology topo;
    ASSERT_TRUE(graphlint::topology(graph, topo, error)) << error.detail;
    graphlint::CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error)) << error.detail;
    // A chain has one path and it takes every node: a table that left one out would not sum it.
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_GT(path.weight_sum, 0u) << "a measured path of zero means the zero point swallowed it";
    EXPECT_LE(path.weight_sum, graphlint::k_max_weight_ns);
}
