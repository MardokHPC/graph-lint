#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

#include <tools/report_core.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using graphlint::Environment;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::GraphErrorKind;
using graphlint::LintResult;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::ReportDocument;
using graphlint::Severity;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::parse_graph;
using graphlint::parse_report;
using graphlint::topology;
using graphlint::tools::ReportFormat;
using graphlint::tools::ReportOptions;
using graphlint::tools::run_report;

constexpr const char* k_graph_dir = GRAPHLINT_GRAPH_DIR;

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

// The two-node graph of docs/report.md section 1, exactly as the contract draws it.
Graph built_two_node() {
    Graph graph;
    graph.source = SourceKind::Built;
    Node kernel = kernel_node(0);
    kernel.grid = {2, 2, 1};
    Node host;
    host.id = 1;
    host.type = NodeType::Host;
    graph.nodes = {kernel, host};
    graph.edges = {{0, 1}};
    return graph;
}

const char* const k_pinned_report =
    "{\n"
    "  \"analysis\": {\n"
    "    \"depth\": 2,\n"
    "    \"distinct_edges\": 1,\n"
    "    \"edges_minimal\": [\n"
    "      [0, 1]\n"
    "    ],\n"
    "    \"edges_redundant\": [],\n"
    "    \"width\": [1, 1]\n"
    "  },\n"
    "  \"applicability\": {\n"
    "    \"applicable\": [\n"
    "      {\"reason\": \"graph is a DAG\", \"rule\": \"dag-check\"},\n"
    "      {\"reason\": \"depth and node count are known\", \"rule\": \"launch-shaped\"},\n"
    "      {\"reason\": \"the reduction is exact on a DAG\", \"rule\": \"redundant-edge\"},\n"
    "      {\"reason\": \"node types are readable\", \"rule\": \"sync-node\"},\n"
    "      {\"reason\": \"successors and reachability are readable\", \"rule\": \"pointless-fork\"}\n"
    "    ],\n"
    "    \"not_applicable\": [\n"
    "      {\"reason\": \"no kernel name\", \"rule\": \"duplicate-launch\"},\n"
    "      {\"reason\": \"no mem_alloc/mem_free node\", \"rule\": \"mem-lifecycle\"}\n"
    "    ]\n"
    "  },\n"
    "  \"environment\": {\"cuda_runtime\": 0, \"device\": \"\"},\n"
    "  \"findings\": [\n"
    "    {\"message\": \"host node 1 runs a CPU callback inside the graph and serialises its "
    "stream\", \"nodes\": [1], \"rule\": \"sync-node\", \"severity\": \"severe\"}\n"
    "  ],\n"
    "  \"graph\": {\n"
    "    \"children\": [],\n"
    "    \"edges\": [\n"
    "      [0, 1]\n"
    "    ],\n"
    "    \"nodes\": [\n"
    "      {\"block\": [1, 1, 1], \"grid\": [2, 2, 1], \"id\": 0, \"type\": \"kernel\"},\n"
    "      {\"id\": 1, \"type\": \"host\"}\n"
    "    ],\n"
    "    \"schema_version\": 1,\n"
    "    \"source\": {\"kind\": \"built\"},\n"
    "    \"weight_kind\": \"levels\"\n"
    "  },\n"
    "  \"schema_version\": 1\n"
    "}\n";

std::string rendered(const Graph& graph) {
    Topology topo;
    GraphError error;
    EXPECT_TRUE(topology(graph, topo, error)) << error.detail;
    LintResult lint_result;
    static_cast<void>(graphlint::lint(graph, topo, error, lint_result));
    return graphlint::dump_report(graph, topo, lint_result, Environment{});
}

// A graph only a caller can build: parse_graph refuses a backwards edge before topology runs.
Graph refused_graph() {
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1)};
    graph.edges = {{1, 0}};
    return graph;
}

// A refused topology is a report too: zeroed analysis, one dag-check error.
std::string rendered_refused(const Graph& graph) {
    Topology topo;
    GraphError error;
    EXPECT_FALSE(topology(graph, topo, error)) << "topology was expected to refuse";
    LintResult lint_result;
    static_cast<void>(graphlint::lint(graph, topo, error, lint_result));
    return graphlint::dump_report(graph, topo, lint_result, Environment{});
}

