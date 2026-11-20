#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

graphlint::Node make_kernel(int id, std::string name) {
    graphlint::Node node;
    node.id = id;
    node.type = graphlint::NodeType::Kernel;
    node.name = std::move(name);
    node.grid = {1, 1, 1};
    node.block = {128, 1, 1};
    return node;
}

graphlint::Graph chain(int length) {
    graphlint::Graph graph;
    for (int i = 0; i < length; ++i) {
        graph.nodes.push_back(make_kernel(i, "k" + std::to_string(i)));
    }
    for (int i = 0; i + 1 < length; ++i) {
        graph.edges.emplace_back(i, i + 1);
    }
    return graph;
}

// Every node carries a distinct name, so all key2 values differ and Kahn never
// reaches the api-index tie-break (F1).
graphlint::Graph asymmetric_graph() {
    graphlint::Graph graph;
    graph.nodes.push_back(make_kernel(0, "entry"));
    graph.nodes.push_back(make_kernel(1, "left"));
    graph.nodes.push_back(make_kernel(2, "right"));
    graph.nodes.push_back(make_kernel(3, "join"));
    graph.nodes.push_back(make_kernel(4, "sink"));
    graph.nodes.push_back(make_kernel(5, "exit"));
    graph.nodes[1].grid = {2, 1, 1};
    graph.nodes[2].block = {64, 1, 1};
    graph.nodes[3].shared_bytes = 2048;
    const std::pair<int, int> edges[] = {{0, 1}, {0, 2}, {1, 3}, {2, 3},
                                         {3, 4}, {1, 4}, {4, 5}};
    graph.edges.assign(std::begin(edges), std::end(edges));
    return graph;
}

std::vector<std::size_t> shuffled_order(std::size_t size, unsigned int seed) {
    std::vector<std::size_t> order(size);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::shuffle(order.begin(), order.end(), std::mt19937(seed));
    return order;
}

graphlint::Graph permuted(const graphlint::Graph& graph,
                          const std::vector<std::size_t>& order) {
    graphlint::Graph result;
    result.source = graph.source;
    result.weight_kind = graph.weight_kind;
    result.nodes.resize(graph.nodes.size());
    std::vector<std::size_t> position(graph.nodes.size(), 0);
    for (std::size_t k = 0; k < order.size(); ++k) {
        result.nodes[k] = graph.nodes[order[k]];
        position[order[k]] = k;
    }
    result.edges.reserve(graph.edges.size());
    for (const std::pair<int, int>& edge : graph.edges) {
        result.edges.emplace_back(
            static_cast<int>(position[static_cast<std::size_t>(edge.first)]),
            static_cast<int>(position[static_cast<std::size_t>(edge.second)]));
    }
    return result;
}

std::vector<std::pair<int, int>> degree_histogram(const graphlint::Graph& graph) {
    std::vector<std::pair<int, int>> histogram(graph.nodes.size(), {0, 0});
    for (const std::pair<int, int>& edge : graph.edges) {
        ++histogram[static_cast<std::size_t>(edge.first)].second;
        ++histogram[static_cast<std::size_t>(edge.second)].first;
    }
    std::sort(histogram.begin(), histogram.end());
    return histogram;
}

}  // namespace

TEST(Graph, NodeTypeTokenRoundTripsForEveryValue) {
    const graphlint::NodeType types[] = {
        graphlint::NodeType::Kernel,       graphlint::NodeType::Memcpy,
        graphlint::NodeType::Memset,       graphlint::NodeType::Host,
        graphlint::NodeType::ChildGraph,   graphlint::NodeType::Empty,
        graphlint::NodeType::EventRecord,  graphlint::NodeType::EventWait,
        graphlint::NodeType::MemAlloc,     graphlint::NodeType::MemFree,
        graphlint::NodeType::ExtSemaphore, graphlint::NodeType::Unknown};

    std::vector<std::string> tokens;
    for (const graphlint::NodeType type : types) {
        const char* token = graphlint::to_string(type);
        tokens.emplace_back(token);
        graphlint::NodeType parsed = graphlint::NodeType::Unknown;
        ASSERT_TRUE(graphlint::node_type_from_string(token, parsed)) << token;
        EXPECT_EQ(parsed, type) << token;
    }
    std::sort(tokens.begin(), tokens.end());
    const auto unique_end = std::unique(tokens.begin(), tokens.end());
    EXPECT_EQ(static_cast<std::size_t>(unique_end - tokens.begin()), 12u);

    graphlint::NodeType ignored = graphlint::NodeType::Kernel;
    EXPECT_FALSE(graphlint::node_type_from_string("Kernel", ignored));
    EXPECT_FALSE(graphlint::node_type_from_string("", ignored));
}

