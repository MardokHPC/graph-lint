#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using graphlint::Graph;
using graphlint::GraphError;
using graphlint::GraphErrorKind;
using graphlint::NodeType;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::TopologyTree;
using graphlint::topology;
using graphlint::topology_tree;

std::string join(const std::vector<int>& values) {
    std::string text;
    for (const int value : values) {
        if (!text.empty()) {
            text += ' ';
        }
        text += std::to_string(value);
    }
    return text;
}

std::string join(const std::vector<std::pair<int, int>>& edges) {
    std::string text;
    for (const std::pair<int, int>& edge : edges) {
        if (!text.empty()) {
            text += ' ';
        }
        text += "[" + std::to_string(edge.first) + "->" + std::to_string(edge.second) + "]";
    }
    return text;
}

Graph make_graph(int count) {
    Graph graph;
    graph.source = SourceKind::Built;
    graph.nodes.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        graph.nodes[static_cast<std::size_t>(i)].id = i;
        graph.nodes[static_cast<std::size_t>(i)].type = NodeType::Empty;
    }
    return graph;
}

Graph chained(int count) {
    Graph graph = make_graph(count);
    for (int i = 1; i < count; ++i) {
        graph.edges.emplace_back(i - 1, i);
    }
    return graph;
}

Graph diamond() {
    Graph graph = make_graph(4);
    graph.edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    return graph;
}

