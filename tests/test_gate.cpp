#include <graphlint/analysis.hpp>
#include <graphlint/gate.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using graphlint::Environment;
using graphlint::GateResult;
using graphlint::GateThresholds;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::LintResult;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::ReportDocument;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::Verdict;
using graphlint::WeightKind;

constexpr const char* k_data_dir = GRAPHLINT_TEST_DATA_DIR;

Node kernel_node(int id) {
    Node node;
    node.id = id;
    node.type = NodeType::Kernel;
    node.grid = {1, 1, 1};
    node.block = {1, 1, 1};
    return node;
}

Node host_node(int id) {
    Node node;
    node.id = id;
    node.type = NodeType::Host;
    return node;
}

Graph chain(int count) {
    Graph graph;
    graph.source = SourceKind::Built;
    for (int i = 0; i < count; ++i) {
        graph.nodes.push_back(kernel_node(i));
    }
    for (int i = 0; i + 1 < count; ++i) {
        graph.edges.emplace_back(i, i + 1);
    }
    return graph;
}

Graph chain_with_host(int count) {
    Graph graph = chain(count - 1);
    graph.nodes.push_back(host_node(count - 1));
    graph.edges.emplace_back(count - 2, count - 1);
    return graph;
}

Graph diamond(bool extra_edge) {
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1), kernel_node(2), kernel_node(3)};
    graph.edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    if (extra_edge) {
        graph.edges.emplace_back(0, 3);  // implied by 0 -> 1 -> 3
    }
    return graph;
}

// Declared blocks filled from the recomputed ones: a fixture that only tests the gate, not
// the writer's staleness note.
ReportDocument document_of(const Graph& graph, const Environment& environment = {}) {
    Topology topo;
    GraphError error;
    const bool analysed = graphlint::topology(graph, topo, error);
    EXPECT_TRUE(analysed) << error.detail;
    LintResult lint_result;
    static_cast<void>(graphlint::lint(graph, topo, error, lint_result));

    ReportDocument document;
    document.graph = graph;
    document.environment = environment;
    document.findings = lint_result.findings;
    document.rules = lint_result.rules;
    document.analysis.depth = topo.depth;
    document.analysis.distinct_edges = topo.distinct_edges;
    document.analysis.edges_minimal = topo.edges_minimal;
    document.analysis.edges_redundant = topo.edges_redundant;
    document.analysis.width = topo.width;
    if (graph.weight_kind == WeightKind::Nanoseconds) {
        graphlint::CriticalPath path;
        GraphError path_error;
        EXPECT_TRUE(graphlint::critical_path(graph, topo, path, path_error)) << path_error.detail;
        document.analysis.path = path.nodes;
        document.analysis.path_ns = path.weight_sum;
        document.analysis.has_path = true;
    }
    return document;
}

// A chain of `count` kernels with one weight per node: the weighted twin of chain().
Graph weighted_chain(const std::vector<std::uint64_t>& weights) {
    Graph graph = chain(static_cast<int>(weights.size()));
    graph.weight_kind = WeightKind::Nanoseconds;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        graph.nodes[i].weight = weights[i];
    }
    return graph;
}

// A graph topology refuses can only be caller-built, so parse_report cannot produce it.
ReportDocument refused_document() {
    Graph graph;
    graph.nodes = {kernel_node(0), kernel_node(1)};
    graph.edges = {{1, 0}};
    Topology topo;
    GraphError error;
    EXPECT_FALSE(graphlint::topology(graph, topo, error));
    LintResult lint_result;
    static_cast<void>(graphlint::lint(graph, topo, error, lint_result));

    ReportDocument document;
    document.graph = std::move(graph);
    document.findings = lint_result.findings;
    document.rules = lint_result.rules;
    return document;
}

bool load_report(const std::string& path, ReportDocument& document, GraphError& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read " << path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return graphlint::parse_report(buffer.str(), document, error);
}