TEST(Graph, EqualityComparesChildrenAndEveryField) {
    graphlint::Node node;
    node.id = 4;
    node.type = graphlint::NodeType::Kernel;
    node.name = "n";
    node.weight = 9;
    node.grid = {2, 3, 4};
    node.block = {5, 6, 7};
    node.shared_bytes = 11;
    node.alloc_bytes = 12;
    node.mem_ptr = 0x1234;
    node.child = 3;

    {
        graphlint::Node other = node;
        EXPECT_TRUE(node == other);
    }
    {
        graphlint::Node other = node;
        other.id = 5;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.type = graphlint::NodeType::Memcpy;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.name = "m";
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.weight = 10;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.grid = {2, 3, 5};
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.block = {5, 6, 8};
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.shared_bytes = 12;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.alloc_bytes = 13;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.mem_ptr = 0x1235;
        EXPECT_FALSE(node == other);
    }
    {
        graphlint::Node other = node;
        other.child = 4;
        EXPECT_FALSE(node == other);
    }

    graphlint::Graph graph;
    graph.nodes.push_back(make_kernel(0, "k"));
    graph.nodes.push_back(make_kernel(1, "j"));
    graphlint::Node child_node;
    child_node.id = 2;
    child_node.type = graphlint::NodeType::ChildGraph;
    child_node.child = 0;
    graph.nodes.push_back(child_node);
    graph.edges = {{0, 1}};
    graph.children.push_back(chain(1));
    graph.source = graphlint::SourceKind::Capture;

    {
        graphlint::Graph other = graph;
        EXPECT_TRUE(graph == other);
    }
    {
        graphlint::Graph other = graph;
        other.nodes[0].name = "x";
        EXPECT_FALSE(graph == other);
    }
    {
        graphlint::Graph other = graph;
        other.edges.clear();
        EXPECT_FALSE(graph == other);
    }
    {
        graphlint::Graph other = graph;
        other.children[0].nodes[0].name = "x";
        EXPECT_FALSE(graph == other);
    }
    {
        graphlint::Graph other = graph;
        other.source = graphlint::SourceKind::Built;
        EXPECT_FALSE(graph == other);
    }
    {
        graphlint::Graph other = graph;
        other.weight_kind = graphlint::WeightKind::Nanoseconds;
        EXPECT_FALSE(graph == other);
    }
}

TEST(Graph, CanonicalizeAssignsDenseTopologicalIds) {
    graphlint::Graph graph = chain(5);
    std::reverse(graph.nodes.begin(), graph.nodes.end());
    for (std::pair<int, int>& edge : graph.edges) {
        edge = {4 - edge.first, 4 - edge.second};
    }

    graphlint::canonicalize(graph);

    ASSERT_EQ(graph.nodes.size(), 5u);
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        EXPECT_EQ(graph.nodes[i].id, static_cast<int>(i));
        EXPECT_EQ(graph.nodes[i].name, "k" + std::to_string(i));
    }
    ASSERT_EQ(graph.edges.size(), 4u);
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        EXPECT_EQ(graph.edges[i].first, static_cast<int>(i));
        EXPECT_EQ(graph.edges[i].second, static_cast<int>(i + 1));
    }
}