std::vector<std::pair<int, int>> unique_edges(const Graph& graph) {
    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

Topology analysed(const Graph& graph) {
    Topology topo;
    GraphError error;
    EXPECT_TRUE(topology(graph, topo, error)) << error.detail;
    return topo;
}

// splitmix64: a fixed seed must pick the same graph on MSVC and GCC, which a standard
// distribution does not promise.
struct Rng {
    std::uint64_t state = 0;

    explicit Rng(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    int below(int limit) {
        return static_cast<int>(next() % static_cast<std::uint64_t>(limit));
    }
};

Graph random_dag(int count, int extra_edges, Rng& rng) {
    Graph graph = make_graph(count);
    for (int node = 1; node < count; ++node) {
        graph.edges.emplace_back(rng.below(node), node);
    }
    for (int k = 0; k < extra_edges; ++k) {
        const int a = rng.below(count);
        const int b = rng.below(count);
        if (a < b) {
            graph.edges.emplace_back(a, b);
        } else if (b < a) {
            graph.edges.emplace_back(b, a);
        }
    }
    return graph;
}

Graph with_shuffled_edges(Graph graph, Rng& rng) {
    for (std::size_t size = graph.edges.size(); size > 1; --size) {
        const std::size_t other = static_cast<std::size_t>(rng.below(static_cast<int>(size)));
        std::swap(graph.edges[size - 1], graph.edges[other]);
    }
    return graph;
}

std::vector<std::vector<char>> closure(int count, const std::vector<std::pair<int, int>>& edges) {
    const std::size_t n = static_cast<std::size_t>(count);
    std::vector<std::vector<char>> reach(n, std::vector<char>(n, 0));
    for (const std::pair<int, int>& edge : edges) {
        reach[static_cast<std::size_t>(edge.first)][static_cast<std::size_t>(edge.second)] = 1;
    }
    for (std::size_t k = 0; k < n; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            if (reach[i][k] == 0) {
                continue;
            }
            for (std::size_t j = 0; j < n; ++j) {
                if (reach[k][j] != 0) {
                    reach[i][j] = 1;
                }
            }
        }
    }
    return reach;
}

// The naive reference: a reachability matrix and one look per edge, no queue, no Kahn.
struct NaiveTopology {
    std::vector<int> level;
    int depth = 0;
    int distinct_edges = 0;
    std::vector<std::pair<int, int>> edges_minimal;
    std::vector<std::pair<int, int>> edges_redundant;
};

NaiveTopology naive_topology(const Graph& graph) {
    const std::size_t n = graph.nodes.size();
    const std::vector<std::pair<int, int>> edges = unique_edges(graph);
    const std::vector<std::vector<char>> reach = closure(static_cast<int>(n), edges);

    NaiveTopology naive;
    naive.distinct_edges = static_cast<int>(edges.size());
    naive.level.assign(n, 0);
    // Relaxation to a fixpoint: exact only on a DAG. On a cycle the n passes stop anyway and
    // leave a wrong level, which is why the helper asserts that topology() succeeded first.
    for (std::size_t pass = 0; pass < n; ++pass) {
        bool changed = false;
        for (const std::pair<int, int>& edge : edges) {
            const std::size_t from = static_cast<std::size_t>(edge.first);
            const std::size_t to = static_cast<std::size_t>(edge.second);
            if (naive.level[to] < naive.level[from] + 1) {
                naive.level[to] = naive.level[from] + 1;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    for (const int value : naive.level) {
        naive.depth = std::max(naive.depth, value + 1);
    }

    for (const std::pair<int, int>& edge : edges) {
        const std::size_t from = static_cast<std::size_t>(edge.first);
        const std::size_t to = static_cast<std::size_t>(edge.second);
        bool implied = false;
        for (std::size_t via = 0; via < n && !implied; ++via) {
            implied = via != to && reach[from][via] != 0 && reach[via][to] != 0;
        }
        if (implied) {
            naive.edges_redundant.push_back(edge);
        } else {
            naive.edges_minimal.push_back(edge);
        }
    }
    return naive;
}

void expect_matches_reference(const Graph& graph, const std::string& label) {
    Topology actual;
    GraphError error;
    ASSERT_TRUE(topology(graph, actual, error)) << label << ": " << error.detail;

    const NaiveTopology reference = naive_topology(graph);

    ASSERT_EQ(actual.level.size(), reference.level.size()) << label;
    for (std::size_t node = 0; node < reference.level.size(); ++node) {
        EXPECT_EQ(actual.level[node], reference.level[node])
            << label << ": level of node " << node << " is " << actual.level[node]
            << ", the naive reference says " << reference.level[node];
    }
    EXPECT_EQ(actual.depth, reference.depth) << label << ": depth";
    EXPECT_EQ(actual.distinct_edges, reference.distinct_edges) << label << ": distinct edges";

    std::vector<int> width(static_cast<std::size_t>(reference.depth), 0);
    for (const int value : reference.level) {
        ++width[static_cast<std::size_t>(value)];
    }
    EXPECT_EQ(join(actual.width), join(width))
        << label << ": width is '" << join(actual.width) << "', the reference says '"
        << join(width) << "'";

    const auto compare = [&](const std::vector<std::pair<int, int>>& actual_edges,
                             const std::vector<std::pair<int, int>>& reference_edges,
                             const char* role) {
        const std::set<std::pair<int, int>> mine(actual_edges.begin(), actual_edges.end());
        const std::set<std::pair<int, int>> naive(reference_edges.begin(), reference_edges.end());
        EXPECT_EQ(mine.size(), actual_edges.size())
            << label << ": the " << role << " list holds a duplicate: " << join(actual_edges);
        for (const std::pair<int, int>& edge : naive) {
            if (mine.count(edge) == 0) {
                ADD_FAILURE() << label << ": edge [" << edge.first << "->" << edge.second << "] is "
                              << role << " for the reference, the library does not list it there";
            }
        }
        for (const std::pair<int, int>& edge : mine) {
            if (naive.count(edge) == 0) {
                ADD_FAILURE() << label << ": edge [" << edge.first << "->" << edge.second << "] is "
                              << role << " for the library, the reference does not list it there";
            }
        }
    };
    compare(actual.edges_minimal, reference.edges_minimal, "minimal");
    compare(actual.edges_redundant, reference.edges_redundant, "redundant");
}

Graph wide_graph() {
    constexpr int k_layers = 50;
    constexpr int k_per_layer = 100;
    const int count = k_layers * k_per_layer;
    Graph graph = make_graph(count);
    for (int layer = 0; layer + 1 < k_layers; ++layer) {
        for (int slot = 0; slot < k_per_layer; ++slot) {
            const int from = layer * k_per_layer + slot;
            for (int branch = 0; branch < 4; ++branch) {
                const int target = (layer + 1) * k_per_layer +
                                   (slot * 7 + branch * 13) % k_per_layer;
                graph.edges.emplace_back(from, target);
            }
        }
    }
    for (int from = 0; from + 400 < count; from += 10) {
        graph.edges.emplace_back(from, from + 400);
    }
    return graph;
}

// The artifacts are asserted by what they mean, never by SHA256: docs/limits.md lets a driver
// permute ids inside a tie class, so a legitimate regeneration must not turn into a red test.
// M5 owns the drift check (`git diff --exit-code graphs/`), not a hash.
constexpr const char* k_graph_dir = GRAPHLINT_GRAPH_DIR;

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool load_artifact(const char* name, Graph& graph) {
    const std::string path = std::string(k_graph_dir) + "/" + name;
    const std::string text = slurp(path);
    if (text.empty()) {
        ADD_FAILURE() << "cannot read " << path;
        return false;
    }
    GraphError error;
    if (!graphlint::parse_graph(text, graph, error)) {
        ADD_FAILURE() << path << ":" << error.line << ":" << error.column << " " << error.detail;
        return false;
    }
    return true;
}

std::string document(int count, const std::string& edges_json) {
    std::string nodes;
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            nodes += ", ";
        }
        nodes += R"({"block": [1, 1, 1], "grid": [1, 1, 1], "id": )" + std::to_string(i) +
                 R"(, "type": "kernel"})";
    }
    return "{\"children\": [], \"edges\": " + edges_json + ", \"nodes\": [" + nodes +
           "], \"schema_version\": 1, \"source\": {\"kind\": \"built\"}, "
           "\"weight_kind\": \"levels\"}";
}

}  // namespace

// ---------------------------------------------------------------------------
// Reference: the naive O(V^3) answer decides when the two disagree (§6.2).
// ---------------------------------------------------------------------------

TEST(Reference, AgreesOnTheShapesWrittenByHand) {
    expect_matches_reference(diamond(), "diamond");

    Graph skip = chained(6);
    skip.edges.emplace_back(0, 3);
    skip.edges.emplace_back(1, 4);
    skip.edges.emplace_back(0, 5);
    expect_matches_reference(skip, "chain with skip edges");

    Graph isolated = make_graph(7);
    for (int i = 1; i < 4; ++i) {
        isolated.edges.emplace_back(i - 1, i);
    }
    expect_matches_reference(isolated, "chain plus isolated nodes");

    Graph layered = make_graph(12);
    for (int from = 0; from < 8; ++from) {
        layered.edges.emplace_back(from, from % 4 + 8);
    }
    expect_matches_reference(layered, "two layers with a fan-in");
}

TEST(Reference, AgreesOnRandomDagsUpToSixtyNodes) {
    Rng rng(0x5eed2026u);
    for (const int count : {2, 5, 12, 30, 45, 60}) {
        for (const int density : {1, 2, 4}) {
            const Graph graph = random_dag(count, count * density, rng);
            expect_matches_reference(graph, "random n=" + std::to_string(count) +
                                                " extra=" + std::to_string(count * density));
        }
    }
}

// ---------------------------------------------------------------------------
// Properties: fixed seed, sizes grow, each property is asserted on all of them.
// ---------------------------------------------------------------------------

TEST(Property, ReductionPreservesReachability) {
    Rng rng(0x20261121u);
    for (const int count : {2, 3, 8, 17, 33, 60, 97}) {
        const Graph graph = random_dag(count, count * 2, rng);
        const Topology topo = analysed(graph);

        const std::vector<std::vector<char>> before = closure(count, unique_edges(graph));
        const std::vector<std::vector<char>> after = closure(count, topo.edges_minimal);
        for (std::size_t from = 0; from < before.size(); ++from) {
            for (std::size_t to = 0; to < before.size(); ++to) {
                EXPECT_EQ(before[from][to], after[from][to])
                    << "n=" << count << ": reachability of [" << from << "->" << to
                    << "] changed: before the reduction " << static_cast<int>(before[from][to])
                    << ", after " << static_cast<int>(after[from][to]);
            }
        }
    }
}

TEST(Property, ReductionNeverAddsAnEdge) {
    Rng rng(0x20261121u);
    for (const int count : {2, 3, 8, 17, 33, 60, 97}) {
        const Graph graph = random_dag(count, count * 2, rng);
        const Topology topo = analysed(graph);

        EXPECT_LE(static_cast<int>(topo.edges_minimal.size()), topo.distinct_edges)
            << "n=" << count;
        EXPECT_EQ(static_cast<int>(topo.edges_minimal.size() + topo.edges_redundant.size()),
                  topo.distinct_edges)
            << "n=" << count << ": minimal " << join(topo.edges_minimal) << ", redundant "
            << join(topo.edges_redundant) << ", distinct " << topo.distinct_edges;
    }
}

TEST(Property, ReductionIsUnique) {
    Rng rng(0x20261121u);
    for (const int count : {2, 3, 8, 17, 33, 60, 97}) {
        const Graph graph = random_dag(count, count * 2, rng);
        const Topology first = analysed(graph);
        const Topology again = analysed(graph);
        const Topology reordered = analysed(with_shuffled_edges(graph, rng));

        EXPECT_EQ(join(first.edges_minimal), join(again.edges_minimal)) << "n=" << count;
        EXPECT_EQ(join(first.edges_minimal), join(reordered.edges_minimal))
            << "n=" << count << ": the edge order in the document changed the reduction";
        EXPECT_EQ(join(first.level), join(reordered.level)) << "n=" << count;
    }
}

TEST(Property, LevelsDoNotChangeAfterTheReduction) {
    Rng rng(0x20261121u);
    for (const int count : {2, 3, 8, 17, 33, 60, 97}) {
        const Graph graph = random_dag(count, count * 2, rng);
        const Topology topo = analysed(graph);

        Graph minimal = make_graph(count);
        minimal.edges = topo.edges_minimal;
        const Topology after = analysed(minimal);

        EXPECT_EQ(join(after.level), join(topo.level)) << "n=" << count;
        EXPECT_EQ(after.depth, topo.depth) << "n=" << count;
        EXPECT_EQ(join(after.width), join(topo.width)) << "n=" << count;
        EXPECT_EQ(join(after.edges_redundant), std::string()) << "n=" << count;
    }
}

// ---------------------------------------------------------------------------
// The committed artifacts: two independent measurements, asserted without a GPU.
// ---------------------------------------------------------------------------

TEST(Artifacts, BuiltHasDepthSeventeenAndNoRedundantEdge) {
    Graph graph;
    ASSERT_TRUE(load_artifact("built.json", graph));
    const Topology topo = analysed(graph);

    EXPECT_EQ(graph.nodes.size(), 27u);
    EXPECT_EQ(graph.edges.size(), 30u);
    EXPECT_EQ(topo.depth, 17);
    EXPECT_EQ(topo.edges_redundant.size(), 0u);
    EXPECT_EQ(topo.edges_minimal.size(), 30u);
    expect_matches_reference(graph, "built.json");
}

TEST(Artifacts, CapturedHasDepthFifteenAndOneRedundantEdge) {
    Graph graph;
    ASSERT_TRUE(load_artifact("captured.json", graph));
    const Topology topo = analysed(graph);

    EXPECT_EQ(graph.nodes.size(), 25u);
    EXPECT_EQ(graph.edges.size(), 29u);
    EXPECT_EQ(topo.depth, 15);
    EXPECT_EQ(topo.edges_redundant.size(), 1u);
    EXPECT_EQ(topo.edges_minimal.size(), 28u);
    EXPECT_EQ(topo.edges_minimal.size() + topo.edges_redundant.size(), graph.edges.size());
    expect_matches_reference(graph, "captured.json");
}

TEST(Artifacts, PingpongHasDepthTwelveAndTenRedundantEdges) {
    Graph graph;
    ASSERT_TRUE(load_artifact("pingpong.json", graph));
    const Topology topo = analysed(graph);

    EXPECT_EQ(graph.nodes.size(), 12u);
    EXPECT_EQ(graph.edges.size(), 21u);
    EXPECT_EQ(topo.depth, 12);
    EXPECT_EQ(join(topo.width), "1 1 1 1 1 1 1 1 1 1 1 1");
    EXPECT_EQ(topo.edges_redundant.size(), 10u);
    EXPECT_EQ(topo.edges_minimal.size(), 11u);
    // i->i+1 is kept, i->i+2 duplicates the same order through one more hop (M0 §0.1).
    EXPECT_EQ(join(topo.edges_minimal),
              "[0->1] [1->2] [2->3] [3->4] [4->5] [5->6] [6->7] [7->8] [8->9] [9->10] [10->11]");
    expect_matches_reference(graph, "pingpong.json");
}

TEST(Artifacts, ChainHasDepthOneThousandAndNoRedundantEdge) {
    Graph graph;
    ASSERT_TRUE(load_artifact("chain.json", graph));
    const Topology topo = analysed(graph);

    EXPECT_EQ(graph.nodes.size(), 1000u);
    EXPECT_EQ(graph.edges.size(), 999u);
    EXPECT_EQ(topo.depth, 1000);
    EXPECT_EQ(topo.level.front(), 0);
    EXPECT_EQ(topo.level.back(), 999);
    EXPECT_DOUBLE_EQ(topo.parallelism, 1.0);
    EXPECT_EQ(topo.edges_redundant.size(), 0u);
    EXPECT_EQ(topo.edges_minimal.size(), 999u);
}

// ---------------------------------------------------------------------------
// Edge cases: the shapes where an off-by-one hides.
// ---------------------------------------------------------------------------

TEST(EdgeCases, EmptyGraphHasNoLevelAndNoParallelism) {
    const Topology topo = analysed(make_graph(0));

    EXPECT_TRUE(topo.acyclic);
    EXPECT_TRUE(topo.level.empty());
    EXPECT_TRUE(topo.width.empty());
    EXPECT_EQ(topo.depth, 0);
    EXPECT_DOUBLE_EQ(topo.parallelism, 0.0);
    EXPECT_EQ(topo.distinct_edges, 0);
    EXPECT_TRUE(topo.edges_minimal.empty());
}

TEST(EdgeCases, SingleNodeIsOneLevelOfWidthOne) {
    const Topology topo = analysed(make_graph(1));

    EXPECT_EQ(join(topo.level), "0");
    EXPECT_EQ(topo.depth, 1);
    EXPECT_EQ(join(topo.width), "1");
    EXPECT_DOUBLE_EQ(topo.parallelism, 1.0);
}

TEST(EdgeCases, TwoIsolatedNodesShareOneLevel) {
    const Topology topo = analysed(make_graph(2));

    EXPECT_EQ(join(topo.level), "0 0");
    EXPECT_EQ(topo.depth, 1);
    EXPECT_EQ(join(topo.width), "2");
    EXPECT_DOUBLE_EQ(topo.parallelism, 2.0);
    EXPECT_EQ(topo.distinct_edges, 0);
}

TEST(EdgeCases, DiamondKeepsItsFourEdges) {
    // 0->{1,2}->3: two branches of one node meet in 3, so every edge carries a path.
    const Topology topo = analysed(diamond());

    EXPECT_EQ(join(topo.level), "0 1 1 2");
    EXPECT_EQ(topo.depth, 3);
    EXPECT_EQ(join(topo.width), "1 2 1");
    EXPECT_EQ(join(topo.edges_minimal), "[0->1] [0->2] [1->3] [2->3]");
    EXPECT_TRUE(topo.edges_redundant.empty());
    EXPECT_DOUBLE_EQ(topo.parallelism, 4.0 / 3.0);
}

TEST(EdgeCases, StaircaseFanInKeepsTheLongestPath) {
    // 0->3, 1->2, 2->3: node 3 is max(0 + 1, 1 + 1) = 2, and the direct edge 0->3 must not
    // pull it back to 1. Levels 0 0 1 2, depth 3, width [2,1,1], no edge of the 3 is implied.
    Graph graph = make_graph(4);
    graph.edges = {{0, 3}, {1, 2}, {2, 3}};
    const Topology topo = analysed(graph);

    EXPECT_EQ(join(topo.level), "0 0 1 2");
    EXPECT_EQ(topo.depth, 3);
    EXPECT_EQ(join(topo.width), "2 1 1");
    EXPECT_EQ(topo.distinct_edges, 3);
    EXPECT_EQ(join(topo.edges_minimal), "[0->3] [1->2] [2->3]");
    EXPECT_TRUE(topo.edges_redundant.empty());
    expect_matches_reference(graph, "staircase fan-in");
}

TEST(EdgeCases, ChainIsAlreadyMinimal) {
    const Topology topo = analysed(chained(5));

    EXPECT_EQ(join(topo.level), "0 1 2 3 4");
    EXPECT_EQ(topo.depth, 5);
    EXPECT_EQ(join(topo.width), "1 1 1 1 1");
    EXPECT_EQ(topo.edges_minimal.size(), 4u);
    EXPECT_TRUE(topo.edges_redundant.empty());
}

TEST(EdgeCases, ChainPlusSkipEdgeLosesExactlyTheSkipEdge) {
    // a->b->c plus a->c: 3 edges, 1 redundant, minimal DAG 2, depth 3, width [1,1,1].
    Graph graph = chained(3);
    graph.edges.emplace_back(0, 2);
    const Topology topo = analysed(graph);

    EXPECT_EQ(topo.depth, 3);
    EXPECT_EQ(join(topo.width), "1 1 1");
    EXPECT_EQ(topo.distinct_edges, 3);
    EXPECT_EQ(join(topo.edges_minimal), "[0->1] [1->2]");
    EXPECT_EQ(join(topo.edges_redundant), "[0->2]");
}

TEST(EdgeCases, IsolatedNodesSitNextToAChain) {
    // 0->1->2 plus an isolated 3: level 0 holds both 0 and 3.
    Graph graph = make_graph(4);
    graph.edges = {{0, 1}, {1, 2}};
    const Topology topo = analysed(graph);

    EXPECT_EQ(join(topo.level), "0 1 2 0");
    EXPECT_EQ(topo.depth, 3);
    EXPECT_EQ(join(topo.width), "2 1 1");
    EXPECT_EQ(join(topo.edges_minimal), "[0->1] [1->2]");
    EXPECT_TRUE(topo.edges_redundant.empty());
}

TEST(EdgeCases, DuplicateEdgesDoNotChangeTheAnswer) {
    Graph single;
    Graph doubled;
    GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(document(3, "[[0, 1], [1, 2], [0, 2]]"), single, error))
        << error.detail;
    ASSERT_TRUE(
        graphlint::parse_graph(document(3, "[[0, 1], [0, 1], [1, 2], [0, 2]]"), doubled, error))
        << error.detail;
    ASSERT_EQ(doubled.edges.size(), 4u) << "the parser is supposed to keep the duplicate";

    const Topology once = analysed(single);
    const Topology twice = analysed(doubled);
    EXPECT_EQ(twice.distinct_edges, 3);
    EXPECT_EQ(join(twice.level), join(once.level));
    EXPECT_EQ(join(twice.edges_minimal), join(once.edges_minimal));
    EXPECT_EQ(join(twice.edges_redundant), join(once.edges_redundant));
    EXPECT_EQ(join(twice.edges_minimal), "[0->1] [1->2]");
    EXPECT_EQ(join(twice.edges_redundant), "[0->2]");
}

