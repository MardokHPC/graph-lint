#include <graphlint/analysis.hpp>
#include <graphlint/emit_mermaid.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/weights.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using graphlint::CriticalPath;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::GraphErrorKind;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::Topology;
using graphlint::WeightsTable;
using graphlint::WeightKind;
using graphlint::graph_fingerprint;
using graphlint::parse_graph;
using graphlint::parse_weights;
using graphlint::topology;

constexpr const char* k_data_dir = GRAPHLINT_TEST_DATA_DIR;
constexpr const char* k_weights_dir = GRAPHLINT_WEIGHTS_DIR;

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

Node kernel_node(int id) {
    Node node;
    node.id = id;
    node.type = NodeType::Kernel;
    node.grid = {1, 1, 1};
    node.block = {1, 1, 1};
    return node;
}

Graph diamond() {
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1), kernel_node(2), kernel_node(3)};
    graph.edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    return graph;
}

WeightsTable diamond_table(const Graph& graph) {
    WeightsTable table;
    table.ns = {200, 700, 300, 100};
    table.fingerprint = graph_fingerprint(graph);
    return table;
}

// The fingerprint of the four-kernel diamond this file builds: one refinement round over the
// node keys and the edges, see tests/data/weights/diamond.json for the committed graph.
constexpr std::uint64_t k_diamond_fingerprint = 0xa0a35f85541f4aebull;
constexpr const char* k_diamond_fingerprint_hex = "a0a35f85541f4aeb";

// The four decimals of the contract's example, so a hostile edit stands out.
std::string diamond_table_document() {
    return std::string("{\n  \"graph\": \"") + k_diamond_fingerprint_hex +
           "\", \"node_count\": 4, \"schema_version\": 1, \"unit\": \"ns\",\n"
           "  \"weights\": [[0, 200], [1, 700], [2, 300], [3, 100]]\n}\n";
}

// A table with the right shape and one row replaced, for the loader's hostile cases.
std::string table_with_rows(const std::string& rows, const std::string& node_count = "4") {
    return std::string("{\"graph\": \"") + k_diamond_fingerprint_hex + "\", \"node_count\": " +
           node_count +
           ", \"schema_version\": 1, \"unit\": \"ns\", \"weights\": [" + rows + "]}";
}

bool refuses(const std::string& text, GraphErrorKind kind, std::string_view needle) {
    WeightsTable table;
    GraphError error;
    if (parse_weights(text, table, error)) {
        ADD_FAILURE() << "the loader accepted: " << text;
        return false;
    }
    if (error.kind != kind) {
        ADD_FAILURE() << "kind " << static_cast<int>(error.kind) << " for " << text << ": "
                      << error.detail;
        return false;
    }
    if (error.detail.find(needle) == std::string::npos) {
        ADD_FAILURE() << "detail \"" << error.detail << "\" does not name \"" << needle
                      << "\" for " << text;
        return false;
    }
    return true;
}

std::vector<int> hop_path(const Graph& graph, const Topology& topo) {
    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::vector<int>> predecessors(graph.nodes.size());
    for (const std::pair<int, int>& edge : edges) {
        predecessors[static_cast<std::size_t>(edge.second)].push_back(edge.first);
    }

    std::vector<int> path;
    if (graph.nodes.empty()) {
        return path;
    }
    int current = -1;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (topo.level[i] == topo.depth - 1) {
            current = static_cast<int>(i);
            break;
        }
    }
    while (current >= 0) {
        path.push_back(current);
        const std::size_t at = static_cast<std::size_t>(current);
        if (topo.level[at] == 0) {
            break;
        }
        int next = -1;
        for (const int candidate : predecessors[at]) {
            if (topo.level[static_cast<std::size_t>(candidate)] == topo.level[at] - 1) {
                next = candidate;
                break;
            }
        }
        current = next;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

bool has_edge(const Graph& graph, int from, int to) {
    return std::find(graph.edges.begin(), graph.edges.end(), std::make_pair(from, to)) !=
           graph.edges.end();
}

// The same graph with its ids permuted, which is what an API-order change looks like: nodes
// stay where their new id puts them and the edges follow, backwards pairs included.
Graph relabelled(const Graph& graph, const std::vector<int>& permutation) {
    const std::size_t n = graph.nodes.size();
    Graph out;
    out.source = graph.source;
    out.weight_kind = graph.weight_kind;
    out.nodes.resize(n);
    for (std::size_t old_id = 0; old_id < n; ++old_id) {
        Node node = graph.nodes[old_id];
        node.id = permutation[old_id];
        out.nodes[static_cast<std::size_t>(node.id)] = node;
    }
    for (const std::pair<int, int>& edge : graph.edges) {
        out.edges.emplace_back(permutation[static_cast<std::size_t>(edge.first)],
                               permutation[static_cast<std::size_t>(edge.second)]);
    }
    std::sort(out.edges.begin(), out.edges.end());
    return out;
}

}  // namespace

