// Host-side check of the committed graphs/: parse them and compare the counts the
// builder knew at construction. No CUDA in this binary: this is the test that
// keeps the host CI job independent of the GPU that generated the artifacts.
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using graphlint::dump_graph;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::parse_graph;
using graphlint::SourceKind;
using graphlint::validate;
using graphlint::WeightKind;

namespace {

constexpr const char* k_graph_dir = GRAPHLINT_GRAPH_DIR;
constexpr std::array<std::uint32_t, 3> k_unit = {1, 1, 1};

std::string path_of(const char* name) {
    return std::string(k_graph_dir) + "/" + name;
}

// Binary: the artifacts are committed with "\n" only, and a text-mode stream
// would hand back CRLF on Windows.
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool load(const char* name, Graph& graph) {
    const std::string path = path_of(name);
    const std::string text = slurp(path);
    if (text.empty()) {
        ADD_FAILURE() << "cannot read " << path;
        return false;
    }
    GraphError error;
    if (!parse_graph(text, graph, error)) {
        ADD_FAILURE() << path << ":" << error.line << ":" << error.column << " " << error.detail;
        return false;
    }
    return true;
}

std::vector<std::size_t> in_degrees(const Graph& graph) {
    std::vector<std::size_t> degrees(graph.nodes.size(), 0);
    for (const std::pair<int, int>& edge : graph.edges) {
        ++degrees[static_cast<std::size_t>(edge.second)];
    }
    return degrees;
}

}  // namespace

TEST(Demo, BuiltShapeMatchesTheConstructedCounts) {
    Graph built;
    ASSERT_TRUE(load("built.json", built));

    EXPECT_EQ(built.source, SourceKind::Built);
    EXPECT_EQ(built.weight_kind, WeightKind::Levels);
    ASSERT_EQ(built.nodes.size(), 27u);
    ASSERT_EQ(built.edges.size(), 30u);
    for (std::size_t i = 0; i < built.nodes.size(); ++i) {
        const Node& node = built.nodes[i];
        EXPECT_EQ(node.id, static_cast<int>(i));
        EXPECT_EQ(node.type, NodeType::Kernel) << "node " << i;
        EXPECT_TRUE(node.name.empty()) << "node " << i;
        EXPECT_EQ(node.grid, k_unit) << "node " << i;
        EXPECT_EQ(node.block, k_unit) << "node " << i;
    }
    GraphError error;
    EXPECT_TRUE(validate(built, error)) << error.detail;
}

TEST(Demo, CapturedShapeHasNoEmptyNodes) {
    Graph captured;
    ASSERT_TRUE(load("captured.json", captured));

    EXPECT_EQ(captured.source, SourceKind::Capture);
    ASSERT_EQ(captured.nodes.size(), 25u);
    ASSERT_EQ(captured.edges.size(), 29u);
    std::size_t empty = 0;
    for (const Node& node : captured.nodes) {
        EXPECT_EQ(node.type, NodeType::Kernel) << "node " << node.id;
        if (node.type == NodeType::Empty) {
            ++empty;
        }
    }
    EXPECT_EQ(empty, 0u);

    // The join of the four branches: the only node with four predecessors.
    const std::vector<std::size_t> degrees = in_degrees(captured);
    ASSERT_FALSE(degrees.empty());
    EXPECT_EQ(*std::max_element(degrees.begin(), degrees.end()), 4u);
    EXPECT_EQ(std::count(degrees.begin(), degrees.end(), 4), 1);
}

TEST(Demo, PingPongHasTenSkipEdges) {
    Graph pingpong;
    ASSERT_TRUE(load("pingpong.json", pingpong));

    EXPECT_EQ(pingpong.source, SourceKind::Capture);
    ASSERT_EQ(pingpong.nodes.size(), 12u);
    ASSERT_EQ(pingpong.edges.size(), 21u);
    for (const Node& node : pingpong.nodes) {
        EXPECT_EQ(node.type, NodeType::Kernel) << "node " << node.id;
    }

    std::size_t stream_edges = 0;
    std::size_t skip_edges = 0;
    std::set<std::pair<int, int>> expected;
    for (int i = 0; i + 1 < 12; ++i) {
        expected.emplace(i, i + 1);  // stream order
        ++stream_edges;
    }
    for (int i = 0; i + 2 < 12; ++i) {
        expected.emplace(i, i + 2);  // the event the next stream waited on
        ++skip_edges;
    }
    EXPECT_EQ(stream_edges, 11u);
    EXPECT_EQ(skip_edges, 10u);
    const std::set<std::pair<int, int>> actual(pingpong.edges.begin(), pingpong.edges.end());
    EXPECT_EQ(actual, expected);
}

TEST(Demo, ChainIsOneThousandSequentialKernels) {
    Graph chain;
    ASSERT_TRUE(load("chain.json", chain));

    EXPECT_EQ(chain.source, SourceKind::Built);
    ASSERT_EQ(chain.nodes.size(), 1000u);
    ASSERT_EQ(chain.edges.size(), 999u);
    for (const Node& node : chain.nodes) {
        EXPECT_EQ(node.type, NodeType::Kernel) << "node " << node.id;
    }
    // Sorted pairs plus one edge per node: edges[k] is the single k -> k+1.
    for (std::size_t k = 0; k + 1 < chain.nodes.size(); ++k) {
        ASSERT_EQ(chain.edges[k], (std::pair<int, int>{static_cast<int>(k), static_cast<int>(k + 1)}))
            << "node " << k;
    }
}

TEST(Demo, ArtifactsRoundTripByteIdentically) {
    for (const char* name : {"built.json", "captured.json", "pingpong.json", "chain.json"}) {
        const std::string text = slurp(path_of(name));
        ASSERT_FALSE(text.empty()) << name;
        Graph graph;
        GraphError error;
        ASSERT_TRUE(parse_graph(text, graph, error))
            << name << ":" << error.line << ":" << error.column << " " << error.detail;
        EXPECT_EQ(dump_graph(graph), text) << name;
        EXPECT_EQ(text.back(), '\n') << name;
        EXPECT_EQ(text.find('\r'), std::string::npos) << name;
    }
}