bool mentions(const std::vector<std::string>& notes, const std::string& needle) {
    for (const std::string& note : notes) {
        if (note.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(Gate, SameDocumentPassesWithANote) {
    const ReportDocument document = document_of(chain(4));
    const GateResult result = graphlint::compare(document, document, GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::pass);
    EXPECT_EQ(result.reason, "all metrics inside their thresholds");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 0);
    ASSERT_EQ(result.notes.size(), 1u);
    EXPECT_EQ(result.notes[0], "baseline and current carry the same graph");
}

TEST(Gate, DepthIncreaseIsARegression) {
    const GateResult result =
        graphlint::compare(document_of(chain(4)), document_of(chain(5)), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "depth grew 1 > 0");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 1);
    EXPECT_EQ(result.metrics[0].name, "depth");
    EXPECT_EQ(result.metrics[0].status, graphlint::MetricStatus::regression);
}

TEST(Gate, RedundantEdgeIncreaseIsARegression) {
    const GateResult result = graphlint::compare(document_of(diamond(false)),
                                                 document_of(diamond(true)), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "edges_redundant grew 1 > 0");
}

TEST(Gate, SevereIncreaseIsARegression) {
    const GateResult result = graphlint::compare(document_of(chain(4)),
                                                 document_of(chain_with_host(4)),
                                                 GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "findings_severe grew 1 > 0");
}

TEST(Gate, OtherSourceIsSuspect) {
    Graph capture = chain(4);
    capture.source = SourceKind::Capture;
    const GateResult result =
        graphlint::compare(document_of(chain(4)), document_of(capture), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::suspect);
    EXPECT_EQ(result.reason, "different source kind: built vs capture");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 2);
}

TEST(Gate, MixedWeightKindsAreSuspect) {
    Graph nanoseconds = chain(4);
    nanoseconds.weight_kind = WeightKind::Nanoseconds;
    const GateResult result =
        graphlint::compare(document_of(chain(4)), document_of(nanoseconds), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::suspect);
    EXPECT_EQ(result.reason, "different weight_kind: levels vs ns");
}

TEST(Gate, ErrorFindingIsSuspect) {
    const GateResult result =
        graphlint::compare(refused_document(), document_of(chain(4)), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::suspect);
    EXPECT_NE(result.reason.find("baseline carries an error finding"), std::string::npos);
}

TEST(Gate, DifferentKnownDeviceIsSuspect) {
    const GateResult result = graphlint::compare(document_of(chain(4), {"A100", 12040}),
                                                 document_of(chain(4), {"H100", 12040}),
                                                 GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::suspect);
    EXPECT_EQ(result.reason, "different device: A100 vs H100");
}

TEST(Gate, DepthDecreaseIsAnImprovement) {
    const GateResult result =
        graphlint::compare(document_of(chain(5)), document_of(chain(4)), GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::improvement);
    EXPECT_EQ(result.reason, "depth fell 1 > 0");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 0);
}

TEST(Gate, DeltaEqualToTheThresholdIsInside) {
    GateThresholds thresholds;
    thresholds.max_depth_increase = 1;
    const GateResult at_boundary =
        graphlint::compare(document_of(chain(4)), document_of(chain(5)), thresholds);
    EXPECT_EQ(at_boundary.verdict, Verdict::pass);
    EXPECT_EQ(at_boundary.metrics[0].status, graphlint::MetricStatus::none);

    thresholds.max_depth_increase = 2;
    EXPECT_EQ(graphlint::compare(document_of(chain(4)), document_of(chain(5)), thresholds).verdict,
              Verdict::pass);

    // Symmetric on the improvement side: -1 is inside a threshold of 1.
    thresholds.max_depth_increase = 1;
    EXPECT_EQ(graphlint::compare(document_of(chain(5)), document_of(chain(4)), thresholds).verdict,
              Verdict::pass);
}

TEST(Gate, ThresholdsAreWiredToTheirOwnMetric) {
    GateThresholds thresholds;
    thresholds.max_depth_increase = 0;
    thresholds.max_redundant_increase = 1;
    thresholds.max_severe_increase = 1;

    // Redundant +1 and severe +1 are inside their own thresholds; wiring either to the depth
    // threshold of 0 turns both into a regression and fails here.
    EXPECT_EQ(graphlint::compare(document_of(diamond(false)), document_of(diamond(true)),
                                 thresholds).verdict,
              Verdict::pass);
    EXPECT_EQ(graphlint::compare(document_of(chain(4)), document_of(chain_with_host(4)),
                                 thresholds).verdict,
              Verdict::pass);

    thresholds.max_redundant_increase = 0;
    thresholds.max_severe_increase = 0;
    EXPECT_EQ(graphlint::compare(document_of(diamond(false)), document_of(diamond(true)),
                                 thresholds).verdict,
              Verdict::regression);
    EXPECT_EQ(graphlint::compare(document_of(chain(4)), document_of(chain_with_host(4)),
                                 thresholds).verdict,
              Verdict::regression);
}