int line_of(const std::string& text, std::string_view needle) {
    const std::size_t at = text.find(needle);
    EXPECT_NE(at, std::string::npos) << "missing " << needle;
    int line = 1;
    for (std::size_t i = 0; i < at; ++i) {
        if (text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

}  // namespace

TEST(Report, BuiltTwoNodeIsBytePinnedToTheContract) {
    const Graph graph = built_two_node();
    EXPECT_EQ(rendered(graph), std::string(k_pinned_report));
}

TEST(Report, RefusedGraphStillReports) {
    const std::string text = rendered_refused(refused_graph());
    EXPECT_NE(text.find("\"depth\": 0"), std::string::npos);
    EXPECT_NE(text.find("\"edges_minimal\": []"), std::string::npos);
    EXPECT_NE(text.find("\"width\": []"), std::string::npos);
    EXPECT_NE(text.find("\"rule\": \"dag-check\", \"severity\": \"error\""), std::string::npos);
    // Six not_applicable entries, one per surviving rule.
    std::size_t entries = 0;
    const std::string reason = "\"reason\": \"graph failed dag-check\"";
    for (std::size_t at = text.find(reason); at != std::string::npos;
         at = text.find(reason, at + reason.size())) {
        ++entries;
    }
    EXPECT_EQ(entries, 6u);
}

TEST(Report, ParserRoundTripsTheWriter) {
    const Graph graph = built_two_node();
    const std::string text = rendered(graph);

    ReportDocument document;
    GraphError error;
    ASSERT_TRUE(parse_report(text, document, error))
        << error.line << ":" << error.column << " " << error.detail;
    EXPECT_FALSE(error);
    EXPECT_EQ(document.graph, graph);
    EXPECT_EQ(document.schema_version, 1);
    EXPECT_EQ(document.environment.device, "");
    EXPECT_EQ(document.environment.cuda_runtime, 0);
    EXPECT_EQ(document.analysis.depth, 2);
    EXPECT_EQ(document.analysis.distinct_edges, 1);
    EXPECT_EQ(document.analysis.edges_minimal, (std::vector<std::pair<int, int>>{{0, 1}}));
    EXPECT_TRUE(document.analysis.edges_redundant.empty());
    EXPECT_EQ(document.analysis.width, (std::vector<int>{1, 1}));
    ASSERT_EQ(document.findings.size(), 1u);
    EXPECT_EQ(document.findings[0].rule, graphlint::RuleId::SyncNode);
    EXPECT_EQ(document.findings[0].severity, Severity::severe);
    ASSERT_EQ(document.rules.size(), 7u);
    EXPECT_TRUE(document.rules[0].applicable);
}

TEST(Report, ParserRejectsAnUnknownKey) {
    std::string text = k_pinned_report;
    const std::size_t at = text.find("  \"environment\"");
    ASSERT_NE(at, std::string::npos);
    text.insert(at, "  \"bogus\": 1,\n");

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::UnknownKey);
    EXPECT_EQ(error.line, line_of(text, "\"bogus\""));
    EXPECT_EQ(error.detail, "unknown key \"bogus\"");
}

TEST(Report, ParserRejectsSchemaVersionTwo) {
    std::string text = k_pinned_report;
    const std::size_t at = text.rfind("  \"schema_version\": 1");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("  \"schema_version\": 1").size(), "  \"schema_version\": 2");

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::SchemaVersion);
    EXPECT_GT(error.line, 0);
}

TEST(Report, ParserRejectsATruncatedDocument) {
    const std::string text = std::string(k_pinned_report).substr(0, 120);

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_GT(error.line, 0);
    EXPECT_GT(error.column, 0);
    EXPECT_FALSE(error.detail.empty());
}

TEST(Report, ParserRejectsTrailingBytes) {
    const std::string text = std::string(k_pinned_report) + "{}";

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::Trailing);
}

