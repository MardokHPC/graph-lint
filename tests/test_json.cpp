#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

// The golden document, byte for byte: writer, parser and the committed file all
// compare against this literal, so a format change cannot move both sides.
const char* const k_golden_document = R"json({
  "children": [
    {
      "children": [],
      "edges": [],
      "nodes": [
        {"block": [32, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}
      ]
    }
  ],
  "edges": [
    [0, 1],
    [0, 3],
    [1, 3],
    [2, 4],
    [3, 4]
  ],
  "nodes": [
    {"block": [128, 1, 1], "grid": [14, 14, 1], "id": 0, "shared_bytes": 8192, "type": "kernel"},
    {"block": [256, 1, 1], "grid": [1, 1, 1], "id": 1, "type": "kernel"},
    {"alloc_bytes": 1048576, "id": 2, "mem_ptr": "0x7f0000000000", "type": "mem_alloc"},
    {"id": 3, "mem_ptr": "0x7f0000000000", "type": "mem_free"},
    {"child": 0, "id": 4, "type": "child_graph"}
  ],
  "schema_version": 1,
  "source": {"kind": "file"},
  "weight_kind": "levels"
}
)json";

const char* const k_one_kernel =
    R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}])";
const char* const k_one_weighted_kernel =
    R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel", "weight": 5}])";

std::string read_fixture(const std::string& name) {
    const std::string path = std::string(GRAPHLINT_TEST_DATA_DIR) + "/" + name;
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open()) << path;
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

std::string document(const std::string& nodes, const std::string& edges = "[]",
                     const std::string& weight_kind = "levels",
                     const std::string& children = "[]") {
    return "{\"children\": " + children + ", \"edges\": " + edges + ", \"nodes\": " + nodes +
           ", \"schema_version\": 1, \"source\": {\"kind\": \"file\"}, \"weight_kind\": \"" +
           weight_kind + "\"}";
}

std::string two_kernel_nodes() {
    return R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}, )"
           R"({"block": [1, 1, 1], "grid": [1, 1, 1], "id": 1, "type": "kernel"}])";
}

std::string named_kernel(const std::string& escaped_name) {
    return "[{\"block\": [1, 1, 1], \"grid\": [1, 1, 1], \"id\": 0, \"name\": \"" + escaped_name +
           "\", \"type\": \"kernel\"}]";
}

graphlint::Graph sentinel_graph() {
    graphlint::Graph graph;
    graphlint::Node node;
    node.id = 0;
    node.type = graphlint::NodeType::Empty;
    graph.nodes.push_back(node);
    graph.source = graphlint::SourceKind::Built;
    return graph;
}

void expect_rejected(std::string_view text, graphlint::GraphErrorKind expected) {
    const graphlint::Graph sentinel = sentinel_graph();
    graphlint::Graph out = sentinel;
    graphlint::GraphError error;
    EXPECT_FALSE(graphlint::parse_graph(text, out, error));
    EXPECT_EQ(error.kind, expected)
        << "line " << error.line << " column " << error.column << ": " << error.detail;
    EXPECT_GE(error.line, 1);
    EXPECT_TRUE(out == sentinel);
}

std::string leaf_graph() {
    return R"({"children": [], "edges": [], "nodes": []})";
}

std::string wrapper_graph(const std::string& child) {
    return "{\"children\": [" + child +
           "], \"edges\": [], \"nodes\": [{\"child\": 0, \"id\": 0, \"type\": "
           "\"child_graph\"}]}";
}

std::string nested_document(int total_levels) {
    std::string child = leaf_graph();
    for (int level = 2; level < total_levels; ++level) {
        child = wrapper_graph(child);
    }
    return "{\"children\": [" + child +
           "], \"edges\": [], \"nodes\": [{\"child\": 0, \"id\": 0, \"type\": "
           "\"child_graph\"}], \"schema_version\": 1, \"source\": {\"kind\": \"file\"}, "
           "\"weight_kind\": \"levels\"}";
}