TEST(Gate, OneLevelOverTheThresholdIsARegression) {
    GateThresholds thresholds;
    thresholds.max_depth_increase = 1;
    const GateResult result =
        graphlint::compare(document_of(chain(4)), document_of(chain(6)), thresholds);

    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "depth grew 2 > 1");
}

TEST(Gate, RegressionWinsOverAnImprovement) {
    Graph deeper_with_host = chain(5);
    deeper_with_host.nodes.push_back(host_node(5));
    deeper_with_host.edges.emplace_back(4, 5);

    Graph shallower_two_hosts = chain(4);
    shallower_two_hosts.nodes.push_back(host_node(4));
    shallower_two_hosts.nodes.push_back(host_node(5));
    shallower_two_hosts.edges.emplace_back(3, 4);
    shallower_two_hosts.edges.emplace_back(3, 5);

    const GateResult result = graphlint::compare(document_of(deeper_with_host),
                                                 document_of(shallower_two_hosts),
                                                 GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "findings_severe grew 1 > 0");
}

TEST(Gate, StaleDeclarationDoesNotDecide) {
    // Baseline declares depth 1 for a 17-level graph, current declares the true 18: a gate
    // reading the declaration would say `depth grew 17 > 0`, the graphs say `1`.
    ReportDocument baseline = document_of(chain(17));
    ReportDocument current = document_of(chain(18));
    baseline.analysis.depth = 1;

    const GateResult result = graphlint::compare(baseline, current, GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "depth grew 1 > 0");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 1);
    EXPECT_TRUE(mentions(result.notes, "baseline: declared analysis disagrees"));
    EXPECT_FALSE(mentions(result.notes, "current: declared analysis disagrees"));
}

TEST(Gate, SameDeclarationDifferentGraphs) {
    // Both declare depth 1, so a gate reading the declaration sees a delta of 0 and passes.
    ReportDocument baseline = document_of(chain(17));
    ReportDocument current = document_of(chain(18));
    baseline.analysis.depth = 1;
    current.analysis.depth = 1;

    const GateResult result = graphlint::compare(baseline, current, GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::regression);
    EXPECT_EQ(result.reason, "depth grew 1 > 0");
    EXPECT_EQ(graphlint::exit_code(result.verdict), 1);
    EXPECT_TRUE(mentions(result.notes, "baseline: declared analysis disagrees"));
    EXPECT_TRUE(mentions(result.notes, "current: declared analysis disagrees"));
}

TEST(Gate, DeclaredFindingsDoNotDecide) {
    ReportDocument baseline = document_of(chain_with_host(4));
    ReportDocument current = baseline;
    current.findings.clear();
    const GateResult result = graphlint::compare(baseline, current, GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::pass);
    EXPECT_TRUE(mentions(result.notes, "declared findings disagree"));
}

TEST(Gate, UnknownDeviceAndRuntimeAreNotesOnly) {
    const GateResult result = graphlint::compare(document_of(chain(4), {"A100", 12040}),
                                                 document_of(chain(4), {"", 12060}),
                                                 GateThresholds{});

    EXPECT_EQ(result.verdict, Verdict::pass);
    EXPECT_TRUE(mentions(result.notes, "device unknown on current"));
    EXPECT_TRUE(mentions(result.notes, "cuda_runtime differs: 12040 vs 12060"));
}