TEST(Weights, DiamondTableParses) {
    WeightsTable table;
    GraphError error;
    ASSERT_TRUE(parse_weights(diamond_table_document(), table, error))
        << error.line << ":" << error.column << " " << error.detail;
    EXPECT_EQ(table.ns, (std::vector<std::uint64_t>{200, 700, 300, 100}));
    EXPECT_EQ(table.fingerprint, k_diamond_fingerprint);
}

TEST(Weights, LoaderAcceptsZero) {
    const std::string text = table_with_rows("[0, 0], [1, 700], [2, 0], [3, 100]");
    WeightsTable parsed;
    GraphError error;
    EXPECT_TRUE(parse_weights(text, parsed, error)) << error.detail;
    EXPECT_EQ(parsed.ns[0], 0u);
}

TEST(Weights, LoaderRefusesEveryIdFaultNamingIt) {
    // Missing: the dense 0..3 sequence loses 2, so row 2 must say 2 and says 3.
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [3, 100]"),
                        GraphErrorKind::WeightRule, "expected id 2, found 3"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [1, 300], [3, 100]"),
                        GraphErrorKind::WeightRule, "repeats id 1"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [4, 100]"),
                        GraphErrorKind::WeightRule, "expected id 3, found 4"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [-1, 100]"),
                        GraphErrorKind::WeightRule, "negative id -1"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [0, 300], [3, 100]"),
                        GraphErrorKind::WeightRule, "after 1: rows ascend by id"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300]"),
                        GraphErrorKind::WeightRule, "node_count 4 does not match 3"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [3, 100]", "5"),
                        GraphErrorKind::WeightRule, "node_count 5 does not match 4"));
}

TEST(Weights, LoaderRefusesABadNumberOrUnit) {
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [3, 1000000000001]"),
                        GraphErrorKind::WeightRule, "over 1e12"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [3, 1.5]"),
                        GraphErrorKind::BadNumber, "expected an integer"));
    EXPECT_TRUE(refuses(table_with_rows("[0, 200], [1, 700], [2, 300], [3, -100]"),
                        GraphErrorKind::BadNumber, "unsigned integer"));

    const std::string no_unit =
        std::string("{\"graph\": \"") + k_diamond_fingerprint_hex +
        "\", \"node_count\": 4, \"schema_version\": 1, "
        "\"weights\": [[0, 200], [1, 700], [2, 300], [3, 100]]}";
    EXPECT_TRUE(refuses(no_unit, GraphErrorKind::MissingKey, "unit"));

    std::string us = diamond_table_document();
    const std::size_t at = us.find("\"ns\"");
    ASSERT_NE(at, std::string::npos);
    us.replace(at, 4, "\"us\"");
    EXPECT_TRUE(refuses(us, GraphErrorKind::WeightRule, "unit \"us\" is not \"ns\""));
}