// ---------------------------------------------------------------------------
// Errors: a caller can hand-build what the parser refuses, and nothing aborts.
// ---------------------------------------------------------------------------

TEST(Errors, BackwardsEdgeIsReported) {
    // The reviewer's shape: {0->1, 2->1, 2->3}. No cycle, so only the endpoint order is wrong,
    // and validate must name the same kind: one precondition, not two copies that can drift.
    Graph graph = make_graph(4);
    graph.edges = {{0, 1}, {2, 1}, {2, 3}};
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeBackwards);
    GraphError validated;
    EXPECT_FALSE(graphlint::validate(graph, validated)) << "validate accepted the graph";
    EXPECT_EQ(validated.kind, error.kind);
    EXPECT_FALSE(topo.acyclic);
    EXPECT_NE(error.detail.find("[2, 1]"), std::string::npos) << error.detail;
    EXPECT_TRUE(topo.level.empty());
    EXPECT_TRUE(topo.edges_minimal.empty());
    EXPECT_EQ(topo.depth, 0);
}

TEST(Errors, EdgeEndingBeforeItStartsIsReported) {
    Graph graph = make_graph(4);
    graph.edges = {{0, 1}, {3, 1}};
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeBackwards);
    EXPECT_NE(error.detail.find("[3, 1]"), std::string::npos) << error.detail;
}