TEST(Gate, DumpGateIsDeterministicAndPinned) {
    const ReportDocument document = document_of(chain(4));
    const GateResult result = graphlint::compare(document, document, GateThresholds{});
    const std::string expected =
        "{\n"
        "  \"baseline\": {\"cuda_runtime\": 0, \"device\": \"\"},\n"
        "  \"current\": {\"cuda_runtime\": 0, \"device\": \"\"},\n"
        "  \"metrics\": [\n"
        "    {\"baseline\": 4, \"current\": 4, \"delta\": 0, \"name\": \"depth\", \"status\": "
        "\"none\"},\n"
        "    {\"baseline\": 0, \"current\": 0, \"delta\": 0, \"name\": \"edges_redundant\", "
        "\"status\": \"none\"},\n"
        "    {\"baseline\": 0, \"current\": 0, \"delta\": 0, \"name\": \"findings_severe\", "
        "\"status\": \"none\"},\n"
        "    {\"baseline\": 4, \"current\": 4, \"delta\": 0, \"name\": \"nodes\", \"status\": "
        "\"context\"},\n"
        "    {\"baseline\": 3, \"current\": 3, \"delta\": 0, \"name\": \"edges_distinct\", "
        "\"status\": \"context\"},\n"
        "    {\"baseline\": 1, \"current\": 1, \"delta\": 0, \"name\": \"width_max\", \"status\": "
        "\"context\"},\n"
        "    {\"baseline\": 0, \"current\": 0, \"delta\": 0, \"name\": \"findings_warn\", "
        "\"status\": \"context\"},\n"
        "    {\"baseline\": 0, \"current\": 0, \"delta\": 0, \"name\": \"findings_info\", "
        "\"status\": \"context\"}\n"
        "  ],\n"
        "  \"notes\": [\n"
        "    \"baseline and current carry the same graph\"\n"
        "  ],\n"
        "  \"reason\": \"all metrics inside their thresholds\",\n"
        "  \"schema_version\": 1,\n"
        "  \"thresholds\": {\"max_depth_increase\": 0, \"max_path_increase_ns\": -1, "
        "\"max_redundant_increase\": 0, \"max_severe_increase\": 0},\n"
        "  \"verdict\": \"pass\"\n"
        "}\n";
    EXPECT_EQ(graphlint::dump_gate(result, GateThresholds{}), expected);
    EXPECT_EQ(graphlint::dump_gate(result, GateThresholds{}),
              graphlint::dump_gate(result, GateThresholds{}));
}

TEST(Gate, FixtureCasesMatchTheContract) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/baseline.json", baseline, error))
        << error.line << ":" << error.column << " " << error.detail;

    // The fixture's declared blocks are the recomputed ones: only the same-graph note survives.
    const GateResult same = graphlint::compare(baseline, baseline, GateThresholds{});
    EXPECT_EQ(same.verdict, Verdict::pass);
    ASSERT_EQ(same.notes.size(), 1u);

    ReportDocument depth_plus_one;
    ASSERT_TRUE(load_report(dir + "/depth_plus_one.json", depth_plus_one, error));
    const GateResult deeper = graphlint::compare(baseline, depth_plus_one, GateThresholds{});
    EXPECT_EQ(deeper.verdict, Verdict::regression);
    EXPECT_EQ(deeper.reason, "depth grew 1 > 0");
    EXPECT_TRUE(deeper.notes.empty());

    ReportDocument host_node_report;
    ASSERT_TRUE(load_report(dir + "/host_node.json", host_node_report, error));
    const GateResult host = graphlint::compare(baseline, host_node_report, GateThresholds{});
    EXPECT_EQ(host.verdict, Verdict::regression);
    EXPECT_EQ(host.reason, "findings_severe grew 1 > 0");

    ReportDocument other_source;
    ASSERT_TRUE(load_report(dir + "/other_source.json", other_source, error));
    const GateResult source = graphlint::compare(baseline, other_source, GateThresholds{});
    EXPECT_EQ(source.verdict, Verdict::suspect);
    EXPECT_EQ(source.reason, "different source kind: built vs capture");

    ReportDocument ns_weights;
    ASSERT_TRUE(load_report(dir + "/ns_weights.json", ns_weights, error));
    const GateResult weights = graphlint::compare(baseline, ns_weights, GateThresholds{});
    EXPECT_EQ(weights.verdict, Verdict::suspect);
    EXPECT_EQ(weights.reason, "different weight_kind: levels vs ns");

    ReportDocument truncated;
    EXPECT_FALSE(load_report(dir + "/truncated.json", truncated, error));
    EXPECT_GT(error.line, 0);
    EXPECT_GT(error.column, 0);
}