TEST(Weights, LoaderRefusesTheWrongSchemaOrFingerprintShape) {
    std::string version_two = diamond_table_document();
    const std::size_t at = version_two.find("\"schema_version\": 1");
    ASSERT_NE(at, std::string::npos);
    version_two.replace(at, std::string("\"schema_version\": 1").size(), "\"schema_version\": 2");
    EXPECT_TRUE(refuses(version_two, GraphErrorKind::SchemaVersion, "unsupported"));

    std::string upper_hex(k_diamond_fingerprint_hex);
    for (char& ch : upper_hex) {
        if (ch >= 'a' && ch <= 'f') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    std::string uppercase = diamond_table_document();
    const std::size_t hex = uppercase.find(k_diamond_fingerprint_hex);
    ASSERT_NE(hex, std::string::npos);
    uppercase.replace(hex, 16, upper_hex);
    EXPECT_TRUE(refuses(uppercase, GraphErrorKind::WeightRule, "16 lowercase hex"));

    std::string short_hex = diamond_table_document();
    short_hex.replace(hex, 16, std::string(k_diamond_fingerprint_hex, 15));
    EXPECT_TRUE(refuses(short_hex, GraphErrorKind::WeightRule, "16 lowercase hex"));

    // The grammar of graph.json: keys ascend, duplicates are refused.
    EXPECT_TRUE(refuses(diamond_table_document() + "{}", GraphErrorKind::Trailing,
                        "trailing bytes"));
    std::string unsorted = diamond_table_document();
    const std::size_t unit = unsorted.find("\"unit\"");
    ASSERT_NE(unit, std::string::npos);
    unsorted.insert(unit, "\"schema_version\": 1, ");
    EXPECT_TRUE(refuses(unsorted, GraphErrorKind::DuplicateKey, "schema_version"));
}

TEST(Weights, ApplyWritesTheWeightsAndTheKind) {
    Graph graph = diamond();
    const WeightsTable table = diamond_table(graph);
    GraphError error;
    ASSERT_TRUE(graphlint::apply_weights(graph, table, error)) << error.detail;
    EXPECT_EQ(graph.weight_kind, WeightKind::Nanoseconds);
    EXPECT_EQ(graph.nodes[0].weight, 200u);
    EXPECT_EQ(graph.nodes[1].weight, 700u);
    EXPECT_EQ(graph.nodes[2].weight, 300u);
    EXPECT_EQ(graph.nodes[3].weight, 100u);
    EXPECT_TRUE(graphlint::validate(graph, error)) << error.detail;

    Topology topo;
    ASSERT_TRUE(topology(graph, topo, error)) << error.detail;
    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error)) << error.detail;
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 3}));
    EXPECT_EQ(path.weight_sum, 1000u);
}

TEST(Weights, ApplyRefusesChildrenAndAnNsGraph) {
    Graph with_child = diamond();
    with_child.children.resize(1);
    GraphError error;
    EXPECT_FALSE(graphlint::apply_weights(with_child, diamond_table(diamond()), error));
    EXPECT_EQ(error.kind, GraphErrorKind::WeightRule);
    EXPECT_NE(error.detail.find("child graphs"), std::string::npos) << error.detail;

    Graph twice = diamond();
    const WeightsTable table = diamond_table(twice);
    ASSERT_TRUE(graphlint::apply_weights(twice, table, error));
    EXPECT_FALSE(graphlint::apply_weights(twice, table, error));
    EXPECT_EQ(error.kind, GraphErrorKind::WeightRule);
    EXPECT_NE(error.detail.find("already carries ns"), std::string::npos) << error.detail;
}

TEST(Weights, ApplyRefusesATableFromAnotherGraphOfTheSameSize) {
    Graph other = diamond();
    other.edges = {{0, 1}, {0, 2}, {1, 2}, {2, 3}};  // four nodes, four distinct edges
    WeightsTable table = diamond_table(diamond());

    GraphError error;
    EXPECT_FALSE(graphlint::apply_weights(other, table, error));
    EXPECT_EQ(error.kind, GraphErrorKind::WeightRule);
    // Both hex names are in the sentence: the table's and the graph's.
    std::string named;
    graphlint::detail::append_fingerprint(named, table.fingerprint);
    EXPECT_NE(error.detail.find(named), std::string::npos) << error.detail;
    EXPECT_NE(error.detail.find("and this graph is"), std::string::npos) << error.detail;
}

TEST(Weights, FingerprintSurvivesEveryRelabelling) {
    // The reviewer's case: 1 -> 2, 3 -> 4, an isolated 0, five identical kernels. F2 leaves a
    // residual in a tie class, so canonicalize can return either 1->3 2->4 or 1->4 2->3.
    Graph five;
    five.nodes = {kernel_node(0), kernel_node(1), kernel_node(2), kernel_node(3),
                  kernel_node(4)};
    five.edges = {{1, 2}, {3, 4}};
    const std::uint64_t reference = graph_fingerprint(five);

    std::mt19937 rng(0x5eed2026u);
    std::vector<int> permutation = {0, 1, 2, 3, 4};
    for (int round = 0; round < 200; ++round) {
        std::shuffle(permutation.begin(), permutation.end(), rng);
        EXPECT_EQ(graph_fingerprint(relabelled(five, permutation)), reference)
            << "round " << round;
    }

    Graph diamond_graph = diamond();
    const std::uint64_t diamond_fingerprint = graph_fingerprint(diamond_graph);
    std::vector<int> diamond_permutation = {0, 1, 2, 3};
    for (int round = 0; round < 200; ++round) {
        std::shuffle(diamond_permutation.begin(), diamond_permutation.end(), rng);
        EXPECT_EQ(graph_fingerprint(relabelled(diamond_graph, diamond_permutation)),
                  diamond_fingerprint)
            << "round " << round;
    }

    // The table of the graph applies to its relabelled copy: that is the false refusal.
    Graph net = diamond_graph;
    Graph copy = relabelled(diamond_graph, {2, 0, 3, 1});
    GraphError error;
    ASSERT_TRUE(graphlint::apply_weights(net, diamond_table(diamond()), error)) << error.detail;
    ASSERT_TRUE(graphlint::apply_weights(copy, diamond_table(diamond()), error)) << error.detail;
}