TEST(Errors, SelfLoopIsABackwardsEdge) {
    // The endpoint check runs before Kahn: with from < to enforced no cycle is constructible,
    // so a self-loop is reported as what it is, a pair that is not from < to.
    Graph graph = make_graph(3);
    graph.edges.emplace_back(2, 2);
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeBackwards);
    EXPECT_FALSE(topo.acyclic);
    EXPECT_NE(error.detail.find("[2, 2]"), std::string::npos) << error.detail;
}

TEST(Errors, EdgeOnAMissingIdIsReportedNotIndexed) {
    Graph graph = make_graph(2);
    graph.edges.emplace_back(0, 2);
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeOutOfRange);
    EXPECT_FALSE(topo.acyclic);
    EXPECT_NE(error.detail.find("[0, 2]"), std::string::npos) << error.detail;
}

TEST(Errors, NegativeEndpointIsReported) {
    Graph graph = make_graph(2);
    graph.edges.emplace_back(-1, 1);
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeOutOfRange);
}

TEST(Errors, SparseIdsAreReported) {
    Graph graph = make_graph(3);
    graph.nodes[1].id = 7;
    Topology topo;
    GraphError error;

    EXPECT_FALSE(topology(graph, topo, error));
    EXPECT_EQ(error.kind, GraphErrorKind::IdNotDense);
}