graphlint::Graph golden_graph() {
    graphlint::Graph graph;
    graph.source = graphlint::SourceKind::File;
    graph.weight_kind = graphlint::WeightKind::Levels;

    graphlint::Node kernel0;
    kernel0.id = 0;
    kernel0.type = graphlint::NodeType::Kernel;
    kernel0.grid = {14, 14, 1};
    kernel0.block = {128, 1, 1};
    kernel0.shared_bytes = 8192;

    graphlint::Node kernel1;
    kernel1.id = 1;
    kernel1.type = graphlint::NodeType::Kernel;
    kernel1.grid = {1, 1, 1};
    kernel1.block = {256, 1, 1};

    graphlint::Node alloc;
    alloc.id = 2;
    alloc.type = graphlint::NodeType::MemAlloc;
    alloc.alloc_bytes = 1048576;
    alloc.mem_ptr = 0x7f0000000000ull;

    graphlint::Node release;
    release.id = 3;
    release.type = graphlint::NodeType::MemFree;
    release.mem_ptr = 0x7f0000000000ull;

    graphlint::Node child_node;
    child_node.id = 4;
    child_node.type = graphlint::NodeType::ChildGraph;
    child_node.child = 0;

    graph.nodes = {kernel0, kernel1, alloc, release, child_node};
    graph.edges = {{0, 1}, {0, 3}, {1, 3}, {2, 4}, {3, 4}};

    graphlint::Node child_kernel;
    child_kernel.id = 0;
    child_kernel.type = graphlint::NodeType::Kernel;
    child_kernel.grid = {1, 1, 1};
    child_kernel.block = {32, 1, 1};
    graphlint::Graph child;
    child.source = graphlint::SourceKind::File;
    child.weight_kind = graphlint::WeightKind::Levels;
    child.nodes.push_back(child_kernel);
    graph.children.push_back(child);
    return graph;
}

}  // namespace

TEST(Json, EmitsTheGoldenDocument) {
    EXPECT_EQ(graphlint::dump_graph(golden_graph()), k_golden_document);
    EXPECT_EQ(read_fixture("mixed.json"), k_golden_document);
}

TEST(Json, RoundTripIsByteIdenticalOnEveryFixture) {
    const char* const fixtures[] = {"mixed.json",  "empty.json",     "chain8.json",
                                    "diamond.json", "ns.json",       "child_graph.json"};
    for (const char* name : fixtures) {
        const std::string text = read_fixture(name);
        ASSERT_FALSE(text.empty()) << name;

        graphlint::Graph graph;
        graphlint::GraphError error;
        ASSERT_TRUE(graphlint::parse_graph(text, graph, error))
            << name << ": " << error.line << ":" << error.column << " " << error.detail;

        const std::string emitted = graphlint::dump_graph(graph);
        EXPECT_EQ(emitted, text) << name;

        graphlint::Graph reparsed;
        ASSERT_TRUE(graphlint::parse_graph(emitted, reparsed, error)) << name;
        EXPECT_TRUE(reparsed == graph) << name;
    }
}

TEST(Json, DuplicateEdgesSurviveTheRoundTrip) {
    graphlint::Graph graph;
    graphlint::Node first;
    first.id = 0;
    first.type = graphlint::NodeType::Kernel;
    first.grid = {1, 1, 1};
    first.block = {1, 1, 1};
    graphlint::Node second = first;
    second.id = 1;
    graph.nodes = {first, second};
    graph.edges = {{0, 1}, {0, 1}};

    const std::string text = graphlint::dump_graph(graph);
    std::size_t count = 0;
    for (std::size_t at = text.find("[0, 1]"); at != std::string::npos;
         at = text.find("[0, 1]", at + 1)) {
        ++count;
    }
    EXPECT_EQ(count, 2u);

    graphlint::Graph parsed;
    graphlint::GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(text, parsed, error));
    ASSERT_EQ(parsed.edges.size(), 2u);
    EXPECT_TRUE(parsed == graph);
}

TEST(Json, EdgesAreSortedAsPairs) {
    graphlint::Graph graph;
    for (int i = 0; i < 4; ++i) {
        graphlint::Node node;
        node.id = i;
        node.type = graphlint::NodeType::Kernel;
        node.grid = {1, 1, 1};
        node.block = {1, 1, 1};
        graph.nodes.push_back(node);
    }
    graph.edges = {{2, 3}, {0, 3}, {1, 2}, {0, 2}, {0, 1}};

    const std::string text = graphlint::dump_graph(graph);
    const char* const pairs[] = {"[0, 1]", "[0, 2]", "[0, 3]", "[1, 2]", "[2, 3]"};
    std::size_t previous = 0;
    for (const char* pair : pairs) {
        const std::size_t at = text.find(pair);
        ASSERT_NE(at, std::string::npos) << pair;
        EXPECT_GT(at, previous) << pair;
        previous = at;
    }
}

TEST(Json, RejectsABackwardEdge) {
    expect_rejected(document(two_kernel_nodes(), "[[1, 0]]"),
                    graphlint::GraphErrorKind::EdgeBackwards);
}

TEST(Json, RejectsASelfLoop) {
    expect_rejected(document(two_kernel_nodes(), "[[1, 1]]"),
                    graphlint::GraphErrorKind::EdgeBackwards);
}

TEST(Json, RejectsAnEdgeToAMissingId) {
    expect_rejected(document(two_kernel_nodes(), "[[0, 5]]"),
                    graphlint::GraphErrorKind::EdgeOutOfRange);
}