TEST(Weights, FingerprintSeparatesGraphsAndIgnoresDuplicates) {
    const std::uint64_t diamond_once = graph_fingerprint(diamond());

    Graph duplicated = diamond();
    duplicated.edges.emplace_back(0, 1);
    EXPECT_EQ(graph_fingerprint(duplicated), diamond_once);

    Graph renamed = diamond();
    renamed.nodes[2].name = "c";
    EXPECT_NE(graph_fingerprint(renamed), diamond_once);

    // A same-size, same-degree graph with one edge moved: the count cannot see it.
    Graph moved = diamond();
    moved.edges = {{0, 1}, {0, 2}, {1, 2}, {2, 3}};
    EXPECT_NE(graph_fingerprint(moved), diamond_once);

    std::string hex;
    graphlint::detail::append_fingerprint(hex, diamond_once);
    EXPECT_EQ(hex.size(), 16u);
}

// ---------------------------------------------------------------------------
// The critical path: docs/weights.md section 3.
// ---------------------------------------------------------------------------

TEST(CriticalPath, DiamondTakesTheHeavierBranch) {
    Graph graph = diamond();
    GraphError error;
    ASSERT_TRUE(graphlint::apply_weights(graph, diamond_table(graph), error)) << error.detail;
    Topology topo;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 3}));
    EXPECT_EQ(path.weight_sum, 1000u);
}

TEST(CriticalPath, SmallestEndWinsATie) {
    // Two sinks at dist 510: the contract takes the smaller id.
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1), kernel_node(2)};
    graph.edges = {{0, 1}, {0, 2}};
    graph.weight_kind = WeightKind::Nanoseconds;
    graph.nodes[0].weight = 10;
    graph.nodes[1].weight = 500;
    graph.nodes[2].weight = 500;
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1}));
    EXPECT_EQ(path.weight_sum, 510u);
}

TEST(CriticalPath, TieInTheMiddleTakesTheSmallestPredecessor) {
    Graph graph = diamond();
    graph.weight_kind = WeightKind::Nanoseconds;
    const std::uint64_t weights[] = {100, 200, 200, 50};
    for (std::size_t i = 0; i < 4; ++i) {
        graph.nodes[i].weight = weights[i];
    }
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 3}));
    EXPECT_EQ(path.weight_sum, 350u);
}

TEST(CriticalPath, ZeroWeightsAndTheEmptyGraph) {
    Graph graph = diamond();
    graph.weight_kind = WeightKind::Nanoseconds;
    for (Node& node : graph.nodes) {
        node.weight = 0;
    }
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));
    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0}));
    EXPECT_EQ(path.weight_sum, 0u);

    Graph empty;
    ASSERT_TRUE(topology(empty, topo, error));
    ASSERT_TRUE(graphlint::critical_path(empty, topo, path, error));
    EXPECT_TRUE(path.nodes.empty());
    EXPECT_EQ(path.weight_sum, 0u);
}

// A zero weight is legal (docs/weights.md section 1), so dist 0 is a value the back-step must
// accept: it is the weight of a real node, not the absence of a predecessor.
TEST(CriticalPath, ZeroWeightSourceStaysOnThePath) {
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1), kernel_node(2)};
    graph.edges = {{0, 1}, {1, 2}};
    graph.weight_kind = WeightKind::Nanoseconds;
    graph.nodes[0].weight = 0;
    graph.nodes[1].weight = 5;
    graph.nodes[2].weight = 1;
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(path.weight_sum, 6u);
}

