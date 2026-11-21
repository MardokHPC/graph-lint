#include <graphlint/analysis.hpp>
#include <graphlint/emit_dot.hpp>
#include <graphlint/emit_mermaid.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using graphlint::DotOptions;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::MermaidOptions;
using graphlint::NodeType;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::parse_graph;
using graphlint::to_dot;
using graphlint::to_mermaid;
using graphlint::topology;

constexpr const char* k_data_dir = GRAPHLINT_TEST_DATA_DIR;
constexpr const char* k_graph_dir = GRAPHLINT_GRAPH_DIR;
constexpr const char* k_source_dir = GRAPHLINT_SOURCE_DIR;

// U+00D7 by bytes: the source encoding must not decide what the label contains.
constexpr const char* k_times = "\xc3\x97";

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct Loaded {
    Graph graph;
    Topology topo;
};

bool load(const std::string& path, Loaded& out) {
    const std::string text = slurp(path);
    if (text.empty()) {
        ADD_FAILURE() << "cannot read " << path;
        return false;
    }
    GraphError error;
    if (!parse_graph(text, out.graph, error)) {
        ADD_FAILURE() << path << ":" << error.line << ":" << error.column << " " << error.detail;
        return false;
    }
    if (!topology(out.graph, out.topo, error)) {
        ADD_FAILURE() << path << ": " << error.detail;
        return false;
    }
    return true;
}

bool load_fixture(const char* name, Loaded& out) {
    return load(std::string(k_data_dir) + "/" + name, out);
}

Graph make_graph(int count, NodeType type) {
    Graph graph;
    graph.source = SourceKind::Built;
    graph.nodes.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        graph.nodes[static_cast<std::size_t>(i)].id = i;
        graph.nodes[static_cast<std::size_t>(i)].type = type;
    }
    return graph;
}