TEST(Json, RejectsADenseIdGap) {
    const std::string nodes =
        R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}, )"
        R"({"block": [1, 1, 1], "grid": [1, 1, 1], "id": 2, "type": "kernel"}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::IdNotDense);
}

TEST(Json, RejectsADuplicateId) {
    const std::string nodes =
        R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}, )"
        R"({"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0, "type": "kernel"}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::IdNotDense);
}

TEST(Json, RejectsAChildThatNoNodeReferences) {
    const std::string children = R"([{"children": [], "edges": [], "nodes": []}])";
    expect_rejected(document(k_one_kernel, "[]", "levels", children),
                    graphlint::GraphErrorKind::ChildMismatch);
}

TEST(Json, RejectsAChildIndexOutOfRange) {
    const std::string children = R"([{"children": [], "edges": [], "nodes": []}])";
    const std::string nodes = R"([{"child": 2, "id": 0, "type": "child_graph"}])";
    expect_rejected(document(nodes, "[]", "levels", children),
                    graphlint::GraphErrorKind::ChildMismatch);
}

TEST(Json, RejectsAWeightWhenKindIsLevels) {
    expect_rejected(document(k_one_weighted_kernel),
                    graphlint::GraphErrorKind::WeightRule);
}

TEST(Json, RequiresAWeightWhenKindIsNanoseconds) {
    expect_rejected(document(k_one_kernel, "[]", "ns"),
                    graphlint::GraphErrorKind::WeightRule);
}

TEST(Json, RejectsAnotherSchemaVersion) {
    const std::string text =
        "{\"children\": [], \"edges\": [], \"nodes\": [], \"schema_version\": 2, "
        "\"source\": {\"kind\": \"file\"}, \"weight_kind\": \"levels\"}";
    expect_rejected(text, graphlint::GraphErrorKind::SchemaVersion);
}

TEST(Json, RejectsAMissingRequiredKey) {
    const std::string without_edges =
        "{\"children\": [], \"nodes\": [], \"schema_version\": 1, "
        "\"source\": {\"kind\": \"file\"}, \"weight_kind\": \"levels\"}";
    expect_rejected(without_edges, graphlint::GraphErrorKind::MissingKey);

    const std::string nodes = R"([{"block": [1, 1, 1], "grid": [1, 1, 1], "id": 0}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::MissingKey);
}

TEST(Json, ReportsLineAndColumnOfAnUnsortedKey) {
    const std::string text =
        "{\n"
        "  \"children\": [],\n"
        "  \"edges\": [],\n"
        "  \"nodes\": [\n"
        "    {\"id\": 0, \"block\": [1, 1, 1], \"grid\": [1, 1, 1], \"type\": \"kernel\"}\n"
        "  ],\n"
        "  \"schema_version\": 1,\n"
        "  \"source\": {\"kind\": \"file\"},\n"
        "  \"weight_kind\": \"levels\"\n"
        "}\n";

    const graphlint::Graph sentinel = sentinel_graph();
    graphlint::Graph out = sentinel;
    graphlint::GraphError error;
    ASSERT_FALSE(graphlint::parse_graph(text, out, error));
    EXPECT_EQ(error.kind, graphlint::GraphErrorKind::UnsortedKey);
    EXPECT_EQ(error.line, 5);
    EXPECT_EQ(error.column, 15);
    EXPECT_NE(error.detail.find("unsorted key \"block\""), std::string::npos);
    EXPECT_TRUE(out == sentinel);
}

TEST(Json, OmitsAnEmptyNameAndAcceptsItsAbsence) {
    graphlint::Graph graph;
    graphlint::Node node;
    node.id = 0;
    node.type = graphlint::NodeType::Kernel;
    node.grid = {1, 1, 1};
    node.block = {1, 1, 1};
    graph.nodes.push_back(node);

    const std::string text = graphlint::dump_graph(graph);
    EXPECT_EQ(text.find("\"name\""), std::string::npos);

    graphlint::Graph parsed;
    graphlint::GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(text, parsed, error));
    EXPECT_TRUE(parsed.nodes[0].name.empty());
    EXPECT_TRUE(parsed == graph);
}

TEST(Json, RejectsAnUnknownKey) {
    const std::string nodes =
        R"([{"block": [1, 1, 1], "colour": "red", "grid": [1, 1, 1], "id": 0, "type": "kernel"}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::UnknownKey);
}

TEST(Json, RejectsTrailingBytes) {
    expect_rejected(document(k_one_kernel) + "x", graphlint::GraphErrorKind::Trailing);
}

TEST(Json, RejectsNestingDeeperThanEight) {
    graphlint::Graph accepted;
    graphlint::GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(nested_document(8), accepted, error))
        << error.line << ":" << error.column << " " << error.detail;
    expect_rejected(nested_document(9), graphlint::GraphErrorKind::TooDeep);
}