TEST(CriticalPath, ZeroWeightNodeInTheMiddleDoesNotStopTheWalk) {
    // Both arms are free, so the tie picks node 1 and the walk still has to reach source 0.
    Graph graph = diamond();
    graph.weight_kind = WeightKind::Nanoseconds;
    const std::uint64_t weights[] = {0, 0, 0, 5};
    for (std::size_t i = 0; i < 4; ++i) {
        graph.nodes[i].weight = weights[i];
    }
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 1, 3}));
    EXPECT_EQ(path.weight_sum, 5u);
}

// The two hand cases above pin the refusal; this catches the next version of it, because a
// path that stops early is visible as a first node with a predecessor.
TEST(CriticalPath, EveryRandomPathStartsAtASourceAndSumsItsWeights) {
    std::mt19937 rng(0x0badc0deu);
    for (int round = 0; round < 300; ++round) {
        const int n = 1 + static_cast<int>(rng() % 12u);
        Graph graph;
        graph.weight_kind = WeightKind::Nanoseconds;
        for (int id = 0; id < n; ++id) {
            Node node = kernel_node(id);
            node.weight = rng() % 4u;
            graph.nodes.push_back(node);
        }
        std::vector<std::vector<int>> predecessors(static_cast<std::size_t>(n));
        for (int from = 0; from < n; ++from) {
            for (int to = from + 1; to < n; ++to) {
                if (rng() % 3u != 0u) {
                    continue;
                }
                graph.edges.emplace_back(from, to);
                predecessors[static_cast<std::size_t>(to)].push_back(from);
            }
        }
        if (!graph.edges.empty() && round % 2 == 0) {
            graph.edges.push_back(graph.edges.front());
        }

        Topology topo;
        GraphError error;
        ASSERT_TRUE(topology(graph, topo, error)) << "round " << round;
        CriticalPath path;
        ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error)) << "round " << round;
        ASSERT_FALSE(path.nodes.empty()) << "round " << round;

        EXPECT_TRUE(predecessors[static_cast<std::size_t>(path.nodes.front())].empty())
            << "round " << round << ": the path starts at " << path.nodes.front()
            << ", which has a predecessor";

        std::uint64_t total = 0;
        for (std::size_t i = 0; i < path.nodes.size(); ++i) {
            total += graph.nodes[static_cast<std::size_t>(path.nodes[i])].weight;
            if (i != 0) {
                EXPECT_TRUE(has_edge(graph, path.nodes[i - 1], path.nodes[i]))
                    << "round " << round << ": " << path.nodes[i - 1] << " -> "
                    << path.nodes[i];
            }
        }
        EXPECT_EQ(path.weight_sum, total) << "round " << round;

        // Independent maximum: the reference the reviewer's harness computed in Python.
        std::vector<std::uint64_t> dist(static_cast<std::size_t>(n), 0);
        std::uint64_t maximum = 0;
        for (int id = 0; id < n; ++id) {
            const std::size_t at = static_cast<std::size_t>(id);
            std::uint64_t best = 0;
            for (const int p : predecessors[at]) {
                best = std::max(best, dist[static_cast<std::size_t>(p)]);
            }
            dist[at] = graph.nodes[at].weight + best;
            maximum = std::max(maximum, dist[at]);
        }
        EXPECT_EQ(path.weight_sum, maximum) << "round " << round;
    }
}

// Uniform 1 is the bridge between M5 and M7a: the weighted path is the hop path, on every
// fixture the emitters already draw.
TEST(CriticalPath, UniformOneEqualsTheHopPathOnEveryFixture) {
    for (const char* name : {"chain8.json", "child_graph.json", "diamond.json", "empty.json",
                             "mixed.json", "ns.json"}) {
        const std::string path = std::string(k_data_dir) + "/" + name;
        const std::string text = slurp(path);
        ASSERT_FALSE(text.empty()) << path;
        Graph graph;
        GraphError error;
        ASSERT_TRUE(parse_graph(text, graph, error)) << path << ": " << error.detail;

        graph.weight_kind = WeightKind::Nanoseconds;
        for (Node& node : graph.nodes) {
            node.weight = 1;
        }
        Topology topo;
        ASSERT_TRUE(topology(graph, topo, error)) << path << ": " << error.detail;

        CriticalPath critical;
        ASSERT_TRUE(graphlint::critical_path(graph, topo, critical, error))
            << path << ": " << error.detail;
        const std::vector<int> hops = hop_path(graph, topo);
        EXPECT_EQ(critical.nodes, hops) << path;
        EXPECT_EQ(critical.weight_sum, critical.nodes.size()) << path;
        for (std::size_t i = 0; i + 1 < critical.nodes.size(); ++i) {
            EXPECT_TRUE(has_edge(graph, critical.nodes[i], critical.nodes[i + 1])) << path;
            EXPECT_TRUE(std::binary_search(topo.edges_minimal.begin(), topo.edges_minimal.end(),
                                           std::make_pair(critical.nodes[i],
                                                          critical.nodes[i + 1])))
                << path << ": pair " << critical.nodes[i] << " -> " << critical.nodes[i + 1];
        }
    }
}