// ---------------------------------------------------------------------------
// Nested graphs: not flattened, one Topology per Graph, in the same shape.
// ---------------------------------------------------------------------------

TEST(ChildGraphs, TopologyTreeMirrorsTheNesting) {
    Graph root = make_graph(3);
    root.edges = {{0, 1}, {1, 2}};
    root.nodes[2].type = NodeType::ChildGraph;
    root.nodes[2].child = 0;
    root.children.push_back(chained(4));
    root.children[0].children.push_back(diamond());

    TopologyTree tree;
    GraphError error;
    ASSERT_TRUE(topology_tree(root, tree, error)) << error.detail;

    EXPECT_EQ(tree.self.depth, 3);
    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_EQ(join(tree.children[0].self.level), "0 1 2 3");
    ASSERT_EQ(tree.children[0].children.size(), 1u);
    EXPECT_EQ(tree.children[0].children[0].self.depth, 3);
    EXPECT_EQ(join(tree.children[0].children[0].self.width), "1 2 1");
}

TEST(ChildGraphs, BrokenChildKeepsTheRootAnalysis) {
    Graph root = make_graph(1);
    root.nodes[0].type = NodeType::ChildGraph;
    root.nodes[0].child = 0;
    Graph child = chained(2);
    child.edges.emplace_back(1, 0);
    root.children.push_back(child);

    TopologyTree tree;
    GraphError error;
    EXPECT_FALSE(topology_tree(root, tree, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeBackwards);
    EXPECT_NE(error.detail.find("[1, 0]"), std::string::npos) << error.detail;
    EXPECT_TRUE(tree.self.acyclic);
    EXPECT_EQ(tree.self.depth, 1);
    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_FALSE(tree.children[0].self.acyclic);
}

TEST(ChildGraphs, ChildErrorKeepsItsKind) {
    Graph root = chained(2);
    root.nodes[0].type = NodeType::ChildGraph;
    root.nodes[0].child = 0;
    Graph child = make_graph(2);
    child.edges.emplace_back(0, 2);
    root.children.push_back(child);

    TopologyTree tree;
    GraphError error;
    EXPECT_FALSE(topology_tree(root, tree, error));
    EXPECT_EQ(error.kind, GraphErrorKind::EdgeOutOfRange);
    EXPECT_NE(error.detail.find("[0, 2]"), std::string::npos) << error.detail;
}

// ---------------------------------------------------------------------------
// Performance: a generous bound on a CI box, not a benchmark (the numbers live in
// the report; here anything above one second means the algorithm is wrong).
// ---------------------------------------------------------------------------

TEST(Performance, ThousandNodeChainUnderOneSecondBound) {
    const Graph graph = chained(1000);
    const auto start = std::chrono::steady_clock::now();
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error)) << error.detail;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    std::cout << "[ measured ] chain 1000 nodes / 999 edges: " << ms << " ms\n";

    EXPECT_EQ(topo.depth, 1000);
    EXPECT_EQ(topo.edges_minimal.size(), 999u);
    EXPECT_LT(ms, 1000.0) << "bound, not a measurement: " << ms << " ms";
}

TEST(Performance, WideGraphUnderOneSecondBound) {
    const Graph graph = wide_graph();
    const auto start = std::chrono::steady_clock::now();
    Topology topo;
    GraphError error;
    ASSERT_TRUE(topology(graph, topo, error)) << error.detail;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    std::cout << "[ measured ] wide 5000 nodes / " << graph.edges.size() << " edges: " << ms
              << " ms\n";

    EXPECT_EQ(topo.depth, 50);
    EXPECT_EQ(join(topo.width).substr(0, 11), "100 100 100");
    EXPECT_DOUBLE_EQ(topo.parallelism, 100.0);
    EXPECT_LT(ms, 1000.0) << "bound, not a measurement: " << ms << " ms";
}