Topology analysed(const Graph& graph) {
    Topology topo;
    GraphError error;
    EXPECT_TRUE(topology(graph, topo, error)) << error.detail;
    return topo;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// DOT: the contract's own byte strings, then the pieces a renderer cares about.
// ---------------------------------------------------------------------------

TEST(Dot, MixedIsBytePinnedToTheContract) {
    Loaded mixed;
    ASSERT_TRUE(load_fixture("mixed.json", mixed));

    // The graph has 5 nodes and 6 edge tuples, one of them a duplicate of 0->3.
    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"kernel#0\"];\n"
        "  n1 [label=\"kernel#1\"];\n"
        "  n2 [label=\"mem_alloc#2\", style=filled, fillcolor=gray85];\n"
        "  n3 [label=\"mem_free#3\", style=filled, fillcolor=gray85];\n"
        "  n4 [label=\"child_graph#4\", shape=box3d];\n"
        "\n"
        "  n0 -> n1;\n"
        "  n0 -> n3 [style=dashed];\n"
        "  n1 -> n3;\n"
        "  n2 -> n4;\n"
        "  n3 -> n4;\n"
        "}\n";
    EXPECT_EQ(to_dot(mixed.graph, mixed.topo), expected);
}

TEST(Dot, ShapesFollowTheTypeTable) {
    const NodeType types[] = {NodeType::Kernel,       NodeType::Memcpy,    NodeType::Memset,
                              NodeType::Host,         NodeType::ChildGraph, NodeType::Empty,
                              NodeType::EventRecord,  NodeType::EventWait, NodeType::MemAlloc,
                              NodeType::MemFree,      NodeType::ExtSemaphore, NodeType::Unknown};
    Graph graph = make_graph(12, NodeType::Unknown);
    for (std::size_t i = 0; i < 12; ++i) {
        graph.nodes[i].type = types[i];
    }

    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"kernel#0\"];\n"
        "  n1 [label=\"memcpy#1\", shape=parallelogram];\n"
        "  n2 [label=\"memset#2\", shape=parallelogram];\n"
        "  n3 [label=\"host#3\", shape=octagon, style=filled, fillcolor=orange];\n"
        "  n4 [label=\"child_graph#4\", shape=box3d];\n"
        "  n5 [label=\"empty#5\"];\n"
        "  n6 [label=\"event_record#6\", shape=octagon, style=filled, fillcolor=orange];\n"
        "  n7 [label=\"event_wait#7\", shape=octagon, style=filled, fillcolor=orange];\n"
        "  n8 [label=\"mem_alloc#8\", style=filled, fillcolor=gray85];\n"
        "  n9 [label=\"mem_free#9\", style=filled, fillcolor=gray85];\n"
        "  n10 [label=\"ext_semaphore#10\", shape=octagon, style=filled, fillcolor=orange];\n"
        "  n11 [label=\"unknown#11\"];\n"
        "}\n";
    EXPECT_EQ(to_dot(graph, analysed(graph)), expected);
}

TEST(Dot, DetailAddsGridSharedAndPointer) {
    Loaded mixed;
    ASSERT_TRUE(load_fixture("mixed.json", mixed));

    DotOptions options;
    options.detail_params = true;
    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"kernel#0\\ngrid 14x14x1 block 128x1x1 shared 8192 B\"];\n"
        "  n1 [label=\"kernel#1\\ngrid 1x1x1 block 256x1x1\"];\n"
        "  n2 [label=\"mem_alloc#2\\n1048576 B ptr 0x7f0000000000\", style=filled, "
        "fillcolor=gray85];\n"
        "  n3 [label=\"mem_free#3\\nptr 0x7f0000000000\", style=filled, fillcolor=gray85];\n"
        "  n4 [label=\"child_graph#4\\nchild 0\", shape=box3d];\n"
        "\n"
        "  n0 -> n1;\n"
        "  n0 -> n3 [style=dashed];\n"
        "  n1 -> n3;\n"
        "  n2 -> n4;\n"
        "  n3 -> n4;\n"
        "}\n";
    EXPECT_EQ(to_dot(mixed.graph, mixed.topo, options), expected);
}

TEST(Dot, AllocWithoutBytesPrintsOnlyThePointer) {
    Graph graph = make_graph(2, NodeType::Kernel);
    graph.nodes[0].grid = {1, 1, 1};
    graph.nodes[0].block = {1, 1, 1};
    graph.nodes[1].type = NodeType::MemAlloc;
    graph.nodes[1].mem_ptr = 0x40;

    DotOptions options;
    options.detail_params = true;
    // alloc_bytes 0 is the absent field in the JSON, so there is no size to print.
    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"kernel#0\\ngrid 1x1x1 block 1x1x1\"];\n"
        "  n1 [label=\"mem_alloc#1\\nptr 0x40\", style=filled, fillcolor=gray85];\n"
        "}\n";
    EXPECT_EQ(to_dot(graph, analysed(graph), options), expected);
}

TEST(Dot, EmptyGraphClosesAfterTheDefault) {
    Loaded empty;
    ASSERT_TRUE(load_fixture("empty.json", empty));

    EXPECT_EQ(to_dot(empty.graph, empty.topo),
              "digraph graphlint {\n  rankdir=LR;\n  node [shape=box];\n}\n");
}

TEST(Dot, SingleNodeIsOneLine) {
    Graph graph = make_graph(1, NodeType::Kernel);
    EXPECT_EQ(to_dot(graph, analysed(graph)),
              "digraph graphlint {\n  rankdir=LR;\n  node [shape=box];\n"
              "  n0 [label=\"kernel#0\"];\n}\n");
}

TEST(Dot, HostileNameIsEscapedAndLoneContinuationFallsBack) {
    Graph graph = make_graph(2, NodeType::Kernel);
    std::string hostile = "q";
    hostile += '"';
    hostile += '\\';
    hostile += '\n';
    hostile += '\t';
    hostile += '\x01';
    hostile += '\x7f';
    hostile += "e\xcc\x81";
    graph.nodes[0].name = hostile;
    graph.nodes[1].name = "\xc3";

    // Four controls become four spaces, then the quote and the backslash are escaped.
    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"q\\\"\\\\    e\xcc\x81\"];\n"
        "  n1 [label=\"kernel#1\"];\n"
        "}\n";
    EXPECT_EQ(to_dot(graph, analysed(graph)), expected);

    // The same names never reach a Mermaid label: the alphabet stays closed.
    EXPECT_EQ(to_mermaid(graph, analysed(graph)),
              "flowchart LR\n  n0[\"kernel#0\"]\n  n1[\"kernel#1\"]\n"
              "  classDef critical stroke:#c00,stroke-width:2px\n"
              "  classDef sync fill:#f90\n"
              "  class n0 critical\n");
}

TEST(Dot, InvalidUtf8FallsBack) {
    // Overlong U+002F, a surrogate, a code point past U+10FFFF, then the valid maximum.
    const char* names[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
    Graph graph = make_graph(4, NodeType::Kernel);
    for (std::size_t i = 0; i < 4; ++i) {
        graph.nodes[i].name = names[i];
    }

    const std::string expected =
        "digraph graphlint {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  n0 [label=\"kernel#0\"];\n"
        "  n1 [label=\"kernel#1\"];\n"
        "  n2 [label=\"kernel#2\"];\n"
        "  n3 [label=\"\xf4\x8f\xbf\xbf\"];\n"
        "}\n";
    EXPECT_EQ(to_dot(graph, analysed(graph)), expected);
}

TEST(Dot, DuplicatePairIsDrawnOnce) {
    Graph graph = make_graph(2, NodeType::Kernel);
    graph.edges = {{0, 1}, {0, 1}};
    EXPECT_EQ(to_dot(graph, analysed(graph)),
              "digraph graphlint {\n  rankdir=LR;\n  node [shape=box];\n"
              "  n0 [label=\"kernel#0\"];\n"
              "  n1 [label=\"kernel#1\"];\n"
              "\n"
              "  n0 -> n1;\n}\n");
}

// ---------------------------------------------------------------------------
// Mermaid: compression, the critical path and the cap.
// ---------------------------------------------------------------------------

TEST(Mermaid, DiamondIsBytePinnedToTheContract) {
    Loaded diamond;
    ASSERT_TRUE(load_fixture("diamond.json", diamond));

    // Nothing compresses and the names a, b, c, d are dropped; the back-walk from 3
    // reaches 1 before 2, so n2 is not red.
    const std::string expected =
        "flowchart LR\n"
        "  n0[\"kernel#0\"]\n"
        "  n1[\"kernel#1\"]\n"
        "  n2[\"kernel#2\"]\n"
        "  n3[\"kernel#3\"]\n"
        "  n0 --> n1\n"
        "  n0 --> n2\n"
        "  n1 --> n3\n"
        "  n2 --> n3\n"
        "  classDef critical stroke:#c00,stroke-width:2px\n"
        "  classDef sync fill:#f90\n"
        "  class n0,n1,n3 critical\n";
    EXPECT_EQ(to_mermaid(diamond.graph, diamond.topo), expected);
}

TEST(Mermaid, Chain8IsOneBox) {
    Loaded chain;
    ASSERT_TRUE(load_fixture("chain8.json", chain));

    const std::string expected = std::string("flowchart LR\n  n0[\"kernel#0") + k_times +
                                 "8\"]\n"
                                 "  classDef critical stroke:#c00,stroke-width:2px\n"
                                 "  classDef sync fill:#f90\n"
                                 "  class n0 critical\n";
    EXPECT_EQ(to_mermaid(chain.graph, chain.topo), expected);
    // The seven edges of the chain are inside the only drawn node: nothing to draw.
    EXPECT_EQ(count_of(to_mermaid(chain.graph, chain.topo), "->"), 0u);
}

TEST(Mermaid, NonKernelBreaksTheChain) {
    const NodeType types[] = {NodeType::Kernel, NodeType::EventWait, NodeType::Kernel,
                              NodeType::MemAlloc, NodeType::Kernel, NodeType::MemFree,
                              NodeType::Kernel};
    Graph graph = make_graph(7, NodeType::Kernel);
    for (std::size_t i = 0; i < 7; ++i) {
        graph.nodes[i].type = types[i];
    }
    for (int i = 1; i < 7; ++i) {
        graph.edges.emplace_back(i - 1, i);
    }

    const std::string expected =
        "flowchart LR\n"
        "  n0[\"kernel#0\"]\n"
        "  n1{{\"event_wait#1\"}}\n"
        "  n2[\"kernel#2\"]\n"
        "  n3[(\"mem_alloc#3\")]\n"
        "  n4[\"kernel#4\"]\n"
        "  n5[(\"mem_free#5\")]\n"
        "  n6[\"kernel#6\"]\n"
        "  n0 --> n1\n"
        "  n1 --> n2\n"
        "  n2 --> n3\n"
        "  n3 --> n4\n"
        "  n4 --> n5\n"
        "  n5 --> n6\n"
        "  classDef critical stroke:#c00,stroke-width:2px\n"
        "  classDef sync fill:#f90\n"
        "  class n0,n1,n2,n3,n4,n5,n6 critical\n"
        "  class n1 sync\n";
    EXPECT_EQ(to_mermaid(graph, analysed(graph)), expected);
}

TEST(Mermaid, ChainKeepsTheExternalDegrees) {
    // Two fan-ins (0, 1), fourteen kernels (2..15), three fan-outs (16..18).
    Graph graph = make_graph(19, NodeType::Kernel);
    graph.edges = {{0, 2}, {1, 2}};
    for (int i = 3; i <= 15; ++i) {
        graph.edges.emplace_back(i - 1, i);
    }
    for (int i = 16; i <= 18; ++i) {
        graph.edges.emplace_back(15, i);
    }
    const Topology topo = analysed(graph);

    const std::string expected = std::string("flowchart LR\n"
                                             "  n0[\"kernel#0\"]\n"
                                             "  n1[\"kernel#1\"]\n"
                                             "  n2[\"kernel#2") +
                                 k_times +
                                 "14\"]\n"
                                 "  n16[\"kernel#16\"]\n"
                                 "  n17[\"kernel#17\"]\n"
                                 "  n18[\"kernel#18\"]\n"
                                 "  n0 --> n2\n"
                                 "  n1 --> n2\n"
                                 "  n2 --> n16\n"
                                 "  n2 --> n17\n"
                                 "  n2 --> n18\n"
                                 "  classDef critical stroke:#c00,stroke-width:2px\n"
                                 "  classDef sync fill:#f90\n"
                                 "  class n0,n2,n16 critical\n";
    const std::string output = to_mermaid(graph, topo);
    EXPECT_EQ(output, expected);

    // The head keeps the two external in-edges and the tail the three out-edges; the
    // model itself is untouched: 14 nodes, 15 edges, depth 16.
    EXPECT_EQ(count_of(output, "--> n2\n"), 2u);
    EXPECT_EQ(count_of(output, "  n2 --> "), 3u);
    EXPECT_EQ(graph.nodes.size(), 19u);
    EXPECT_EQ(graph.edges.size(), 18u);
    EXPECT_EQ(topo.depth, 16);
}

TEST(Mermaid, EmptyGraphIsTheHeaderAndTwoClassDefs) {
    Loaded empty;
    ASSERT_TRUE(load_fixture("empty.json", empty));

    EXPECT_EQ(to_mermaid(empty.graph, empty.topo),
              "flowchart LR\n"
              "  classDef critical stroke:#c00,stroke-width:2px\n"
              "  classDef sync fill:#f90\n");
}

TEST(Mermaid, SingleNodeWithoutEdges) {
    Graph graph = make_graph(1, NodeType::Kernel);

    // A lone node is the longest path of a one-level graph: the walk starts and ends
    // on it, so it is the node the back-walk picks.
    EXPECT_EQ(to_mermaid(graph, analysed(graph)),
              "flowchart LR\n  n0[\"kernel#0\"]\n"
              "  classDef critical stroke:#c00,stroke-width:2px\n"
              "  classDef sync fill:#f90\n"
              "  class n0 critical\n");
}

TEST(Mermaid, ShapesFollowTheTypeTable) {
    const NodeType types[] = {NodeType::Kernel,       NodeType::Memcpy,    NodeType::Memset,
                              NodeType::Host,         NodeType::ChildGraph, NodeType::Empty,
                              NodeType::EventRecord,  NodeType::EventWait, NodeType::MemAlloc,
                              NodeType::MemFree,      NodeType::ExtSemaphore, NodeType::Unknown};
    const char* lines[] = {
        "  n0[\"kernel#0\"]\n",
        "  n1[/\"memcpy#1\"/]\n",
        "  n2[/\"memset#2\"/]\n",
        "  n3{{\"host#3\"}}\n",
        "  n4[[\"child_graph#4\"]]\n",
        "  n5[\"empty#5\"]\n",
        "  n6{{\"event_record#6\"}}\n",
        "  n7{{\"event_wait#7\"}}\n",
        "  n8[(\"mem_alloc#8\")]\n",
        "  n9[(\"mem_free#9\")]\n",
        "  n10{{\"ext_semaphore#10\"}}\n",
        "  n11[\"unknown#11\"]\n",
    };
    Graph graph = make_graph(12, NodeType::Unknown);
    for (std::size_t i = 0; i < 12; ++i) {
        graph.nodes[i].type = types[i];
    }

    const std::string output = to_mermaid(graph, analysed(graph));
    for (const char* line : lines) {
        EXPECT_NE(output.find(line), std::string::npos) << line;
    }
    EXPECT_NE(output.find("  class n3,n6,n7,n10 sync\n"), std::string::npos);
}

TEST(Mermaid, CapEmitsTheWholeDiagramWithABanner) {
    Loaded diamond;
    ASSERT_TRUE(load_fixture("diamond.json", diamond));

    MermaidOptions options;
    options.max_nodes = 3;
    const std::string output = to_mermaid(diamond.graph, diamond.topo, options);

    // max_nodes counts drawn nodes after compression: four here, none dropped.
    const std::string banner =
        "flowchart LR\n"
        "  limit[\"4 nodes after compression, over --max-nodes 3: see the DOT output\"]\n";
    EXPECT_EQ(output.compare(0, banner.size(), banner), 0);
    for (int id = 0; id < 4; ++id) {
        const std::string node = "  n" + std::to_string(id) + "[\"kernel#" + std::to_string(id) +
                                 "\"]\n";
        EXPECT_NE(output.find(node), std::string::npos) << node;
    }
    EXPECT_EQ(count_of(output, "-->"), 4u);
    EXPECT_EQ(output.find("%%"), std::string::npos);
}

TEST(Mermaid, CapIsStrictAtTheBoundary) {
    Loaded diamond;
    ASSERT_TRUE(load_fixture("diamond.json", diamond));

    // Four drawn nodes: at the cap the diagram is complete and says nothing.
    MermaidOptions options;
    options.max_nodes = 4;
    EXPECT_EQ(to_mermaid(diamond.graph, diamond.topo, options).find("limit["), std::string::npos);

    // One less than drawn is over the cap, and the banner names the exact count.
    options.max_nodes = 3;
    const std::string output = to_mermaid(diamond.graph, diamond.topo, options);
    EXPECT_NE(output.find("  limit[\"4 nodes after compression, over --max-nodes 3: see the DOT "
                          "output\"]\n"),
              std::string::npos)
        << output;
}

TEST(Mermaid, ChainArtifactStaysUnderTheCap) {
    Loaded chain;
    ASSERT_TRUE(load(std::string(k_graph_dir) + "/chain.json", chain));

    // 1000 kernels drawn as one box: the cap counts drawn nodes, not model nodes.
    const std::string output = to_mermaid(chain.graph, chain.topo);
    EXPECT_EQ(output.find("limit["), std::string::npos);
    const std::string box = std::string("  n0[\"kernel#0") + k_times + "1000\"]\n";
    EXPECT_NE(output.find(box), std::string::npos) << box;
}

// The two emitters refuse a stale Topology on the pairs, not only on the total.
TEST(Emitters, MismatchedTopologyTripsTheAssert) {
    Graph graph = make_graph(3, NodeType::Kernel);
    graph.edges = {{0, 1}};
    Topology topo = analysed(graph);
    topo.edges_minimal = {{0, 2}};

    // One pair for one pair, so a count comparison cannot see it.
    EXPECT_DEATH((void)to_dot(graph, topo), "graphlint: assertion failed");
    EXPECT_DEATH((void)to_mermaid(graph, topo), "graphlint: assertion failed");
}

// The committed artifacts and the README figure: regenerate-and-compare, so neither
// can age alone.

TEST(Artifacts, DotMatchesTheCommittedFiles) {
    for (const char* name : {"built", "captured", "pingpong", "chain"}) {
        Loaded loaded;
        ASSERT_TRUE(load(std::string(k_graph_dir) + "/" + name + ".json", loaded)) << name;
        const std::string path = std::string(k_graph_dir) + "/" + name + ".dot";
        const std::string committed = slurp(path);
        ASSERT_FALSE(committed.empty()) << path;
        EXPECT_EQ(to_dot(loaded.graph, loaded.topo), committed) << path;
    }
}

TEST(Artifacts, MermaidMatchesTheCommittedFiles) {
    for (const char* name : {"built", "captured", "pingpong", "chain"}) {
        Loaded loaded;
        ASSERT_TRUE(load(std::string(k_graph_dir) + "/" + name + ".json", loaded)) << name;
        const std::string path = std::string(k_graph_dir) + "/" + name + ".mmd";
        const std::string committed = slurp(path);
        ASSERT_FALSE(committed.empty()) << path;
        EXPECT_EQ(to_mermaid(loaded.graph, loaded.topo), committed) << path;
    }
}

TEST(Artifacts, TwoCallsInOneProcessAreIdentical) {
    for (const char* name : {"built", "captured", "pingpong", "chain"}) {
        Loaded loaded;
        ASSERT_TRUE(load(std::string(k_graph_dir) + "/" + name + ".json", loaded)) << name;
        EXPECT_EQ(to_dot(loaded.graph, loaded.topo), to_dot(loaded.graph, loaded.topo)) << name;
        EXPECT_EQ(to_mermaid(loaded.graph, loaded.topo), to_mermaid(loaded.graph, loaded.topo))
            << name;
    }
}

TEST(Readme, FigureMatchesTheEmitter) {
    const std::string readme = slurp(std::string(k_source_dir) + "/README.md");
    ASSERT_FALSE(readme.empty()) << "cannot read README.md";

    const std::string fence = "```mermaid\n";
    const std::size_t begin = readme.find(fence);
    ASSERT_NE(begin, std::string::npos) << "no mermaid block in README.md";
    const std::size_t start = begin + fence.size();
    const std::size_t end = readme.find("```", start);
    ASSERT_NE(end, std::string::npos) << "unterminated mermaid block in README.md";

    Loaded captured;
    ASSERT_TRUE(load(std::string(k_graph_dir) + "/captured.json", captured));
    EXPECT_EQ(readme.substr(start, end - start), to_mermaid(captured.graph, captured.topo));
}