TEST(Graph, ShuffledInputGivesTheSameBytes) {
    graphlint::Graph reference = asymmetric_graph();
    graphlint::canonicalize(reference);
    const std::string expected = graphlint::dump_graph(reference);
    ASSERT_FALSE(expected.empty());

    for (unsigned int seed = 1; seed <= 8; ++seed) {
        const std::vector<std::size_t> order =
            shuffled_order(reference.nodes.size(), seed);
        graphlint::Graph shuffled = permuted(reference, order);
        graphlint::canonicalize(shuffled);
        EXPECT_EQ(graphlint::dump_graph(shuffled), expected) << "seed " << seed;
    }
}

// F2's residual is a permutation inside one class: bytes may differ, every
// number the tool reports does not.
TEST(Graph, SymmetricBranchesKeepTheSameDegreeHistogram) {
    graphlint::Graph reference;
    reference.nodes.push_back(make_kernel(0, "root"));
    for (int i = 0; i < 4; ++i) {
        reference.nodes.push_back(make_kernel(1 + i, ""));
    }
    reference.nodes.push_back(make_kernel(5, "join"));
    for (int i = 0; i < 4; ++i) {
        reference.edges.emplace_back(0, 1 + i);
        reference.edges.emplace_back(1 + i, 5);
    }

    graphlint::canonicalize(reference);
    const std::vector<std::pair<int, int>> expected = degree_histogram(reference);

    for (unsigned int seed = 1; seed <= 8; ++seed) {
        const std::vector<std::size_t> order =
            shuffled_order(reference.nodes.size(), seed);
        graphlint::Graph shuffled = permuted(reference, order);
        graphlint::canonicalize(shuffled);
        EXPECT_EQ(degree_histogram(shuffled), expected) << "seed " << seed;
    }
}

// A hand-built Graph can hold 0 -> 1 and 1 -> 0: validate is where the kind is
// reachable, since the document grammar makes a cycle unrepresentable (§3).
TEST(Graph, ValidateRejectsACycle) {
    graphlint::Graph graph;
    graph.nodes.push_back(make_kernel(0, "a"));
    graph.nodes.push_back(make_kernel(1, "b"));
    graph.edges = {{0, 1}, {1, 0}};

    graphlint::GraphError error;
    EXPECT_FALSE(graphlint::validate(graph, error));
    EXPECT_EQ(error.kind, graphlint::GraphErrorKind::Cycle);
    EXPECT_NE(error.detail.find("cycle"), std::string::npos);
    EXPECT_NE(error.detail.find("2 of 2"), std::string::npos);
}

TEST(Graph, ChildrenArePermutedIntoOwnerIdOrder) {
    graphlint::Graph graph;
    graph.nodes.push_back(make_kernel(0, "k"));
    graphlint::Node second_listed;
    second_listed.id = 1;
    second_listed.type = graphlint::NodeType::ChildGraph;
    second_listed.name = "aa";
    second_listed.child = 1;
    graph.nodes.push_back(second_listed);
    graphlint::Node third_listed;
    third_listed.id = 2;
    third_listed.type = graphlint::NodeType::ChildGraph;
    third_listed.name = "zz";
    third_listed.child = 0;
    graph.nodes.push_back(third_listed);
    graph.children.push_back(chain(1));
    graph.children.push_back(chain(2));

    graphlint::canonicalize(graph);

    ASSERT_EQ(graph.nodes.size(), 3u);
    ASSERT_EQ(graph.children.size(), 2u);
    EXPECT_EQ(graph.nodes[0].name, "k");
    EXPECT_EQ(graph.nodes[1].name, "aa");
    EXPECT_EQ(graph.nodes[2].name, "zz");
    EXPECT_EQ(graph.nodes[1].child, 0);
    EXPECT_EQ(graph.nodes[2].child, 1);
    EXPECT_EQ(graph.children[0].nodes.size(), 2u);
    EXPECT_EQ(graph.children[1].nodes.size(), 1u);
    for (const graphlint::Graph& child : graph.children) {
        for (std::size_t i = 0; i < child.nodes.size(); ++i) {
            EXPECT_EQ(child.nodes[i].id, static_cast<int>(i));
        }
    }
}