// ---------------------------------------------------------------------------
// The matrix of docs/weights.md section 5, on the two committed ns fixtures.
// ---------------------------------------------------------------------------

TEST(Gate, NsFixturesCompareOnPathNs) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    ReportDocument plus_one;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/ns_baseline.json", baseline, error))
        << error.line << ":" << error.column << " " << error.detail;
    ASSERT_TRUE(load_report(dir + "/ns_path_plus_one.json", plus_one, error));

    // Declared blocks are the recomputed ones: the same document notes only its own graph.
    const GateResult same = graphlint::compare(baseline, baseline, GateThresholds{});
    EXPECT_EQ(same.verdict, Verdict::pass);
    ASSERT_EQ(same.notes.size(), 1u);

    // The weight table moved the same graph by 1 ns and nothing else moved.
    EXPECT_EQ(baseline.analysis.path_ns, 1000u);
    EXPECT_EQ(plus_one.analysis.path_ns, 1001u);
    EXPECT_EQ(baseline.analysis.path, plus_one.analysis.path);
    EXPECT_FALSE(graphlint::detail::same_graph_document(baseline.graph, plus_one.graph));
}

TEST(Gate, NsThresholdDecidesOnlyOnAnNsPair) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    ReportDocument plus_one;
    ReportDocument levels;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/ns_baseline.json", baseline, error));
    ASSERT_TRUE(load_report(dir + "/ns_path_plus_one.json", plus_one, error));
    ASSERT_TRUE(load_report(dir + "/baseline.json", levels, error));

    GateThresholds thresholds;
    thresholds.max_path_increase_ns = 0;
    const GateResult grew = graphlint::compare(baseline, plus_one, thresholds);
    EXPECT_EQ(grew.verdict, Verdict::regression);
    EXPECT_EQ(grew.reason, "path_ns grew 1 > 0");
    EXPECT_EQ(graphlint::exit_code(grew.verdict), 1);

    thresholds.max_path_increase_ns = 1;
    EXPECT_EQ(graphlint::compare(baseline, plus_one, thresholds).verdict, Verdict::pass);

    thresholds.max_path_increase_ns = 0;
    const GateResult fell = graphlint::compare(plus_one, baseline, thresholds);
    EXPECT_EQ(fell.verdict, Verdict::improvement);
    EXPECT_EQ(fell.reason, "path_ns fell 1 > 0");
    EXPECT_EQ(graphlint::exit_code(fell.verdict), 0);

    // ns fixtures on a levels pair: the threshold and the pair are in two units.
    GateThresholds on_levels;
    on_levels.max_path_increase_ns = 0;
    const GateResult refused = graphlint::compare(levels, levels, on_levels);
    EXPECT_EQ(refused.verdict, Verdict::suspect);
    EXPECT_NE(refused.reason.find("--max-path-increase-ns is a nanosecond threshold"),
              std::string::npos)
        << refused.reason;
    EXPECT_NE(refused.reason.find("levels vs levels"), std::string::npos) << refused.reason;
}

TEST(Gate, PathNsWithoutItsThresholdIsContext) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    ReportDocument plus_one;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/ns_baseline.json", baseline, error));
    ASSERT_TRUE(load_report(dir + "/ns_path_plus_one.json", plus_one, error));

    // No default for a measurement: 1 ns of growth is a number, not a verdict.
    const GateResult result = graphlint::compare(baseline, plus_one, GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::pass);
    ASSERT_EQ(result.metrics.size(), 9u);
    EXPECT_EQ(result.metrics[3].name, "path_ns");
    EXPECT_EQ(result.metrics[3].baseline, 1000);
    EXPECT_EQ(result.metrics[3].current, 1001);
    EXPECT_EQ(result.metrics[3].delta, 1);
    EXPECT_EQ(result.metrics[3].status, graphlint::MetricStatus::context);
    EXPECT_EQ(result.metrics[0].name, "depth");
    EXPECT_EQ(result.metrics[0].status, graphlint::MetricStatus::context);
}