// ---------------------------------------------------------------------------
// Mermaid: the red path is the weighted one in ns, and a chain is wholly on or off.
// ---------------------------------------------------------------------------

TEST(Mermaid, WeightedChain8IsByteIdenticalToTheHopDrawing) {
    const std::string text = slurp(std::string(k_data_dir) + "/chain8.json");
    ASSERT_FALSE(text.empty());
    Graph levels;
    GraphError error;
    ASSERT_TRUE(parse_graph(text, levels, error));
    Graph weighted = levels;
    weighted.weight_kind = WeightKind::Nanoseconds;
    for (Node& node : weighted.nodes) {
        node.weight = 1;
    }

    Topology topo;
    ASSERT_TRUE(topology(weighted, topo, error));
    const std::string drawing = graphlint::to_mermaid(weighted, topo);
    EXPECT_EQ(drawing, graphlint::to_mermaid(levels, topo));
    EXPECT_NE(drawing.find("  n0[\"kernel#0\xc3\x97" "8\"]\n"), std::string::npos) << drawing;
    EXPECT_NE(drawing.find("  class n0 critical\n"), std::string::npos) << drawing;
}

TEST(Mermaid, AWeightedPathThatSkipsALevelMarksOnlyItsNodes) {
    // 0 -> 1 -> 2 -> 3 plus 0 -> 2 implied: in ns the 1000 ns of node 2 through 0 beats the
    // free detour through 1, so the red path is 0, 2, 3 and node 1 stays black.
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1), kernel_node(2), kernel_node(3)};
    graph.edges = {{0, 1}, {0, 2}, {1, 2}, {2, 3}};
    graph.weight_kind = WeightKind::Nanoseconds;
    const std::uint64_t weights[] = {1000, 0, 1000, 1};
    for (std::size_t i = 0; i < 4; ++i) {
        graph.nodes[i].weight = weights[i];
    }
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error));

    CriticalPath path;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, path, error));
    EXPECT_EQ(path.nodes, (std::vector<int>{0, 2, 3}));
    EXPECT_EQ(path.weight_sum, 2001u);

    const std::string drawing = graphlint::to_mermaid(graph, topo);
    EXPECT_NE(drawing.find("  class n0,n2 critical\n"), std::string::npos) << drawing;

    // The hop answer would have marked every node: the emitter read the weights.
    Graph levels = graph;
    levels.weight_kind = WeightKind::Levels;
    for (Node& node : levels.nodes) {
        node.weight = 1;
    }
    EXPECT_NE(graphlint::to_mermaid(levels, topo).find("  class n0,n1,n2 critical\n"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Determinism: one graph, one answer, twice.
// ---------------------------------------------------------------------------

TEST(Weights, TheCommittedTableBindsTheCommittedGraph) {
    Graph graph;
    GraphError error;
    ASSERT_TRUE(parse_graph(slurp(std::string(k_data_dir) + "/diamond.json"), graph, error))
        << error.detail;
    WeightsTable table;
    ASSERT_TRUE(parse_weights(slurp(std::string(k_weights_dir) + "/diamond.json"), table, error))
        << error.detail;
    ASSERT_TRUE(graphlint::apply_weights(graph, table, error)) << error.detail;
    EXPECT_EQ(graph.weight_kind, WeightKind::Nanoseconds);
    EXPECT_EQ(graph.nodes[1].weight, 700u);

    // The same two calls, twice: the fingerprint and the path do not depend on the run.
    const std::uint64_t once = graph_fingerprint(graph);
    EXPECT_EQ(once, graph_fingerprint(graph));
    Topology topo;
    ASSERT_TRUE(topology(graph, topo, error));
    CriticalPath first;
    CriticalPath second;
    ASSERT_TRUE(graphlint::critical_path(graph, topo, first, error));
    ASSERT_TRUE(graphlint::critical_path(graph, topo, second, error));
    EXPECT_EQ(first.nodes, second.nodes);
    EXPECT_EQ(first.weight_sum, second.weight_sum);
}