TEST(Json, RejectsADuplicateKey) {
    const std::string text =
        "{\"children\": [], \"edges\": [], \"edges\": [], \"nodes\": [], "
        "\"schema_version\": 1, \"source\": {\"kind\": \"file\"}, "
        "\"weight_kind\": \"levels\"}";
    expect_rejected(text, graphlint::GraphErrorKind::DuplicateKey);
}

TEST(Json, RejectsMalformedJson) {
    expect_rejected("{\"children\": [}", graphlint::GraphErrorKind::Syntax);
}

TEST(Json, RejectsAStringWhereANumberIsRequired) {
    const std::string text =
        "{\"children\": [], \"edges\": [], \"nodes\": [], \"schema_version\": \"1\", "
        "\"source\": {\"kind\": \"file\"}, \"weight_kind\": \"levels\"}";
    expect_rejected(text, graphlint::GraphErrorKind::WrongType);
}

TEST(Json, RejectsAFloatWhereAnIntegerIsRequired) {
    const std::string text =
        "{\"children\": [], \"edges\": [], \"nodes\": [], \"schema_version\": 1.5, "
        "\"source\": {\"kind\": \"file\"}, \"weight_kind\": \"levels\"}";
    expect_rejected(text, graphlint::GraphErrorKind::BadNumber);
}

TEST(Json, RejectsAnUnknownNodeType) {
    const std::string nodes = R"([{"id": 0, "type": "frobnicator"}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::UnknownType);
}

TEST(Json, RejectsAFieldForTheWrongNodeType) {
    const std::string nodes =
        R"([{"alloc_bytes": 16, "id": 0, "mem_ptr": "0x10", "type": "mem_free"}])";
    expect_rejected(document(nodes), graphlint::GraphErrorKind::FieldForType);
}

TEST(Json, EscapesQuotesBackslashesAndControlBytes) {
    graphlint::Graph graph;
    graphlint::Node node;
    node.id = 0;
    node.type = graphlint::NodeType::Kernel;
    node.name = "a\"b\\c" "\x01" "d\xc3\xa9";
    node.grid = {1, 1, 1};
    node.block = {1, 1, 1};
    graph.nodes.push_back(node);

    const std::string text = graphlint::dump_graph(graph);
    EXPECT_NE(text.find("\\\"b"), std::string::npos);
    EXPECT_NE(text.find("\\\\c"), std::string::npos);
    EXPECT_NE(text.find("\\u0001"), std::string::npos);
    EXPECT_NE(text.find("\xc3\xa9"), std::string::npos);

    graphlint::Graph parsed;
    graphlint::GraphError error;
    ASSERT_TRUE(graphlint::parse_graph(text, parsed, error));
    EXPECT_EQ(parsed.nodes[0].name, node.name);
    EXPECT_TRUE(parsed == graph);
}

TEST(Json, SurrogatesMustBePaired) {
    graphlint::Graph out;
    graphlint::GraphError error;

    EXPECT_FALSE(graphlint::parse_graph(document(named_kernel("\\ud800")), out, error));
    EXPECT_EQ(error.kind, graphlint::GraphErrorKind::Syntax);
    EXPECT_NE(error.detail.find("low surrogate"), std::string::npos) << error.detail;

    EXPECT_FALSE(graphlint::parse_graph(document(named_kernel("\\ud800\\u0041")), out, error));
    EXPECT_EQ(error.kind, graphlint::GraphErrorKind::Syntax);
    EXPECT_NE(error.detail.find("low surrogate"), std::string::npos) << error.detail;

    EXPECT_FALSE(graphlint::parse_graph(document(named_kernel("\\udc00")), out, error));
    EXPECT_EQ(error.kind, graphlint::GraphErrorKind::Syntax);
    EXPECT_NE(error.detail.find("high surrogate"), std::string::npos) << error.detail;

    // U+1F600: the pair becomes four UTF-8 bytes and survives a round trip.
    graphlint::Graph pair;
    ASSERT_TRUE(graphlint::parse_graph(document(named_kernel("\\ud83d\\ude00")), pair, error))
        << error.line << ":" << error.column << " " << error.detail;
    EXPECT_EQ(pair.nodes[0].name, "\xf0\x9f\x98\x80");
    const std::string dumped = graphlint::dump_graph(pair);
    EXPECT_NE(dumped.find("\xf0\x9f\x98\x80"), std::string::npos);
    graphlint::Graph reloaded;
    ASSERT_TRUE(graphlint::parse_graph(dumped, reloaded, error)) << error.detail;
    EXPECT_TRUE(reloaded == pair);
}