TEST(Report, ParserMapsAGraphErrorIntoTheReportCoordinates) {
    std::string text = k_pinned_report;
    const std::size_t at = text.find("\"id\": 0");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"id\": 0").size(), "\"id\": 9");

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::IdNotDense);
    // The nested position is the report's own: the line that carries `"id": 9`.
    EXPECT_EQ(error.line, line_of(text, "\"id\": 9"));
}

TEST(Report, ParserMapsTheNestedColumnOfASingleLineGraph) {
    // The graph object must start after column 1, or the `- 1` of the mapping is invisible.
    std::string text = k_pinned_report;
    const std::size_t start = text.find("  \"graph\": {");
    const std::size_t end = text.find("\n  \"schema_version\": 1");
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    text.replace(start, end - start, "  \"graph\": {\"bad\": 1},");

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::UnknownKey);
    EXPECT_EQ(error.detail, "unknown key \"bad\"");
    EXPECT_EQ(error.line, line_of(text, "\"graph\""));

    const std::size_t at = text.find("\"bad\"");
    ASSERT_NE(at, std::string::npos);
    const std::size_t line_start = text.rfind('\n', at) + 1;
    const int expected_column = static_cast<int>(at - line_start) + 1;
    EXPECT_GT(expected_column, 1);
    EXPECT_EQ(error.column, expected_column);
}

TEST(Report, ParserRejectsAGraphDocument) {
    const std::string text = slurp(std::string(k_graph_dir) + "/built.json");
    ASSERT_FALSE(text.empty());

    ReportDocument document;
    GraphError error;
    EXPECT_FALSE(parse_report(text, document, error));
    EXPECT_EQ(error.kind, GraphErrorKind::UnknownKey);
    EXPECT_EQ(error.detail, "unknown key \"children\"");
    EXPECT_EQ(error.line, 2);
}

TEST(Report, WidthHasOneEntryPerLevel) {
    const std::string text = slurp(std::string(k_graph_dir) + "/built.json");
    ASSERT_FALSE(text.empty());
    Graph graph;
    GraphError error;
    ASSERT_TRUE(parse_graph(text, graph, error));

    ReportDocument document;
    ASSERT_TRUE(parse_report(rendered(graph), document, error))
        << error.line << ":" << error.column << " " << error.detail;
    EXPECT_GT(document.analysis.depth, 1);
    EXPECT_EQ(document.analysis.width.size(),
              static_cast<std::size_t>(document.analysis.depth));
}

// ---------------------------------------------------------------------------
// The CLI body: the exit policy is a function, not a branch only a file can reach.
// ---------------------------------------------------------------------------

TEST(ReportCli, AnErrorFindingStillProducesASummaryAndExitsZero) {
    ReportOptions options;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_report(refused_graph(), options, out, err), 0);
    EXPECT_NE(out.str().find("finding: error dag-check"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(ReportCli, AnErrorFindingStillProducesJsonAndExitsZero) {
    ReportOptions options;
    options.format = ReportFormat::json;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_report(refused_graph(), options, out, err), 0);
    EXPECT_NE(out.str().find("\"rule\": \"dag-check\", \"severity\": \"error\""),
              std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(ReportCli, DotOnARefusedGraphExitsTwo) {
    ReportOptions options;
    options.format = ReportFormat::dot;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_report(refused_graph(), options, out, err), 2);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("edge [1, 0] is not from < to"), std::string::npos);
}

TEST(ReportCli, MermaidOnARefusedGraphExitsTwo) {
    ReportOptions options;
    options.format = ReportFormat::mermaid;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_report(refused_graph(), options, out, err), 2);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("edge [1, 0] is not from < to"), std::string::npos);
}

TEST(ReportCli, JsonCarriesTheEnvironment) {
    ReportOptions options;
    options.format = ReportFormat::json;
    options.environment.device = "A100";
    options.environment.cuda_runtime = 12040;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_report(built_two_node(), options, out, err), 0);
    EXPECT_NE(out.str().find("\"cuda_runtime\": 12040"), std::string::npos);
    EXPECT_NE(out.str().find("\"device\": \"A100\""), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}