TEST(Gate, DepthThresholdOnAnNsPairIsRefused) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/ns_baseline.json", baseline, error));

    GateThresholds given;
    given.depth_given = true;
    const GateResult refused = graphlint::compare(baseline, baseline, given);
    EXPECT_EQ(refused.verdict, Verdict::suspect);
    EXPECT_EQ(refused.reason,
              "--max-depth-increase is a level count and the pair is in ns");
    EXPECT_EQ(graphlint::exit_code(refused.verdict), 2);

    // The absence of the flag is not a threshold of 0: on ns the depth stays context.
    const GateResult absent = graphlint::compare(baseline, baseline, GateThresholds{});
    EXPECT_EQ(absent.verdict, Verdict::pass);
    EXPECT_EQ(absent.metrics[0].status, graphlint::MetricStatus::context);
}

TEST(Gate, OtherWeightKindTokensAreStillSuspect) {
    // ns on both sides but one graph is levels: the mixed pair stays the first problem.
    Graph nanoseconds = chain(4);
    nanoseconds.weight_kind = WeightKind::Nanoseconds;
    const GateResult result = graphlint::compare(document_of(nanoseconds),
                                                 document_of(chain(4)), GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::suspect);
    EXPECT_EQ(result.reason, "different weight_kind: ns vs levels");
}

TEST(Gate, EqualPathNsOtherPathIsANote) {
    // Two diamonds whose weighted sums agree: 0-1-3 and 0-2-3 both total 900.
    Graph left;
    left.nodes = {kernel_node(0), kernel_node(1), kernel_node(2), kernel_node(3)};
    left.edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    left.weight_kind = WeightKind::Nanoseconds;
    left.nodes[0].weight = 200;
    left.nodes[1].weight = 500;
    left.nodes[2].weight = 100;
    left.nodes[3].weight = 200;
    Graph right = left;
    right.nodes[1].weight = 100;
    right.nodes[2].weight = 500;

    const GateResult result =
        graphlint::compare(document_of(left), document_of(right), GateThresholds{});
    EXPECT_EQ(result.verdict, Verdict::pass);
    EXPECT_TRUE(mentions(result.notes, "path differs while path_ns matches: 0 1 3 vs 0 2 3"))
        << graphlint::detail::join_problems(result.notes);
}

TEST(Gate, ATamperedNsDeclaredPathIsANoteOnly) {
    const std::string dir = k_data_dir;
    ReportDocument baseline;
    ReportDocument plus_one;
    GraphError error;
    ASSERT_TRUE(load_report(dir + "/ns_baseline.json", baseline, error));
    ASSERT_TRUE(load_report(dir + "/ns_path_plus_one.json", plus_one, error));

    // A declared 999 on a 1000 ns document: the recomputed sum decides, the block is a note.
    ReportDocument tampered = baseline;
    tampered.analysis.path_ns = 999;
    GateThresholds thresholds;
    thresholds.max_path_increase_ns = 0;
    const GateResult within = graphlint::compare(baseline, tampered, thresholds);
    EXPECT_EQ(within.verdict, Verdict::pass);
    EXPECT_TRUE(mentions(within.notes, "current: declared analysis disagrees"));

    // The same tamper does not hide a real 1 ns growth: the current graph says 1001.
    plus_one.analysis.path = {0, 2, 3};
    const GateResult grew = graphlint::compare(baseline, plus_one, thresholds);
    EXPECT_EQ(grew.verdict, Verdict::regression);
    EXPECT_EQ(grew.reason, "path_ns grew 1 > 0");
    EXPECT_TRUE(mentions(grew.notes, "current: declared analysis disagrees"));
}

TEST(Gate, DumpGateCarriesPathNsInNs) {
    const ReportDocument document = document_of(weighted_chain({100, 200, 300}));
    GateThresholds thresholds;
    thresholds.max_path_increase_ns = 0;
    const GateResult result = graphlint::compare(document, document, thresholds);
    const std::string text = graphlint::dump_gate(result, thresholds);
    EXPECT_NE(text.find("{\"baseline\": 600, \"current\": 600, \"delta\": 0, \"name\": "
                        "\"path_ns\", \"status\": \"none\"}"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("\"max_path_increase_ns\": 0"), std::string::npos) << text;
}
