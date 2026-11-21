#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/rules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using graphlint::Finding;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::GraphErrorKind;
using graphlint::LintResult;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::RuleId;
using graphlint::RuleStatus;
using graphlint::Severity;
using graphlint::SourceKind;
using graphlint::Topology;

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

std::string describe(const Finding& finding) {
    return std::string(graphlint::to_string(finding.rule)) + " " +
           graphlint::to_string(finding.severity) + " [" + join(finding.nodes) + "] " +
           finding.message;
}

std::vector<std::string> describe(const std::vector<Finding>& findings) {
    std::vector<std::string> lines;
    lines.reserve(findings.size());
    for (const Finding& finding : findings) {
        lines.push_back(describe(finding));
    }
    return lines;
}

std::string describe(const RuleStatus& status) {
    return std::string(graphlint::to_string(status.rule)) +
           (status.applicable ? " applicable: " : " not_applicable: ") + status.reason;
}

std::vector<std::string> statuses_of(const LintResult& result) {
    std::vector<std::string> lines;
    for (const RuleStatus& status : result.rules) {
        lines.push_back(describe(status));
    }
    return lines;
}

std::string status_of(const LintResult& result, RuleId rule) {
    for (const RuleStatus& status : result.rules) {
        if (status.rule == rule) {
            return describe(status);
        }
    }
    return "missing";
}

std::vector<Finding> findings_of(const LintResult& result, RuleId rule) {
    std::vector<Finding> selected;
    for (const Finding& finding : result.findings) {
        if (finding.rule == rule) {
            selected.push_back(finding);
        }
    }
    return selected;
}

void retype(Graph& graph, int id, NodeType type) {
    Node& node = graph.nodes[static_cast<std::size_t>(id)];
    node.type = type;
    const std::uint32_t dim = type == NodeType::Kernel ? 1u : 0u;
    node.grid = {dim, dim, dim};
    node.block = {dim, dim, dim};
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

Graph kernel_graph(int count) {
    Graph graph = make_graph(count);
    for (int i = 0; i < count; ++i) {
        retype(graph, i, NodeType::Kernel);
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

// Not `Run`: testing::Test has a private member function with that name, and inside a TEST body
// it hides any type called Run at namespace scope.
struct Linted {
    bool analysed = false;
    LintResult result;
};

Linted run_lint(const Graph& graph) {
    Topology topo;
    GraphError error;
    const bool ok = graphlint::topology(graph, topo, error);
    Linted run;
    run.analysed = graphlint::lint(graph, topo, error, run.result);
    EXPECT_EQ(run.analysed, ok) << "lint and topology disagree on whether the graph is analysable";
    return run;
}

// Field by field, not just the size: a rule that moves a node list or a byte of schema text
// between two runs is a broken report, and the vectors are sorted so positions must match too.
void expect_identical(const LintResult& first, const LintResult& second) {
    ASSERT_EQ(second.findings.size(), first.findings.size());
    ASSERT_EQ(second.rules.size(), first.rules.size());
    for (std::size_t i = 0; i < first.findings.size(); ++i) {
        EXPECT_EQ(first.findings[i].rule, second.findings[i].rule) << "rule " << i;
        EXPECT_EQ(first.findings[i].severity, second.findings[i].severity) << "severity " << i;
        EXPECT_EQ(first.findings[i].nodes, second.findings[i].nodes) << "nodes " << i;
        EXPECT_EQ(first.findings[i].message, second.findings[i].message) << "message " << i;
    }
    for (std::size_t i = 0; i < first.rules.size(); ++i) {
        EXPECT_EQ(first.rules[i].rule, second.rules[i].rule) << "status rule " << i;
        EXPECT_EQ(first.rules[i].applicable, second.rules[i].applicable)
            << "status applicable " << i;
        EXPECT_EQ(first.rules[i].reason, second.rules[i].reason) << "status reason " << i;
    }
}

// A graph that fires five rules at once: a chain with a duplicate edge, both sync tiers, one
// named-kernel group, two allocations live together and two isolated kernels.
Graph noisy_graph() {
    Graph graph = make_graph(20);
    for (int i = 1; i < 18; ++i) {
        graph.edges.emplace_back(i - 1, i);
    }
    graph.edges.emplace_back(3, 4);

    for (int id = 0; id < 20; ++id) {
        retype(graph, id, NodeType::Kernel);
    }
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x100;
    retype(graph, 1, NodeType::Host);
    retype(graph, 2, NodeType::ExtSemaphore);
    retype(graph, 3, NodeType::EventRecord);
    retype(graph, 4, NodeType::EventWait);
    retype(graph, 6, NodeType::MemAlloc);
    graph.nodes[6].alloc_bytes = 1024;
    graph.nodes[6].mem_ptr = 0x200;
    retype(graph, 16, NodeType::MemFree);
    graph.nodes[16].mem_ptr = 0x100;
    retype(graph, 17, NodeType::MemFree);
    graph.nodes[17].mem_ptr = 0x200;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].type == NodeType::Kernel) {
            graph.nodes[i].name = "axpy";
        }
    }
    return graph;
}

std::vector<std::string> refusal_statuses() {
    return {
        "dag-check applicable: topology refused the graph",
        "launch-shaped not_applicable: graph failed dag-check",
        "redundant-edge not_applicable: graph failed dag-check",
        "sync-node not_applicable: graph failed dag-check",
        "pointless-fork not_applicable: graph failed dag-check",
        "duplicate-launch not_applicable: graph failed dag-check",
        "mem-lifecycle not_applicable: graph failed dag-check",
    };
}

// The four artifacts hold only unnamed 1x1x1 kernels and no mem node (graphs/REPORT.md).
std::vector<std::string> artifact_statuses() {
    return {
        "dag-check applicable: graph is a DAG",
        "launch-shaped applicable: depth and node count are known",
        "redundant-edge applicable: the reduction is exact on a DAG",
        "sync-node applicable: node types are readable",
        "pointless-fork applicable: successors and reachability are readable",
        "duplicate-launch not_applicable: no kernel name",
        "mem-lifecycle not_applicable: no mem_alloc/mem_free node",
    };
}

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

}  // namespace

// ---------------------------------------------------------------------------
// The tokens are schema text: the report is byte-identical, so the spelling of a
// severity, a rule id and an error kind cannot drift with an enum rename.
// ---------------------------------------------------------------------------

TEST(Tokens, SeverityTokensAreTheDocumentWords) {
    EXPECT_EQ(std::string(graphlint::to_string(Severity::error)), "error");
    EXPECT_EQ(std::string(graphlint::to_string(Severity::severe)), "severe");
    EXPECT_EQ(std::string(graphlint::to_string(Severity::warn)), "warn");
    EXPECT_EQ(std::string(graphlint::to_string(Severity::info)), "info");
}

TEST(Tokens, SeverityRanksFollowTheDocumentOrder) {
    EXPECT_EQ(graphlint::severity_rank(Severity::error), 0);
    EXPECT_EQ(graphlint::severity_rank(Severity::severe), 1);
    EXPECT_EQ(graphlint::severity_rank(Severity::warn), 2);
    EXPECT_EQ(graphlint::severity_rank(Severity::info), 3);
}

TEST(Tokens, RuleTokensAreTheDocumentIds) {
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::DagCheck)), "dag-check");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::LaunchShaped)), "launch-shaped");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::RedundantEdge)), "redundant-edge");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::SyncNode)), "sync-node");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::PointlessFork)), "pointless-fork");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::DuplicateLaunch)), "duplicate-launch");
    EXPECT_EQ(std::string(graphlint::to_string(RuleId::MemLifecycle)), "mem-lifecycle");
    EXPECT_EQ(graphlint::k_rule_count, 7u);
}

TEST(Tokens, ErrorKindTokensAreStable) {
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::None)), "none");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::SchemaVersion)),
              "schema_version");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::Syntax)), "syntax");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::Trailing)),
              "trailing");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::UnknownKey)),
              "unknown_key");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::DuplicateKey)),
              "duplicate_key");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::UnsortedKey)),
              "unsorted_key");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::WrongType)),
              "wrong_type");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::BadNumber)),
              "bad_number");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::UnknownType)),
              "unknown_type");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::MissingKey)),
              "missing_key");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::FieldForType)),
              "field_for_type");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::IdNotDense)),
              "id_not_dense");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::EdgeBackwards)),
              "edge_backwards");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::EdgeOutOfRange)),
              "edge_out_of_range");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::ChildMismatch)),
              "child_mismatch");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::WeightRule)),
              "weight_rule");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::TooDeep)),
              "too_deep");
    EXPECT_EQ(std::string(graphlint::detail::error_kind_token(GraphErrorKind::Cycle)), "cycle");
}

// ---------------------------------------------------------------------------
// dag-check: warn for a duplicate pair and for an isolated node, error only when
// the topology refuses the graph, and the two tiers never mix.
// ---------------------------------------------------------------------------

TEST(DagCheck, DuplicatePairIsOneWarnPerPairPlusTheIsolatedNode) {
    // 5 nodes and 5 raw edges: 0->1 three times, then 1->2 and 2->3, so node 4 is touched
    // by nothing. 3 distinct edges, depth 4, no cycle.
    Graph graph = make_graph(5);
    graph.edges = {{0, 1}, {0, 1}, {0, 1}, {1, 2}, {2, 3}};
    const Linted run = run_lint(graph);

    ASSERT_TRUE(run.analysed);
    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "dag-check warn [0 1] edge [0, 1] appears 3 times; the analysis counts it once",
                  "dag-check warn [4] node 4 has no in- and no out-edge: it runs concurrently, "
                  "and a forgotten dependency looks the same"}));
}

TEST(DagCheck, IsolatedNodeInAGraphWithoutEdgesIsSilent) {
    // With no edge at all there is nothing to be isolated from, and the rule still answers.
    const Linted run = run_lint(make_graph(3));

    EXPECT_TRUE(run.analysed);
    EXPECT_TRUE(run.result.findings.empty());
    EXPECT_EQ(status_of(run.result, RuleId::DagCheck), "dag-check applicable: graph is a DAG");
}

TEST(DagCheck, BackwardsPairBecomesOneErrorAndSixRefusals) {
    // {3->4, 4->3} is the cycle a hand-built Graph can hold. topology checks from < to before
    // Kahn, so the refusal arrives as a backwards pair; lint reports the refusal, it does not
    // rescan the graph for a cycle.
    Graph graph = make_graph(5);
    graph.edges = {{3, 4}, {4, 3}};
    const Linted run = run_lint(graph);

    EXPECT_FALSE(run.analysed);
    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "dag-check error [] edge_backwards: edge [4, 3] is not from < to"}));
    EXPECT_EQ(statuses_of(run.result), refusal_statuses());
}

TEST(DagCheck, UnknownEndpointBecomesOneErrorAndSixRefusals) {
    Graph graph = make_graph(2);
    graph.edges.emplace_back(0, 2);
    const Linted run = run_lint(graph);

    EXPECT_FALSE(run.analysed);
    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "dag-check error [] edge_out_of_range: edge [0, 2] is outside the node list"}));
    EXPECT_EQ(statuses_of(run.result), refusal_statuses());
}

TEST(DagCheck, SparseIdsBecomeOneErrorAndSixRefusals) {
    Graph graph = make_graph(3);
    graph.nodes[1].id = 7;
    const Linted run = run_lint(graph);

    EXPECT_FALSE(run.analysed);
    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "dag-check error [] id_not_dense: node at position 1 has id 7"}));
    EXPECT_EQ(statuses_of(run.result), refusal_statuses());
}

TEST(DagCheck, RefusalWithoutAnErrorStillLeavesAWholeResult) {
    // A caller that lost the GraphError gets a sentence, not "none: ".
    Graph graph = make_graph(2);
    Topology topo;
    topo.acyclic = false;
    GraphError error;
    LintResult result;

    EXPECT_FALSE(graphlint::lint(graph, topo, error, result));
    EXPECT_EQ(describe(result.findings),
              (std::vector<std::string>{
                  "dag-check error [] graph failed dag-check: no error detail was passed"}));
    EXPECT_EQ(statuses_of(result), refusal_statuses());
}

TEST(DagCheck, RefusalMessageStuttersAndDanglesNever) {
    // A caller-built error may carry no detail, or a Cycle detail that already opens with the
    // token (validate's wording). Both must read as one sentence.
    Graph graph = make_graph(2);
    Topology topo;
    topo.acyclic = false;

    GraphError no_detail;
    no_detail.kind = GraphErrorKind::EdgeOutOfRange;
    LintResult first;
    EXPECT_FALSE(graphlint::lint(graph, topo, no_detail, first));
    ASSERT_EQ(first.findings.size(), 1u);
    EXPECT_EQ(first.findings[0].message, "edge_out_of_range");
    EXPECT_EQ(statuses_of(first), refusal_statuses());

    GraphError repeated;
    repeated.kind = GraphErrorKind::Cycle;
    repeated.detail = "cycle: 2 of 2 nodes never became ready (first id 0)";
    LintResult second;
    EXPECT_FALSE(graphlint::lint(graph, topo, repeated, second));
    ASSERT_EQ(second.findings.size(), 1u);
    EXPECT_EQ(second.findings[0].message,
              "cycle: 2 of 2 nodes never became ready (first id 0)");
}

// ---------------------------------------------------------------------------
// launch-shaped: 10 * depth >= 9 * nodes && nodes >= 16, in integers.
// ---------------------------------------------------------------------------

TEST(LaunchShaped, ThousandNodeChainFires) {
    Graph graph;
    ASSERT_TRUE(load_artifact("chain.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "launch-shaped warn [] 1000 levels over 1000 nodes: no two nodes can overlap"}));
}

TEST(LaunchShaped, GuardIsSixteenNodesNotFifteenLevels) {
    // On a chain depth == nodes: 15 is 150 against 135 and still silent, because 15 < 16.
    EXPECT_TRUE(findings_of(run_lint(chained(15)).result, RuleId::LaunchShaped).empty());
    EXPECT_EQ(findings_of(run_lint(chained(16)).result, RuleId::LaunchShaped).size(), 1u);
}

TEST(LaunchShaped, PingpongStaysUnderTheGuard) {
    // 12 levels over 12 nodes is a ratio of 1.00 and 120 >= 108; the floor of 16 nodes is the
    // only thing that keeps it quiet, and it is a declared limit (§5.1).
    Graph graph;
    ASSERT_TRUE(load_artifact("pingpong.json", graph));
    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::LaunchShaped).empty());
}

TEST(LaunchShaped, CapturedIsNotALaunchShapedGraph) {
    // 15 levels over 25 nodes: a bare depth >= 15 fires here, the ratio (150 against 225) does
    // not. This is the false positive the guard exists to avoid.
    Graph graph;
    ASSERT_TRUE(load_artifact("captured.json", graph));
    Topology topo;
    GraphError error;
    ASSERT_TRUE(graphlint::topology(graph, topo, error)) << error.detail;
    EXPECT_EQ(topo.depth, 15);
    EXPECT_EQ(graph.nodes.size(), 25u);

    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::LaunchShaped).empty());
}

TEST(LaunchShaped, TwoHundredFiftyMillionNodesUseSixtyFourBitMath) {
    // Synthetic inputs: no graph is built, the arithmetic of the rule is what is under test, and a
    // 2.5e8-node Graph does not fit in a test. 10 * 2.5e8 = 2.5e9 is above INT_MAX, so an int32
    // product wraps; 9 * 2.5e8 = 2.25e9 wraps too, which turns the second case from false to true.
    EXPECT_TRUE(graphlint::detail::launch_shaped(250000000, 250000000u));
    EXPECT_FALSE(graphlint::detail::launch_shaped(0, 250000000u));
    EXPECT_FALSE(graphlint::detail::launch_shaped(10000000, 100000000u));
}

// ---------------------------------------------------------------------------
// redundant-edge: the reduction is the answer, and 0 is an answer.
// ---------------------------------------------------------------------------

TEST(RedundantEdge, TenOfTwentyOneWithElevenInTheMinimalDag) {
    Graph graph;
    ASSERT_TRUE(load_artifact("pingpong.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "redundant-edge warn [0 1 2 3 4 5 6 7 8 9 10 11] 10 of 21 edges are implied by "
                  "a longer path; the minimal DAG has 11 edges"}));
}

TEST(RedundantEdge, OneOfTwentyNineUsesTheSingularVerb) {
    // captured.json: 2->13 is the only implied edge, and the sentence is read by a human, so
    // "1 of 29 edges is implied" — 29 stays plural, the verb follows the count.
    Graph graph;
    ASSERT_TRUE(load_artifact("captured.json", graph));
    const Linted run = run_lint(graph);

    const std::vector<Finding> findings = findings_of(run.result, RuleId::RedundantEdge);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].message,
              "1 of 29 edges is implied by a longer path; the minimal DAG has 28 edges");
}

TEST(RedundantEdge, ZeroIsAnAnswerAndTheRuleStaysApplicable) {
    Graph graph;
    ASSERT_TRUE(load_artifact("built.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::RedundantEdge).empty());
    EXPECT_EQ(status_of(run.result, RuleId::RedundantEdge),
              "redundant-edge applicable: the reduction is exact on a DAG");
}

// ---------------------------------------------------------------------------
// sync-node: severe per node for host and ext_semaphore, one info per graph for
// events, and no warn tier in between.
// ---------------------------------------------------------------------------

TEST(SyncNode, HostAndSemaphoreAreSeverePerNode) {
    Graph graph = chained(6);
    retype(graph, 0, NodeType::Host);
    retype(graph, 1, NodeType::ExtSemaphore);
    retype(graph, 2, NodeType::EventRecord);
    retype(graph, 3, NodeType::EventWait);
    retype(graph, 4, NodeType::Kernel);
    retype(graph, 5, NodeType::Kernel);
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::SyncNode)),
              (std::vector<std::string>{
                  "sync-node severe [0] host node 0 runs a CPU callback inside the graph and "
                  "serialises its stream",
                  "sync-node severe [1] ext_semaphore node 1 waits on a producer the graph does "
                  "not contain",
                  "sync-node info [] 2 event nodes record or wait on events: this is how a fork "
                  "is expressed"}));
}

TEST(SyncNode, TenEventsAreOneInfoFindingAndNeverAWarn) {
    // No count threshold and no warn tier: ten events are context, not ten defects.
    Graph graph = chained(10);
    for (int i = 0; i < 10; ++i) {
        retype(graph, i, i % 2 == 0 ? NodeType::EventRecord : NodeType::EventWait);
    }
    const Linted run = run_lint(graph);

    const std::vector<Finding> findings = findings_of(run.result, RuleId::SyncNode);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].severity, Severity::info);
    EXPECT_TRUE(findings[0].nodes.empty());
    EXPECT_EQ(describe(findings),
              (std::vector<std::string>{
                  "sync-node info [] 10 event nodes record or wait on events: this is how a fork "
                  "is expressed"}));
}

TEST(SyncNode, OneEventIsASingularSentence) {
    Graph graph = chained(3);
    retype(graph, 1, NodeType::EventRecord);
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::SyncNode)),
              (std::vector<std::string>{
                  "sync-node info [] 1 event node records or waits on events: this is how a "
                  "fork is expressed"}));
}

// ---------------------------------------------------------------------------
// pointless-fork: two single-node branches that rejoin, never a renamed
// redundant edge, never a branch that holds two nodes.
// ---------------------------------------------------------------------------

TEST(PointlessFork, MinimalDiamondFires) {
    // 0->{1,2}->3: two branches of one node each, and the branch is the edge itself.
    Graph graph = make_graph(4);
    graph.edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::PointlessFork)),
              (std::vector<std::string>{
                  "pointless-fork info [0 3] node 0 splits into 2 single-node branches that "
                  "rejoin at node 3: the fork buys nothing"}));
}

TEST(PointlessFork, DiamondInTheBuiltArtifactFires) {
    Graph graph;
    ASSERT_TRUE(load_artifact("built.json", graph));
    const std::vector<Finding> findings = findings_of(run_lint(graph).result,
                                                      RuleId::PointlessFork);

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].nodes, (std::vector<int>{1, 4}));
    EXPECT_EQ(findings[0].severity, Severity::info);
}

TEST(PointlessFork, BranchWithTwoNodesIsSilent) {
    // u=0 -> {1,2}; t=1 goes 1->3->4, so j=4 is not a successor of t and the branch holds two
    // nodes. Only t=2 gives 2->4 once: one branch, no fork.
    Graph graph = make_graph(5);
    graph.edges = {{0, 1}, {0, 2}, {1, 3}, {3, 4}, {2, 4}};
    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::PointlessFork).empty());
}

TEST(PointlessFork, TransitiveEdgeDoesNotTurnABranchIntoAFork) {
    // 1->4 exists too, but 1->3->4 makes it implied: the branch through 1 still holds two nodes,
    // so it is not a fork and the skip edge must not be counted as one.
    Graph graph = make_graph(5);
    graph.edges = {{0, 1}, {0, 2}, {1, 4}, {1, 3}, {3, 4}, {2, 4}};
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::PointlessFork).empty());
    // The edge it cannot count is redundant, and redundant-edge is where it belongs.
    EXPECT_EQ(findings_of(run.result, RuleId::RedundantEdge).size(), 1u);
}

TEST(PointlessFork, ImpliedFirstEdgeIsNotABranch) {
    // u=0 -> {1, 3} minimal; 0->2 is implied by 0->1->2, so the route to j through 2 is
    // 0->1->2->4: three nodes, not a single-node branch. Counting 2 as a successor of u would
    // pair it with 3 and report a fork that does not exist.
    Graph graph = make_graph(5);
    graph.edges = {{0, 1}, {1, 2}, {0, 2}, {2, 4}, {0, 3}, {3, 4}};
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::PointlessFork).empty());
    // The edge it must not count is implied, and redundant-edge is where it belongs.
    const std::vector<Finding> redundant = findings_of(run.result, RuleId::RedundantEdge);
    ASSERT_EQ(redundant.size(), 1u);
    EXPECT_EQ(redundant[0].nodes, (std::vector<int>{0, 2}));
}

TEST(PointlessFork, JoinThenBroadcastReportsOneFork) {
    // 0 fans out to eight one-node branches that all join at 9. Every other node has a single
    // successor, which is the shape the early exit skips: the finding must still be there.
    Graph graph = make_graph(10);
    for (int branch = 1; branch <= 8; ++branch) {
        graph.edges.emplace_back(0, branch);
        graph.edges.emplace_back(branch, 9);
    }
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::PointlessFork)),
              (std::vector<std::string>{
                  "pointless-fork info [0 9] node 0 splits into 8 single-node branches that "
                  "rejoin at node 9: the fork buys nothing"}));
}

TEST(PointlessFork, PingpongIsSilent) {
    // Before `t != j` the rule fired 10 times here: u->j was the redundant edge renamed.
    Graph graph;
    ASSERT_TRUE(load_artifact("pingpong.json", graph));
    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::PointlessFork).empty());
}

// ---------------------------------------------------------------------------
// duplicate-launch: equal non-empty names and equal geometry, and geometry alone
// is never enough.
// ---------------------------------------------------------------------------

TEST(DuplicateLaunch, EqualNonEmptyNamesAreOneCandidateGroup) {
    // 0 and 2 share name and geometry; 1 has the same name and a different block.
    Graph graph = kernel_graph(3);
    graph.nodes[0].name = "axpy";
    graph.nodes[1].name = "axpy";
    graph.nodes[1].block = {64, 1, 1};
    graph.nodes[2].name = "axpy";
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::DuplicateLaunch)),
              (std::vector<std::string>{
                  "duplicate-launch info [0 2] 2 kernel nodes launch \"axpy\" with the same grid, "
                  "block and shared bytes: a candidate, not a duplicate"}));
}

TEST(DuplicateLaunch, DifferentGeometryIsADifferentGroup) {
    Graph graph = kernel_graph(2);
    graph.nodes[0].name = "axpy";
    graph.nodes[1].name = "axpy";
    graph.nodes[1].grid = {2, 1, 1};
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::DuplicateLaunch).empty());
    EXPECT_EQ(status_of(run.result, RuleId::DuplicateLaunch),
              "duplicate-launch applicable: at least two named kernels");
}

TEST(DuplicateLaunch, OneNamedKernelIsStillNotApplicable) {
    // "fewer than two names" is the guard: one name cannot form a group, and flipping the
    // comparison to < 1 would declare the rule applicable with nothing to say.
    Graph graph = kernel_graph(1);
    graph.nodes[0].name = "axpy";
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::DuplicateLaunch).empty());
    EXPECT_EQ(status_of(run.result, RuleId::DuplicateLaunch),
              "duplicate-launch not_applicable: no kernel name");
}

TEST(DuplicateLaunch, UnnamedKernelsStayNotApplicable) {
    // 27 kernels, every one 1x1x1 and unnamed: on geometry alone this would be 27 candidates
    // carrying nothing, which is why the rule documents its own blindness instead.
    const Linted run = run_lint(kernel_graph(27));

    EXPECT_TRUE(findings_of(run.result, RuleId::DuplicateLaunch).empty());
    EXPECT_EQ(status_of(run.result, RuleId::DuplicateLaunch),
              "duplicate-launch not_applicable: no kernel name");
}

// ---------------------------------------------------------------------------
// mem-lifecycle: one allocation interval per mem_ptr, swept in id order.
// ---------------------------------------------------------------------------

TEST(MemLifecycle, AllocationWithoutFreeIsReported) {
    // 4096 bytes at node 0, no free: live to the end. The peak equals the request, so the
    // second finding (peak above the largest request) does not fire.
    Graph graph = chained(3);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x1000;
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::MemLifecycle)),
              (std::vector<std::string>{
                  "mem-lifecycle warn [0] 4096 bytes at node 0 are never freed: live to the end "
                  "and counted in the peak"}));
}

TEST(MemLifecycle, TwoLiveAllocationsReportThePeak) {
    // alloc 4096 at 0, alloc 4096 at 1, free at 2 and 3: at node 1 both are live, so the peak
    // is 8192 against a largest request of 4096.
    Graph graph = chained(4);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x100;
    retype(graph, 1, NodeType::MemAlloc);
    graph.nodes[1].alloc_bytes = 4096;
    graph.nodes[1].mem_ptr = 0x200;
    retype(graph, 2, NodeType::MemFree);
    graph.nodes[2].mem_ptr = 0x100;
    retype(graph, 3, NodeType::MemFree);
    graph.nodes[3].mem_ptr = 0x200;
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::MemLifecycle)),
              (std::vector<std::string>{
                  "mem-lifecycle warn [] peak 8192 bytes at node 1, 2 live, largest request "
                  "4096, 2 alloc/free pairs"}));
}

TEST(MemLifecycle, SequentialAllocationsDoNotRaiseThePeak) {
    // alloc 0 p / free 1 p / alloc 2 q / free 3 q: never two live at once, so the peak equals the
    // largest request and there is nothing to report. Without the expiry the second allocation
    // adds to a sum that never went down.
    Graph graph = chained(4);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x100;
    retype(graph, 1, NodeType::MemFree);
    graph.nodes[1].mem_ptr = 0x100;
    retype(graph, 2, NodeType::MemAlloc);
    graph.nodes[2].alloc_bytes = 4096;
    graph.nodes[2].mem_ptr = 0x200;
    retype(graph, 3, NodeType::MemFree);
    graph.nodes[3].mem_ptr = 0x200;

    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::MemLifecycle).empty());
}

TEST(MemLifecycle, SamePointerIsPairedFirstInFirstOut) {
    // Same shape as the sequential case, but one pointer: the first free belongs to the first
    // alloc. A LIFO pairing would give node 2 the earliest free, leave it unfreed and report it.
    Graph graph = chained(4);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x100;
    retype(graph, 1, NodeType::MemFree);
    graph.nodes[1].mem_ptr = 0x100;
    retype(graph, 2, NodeType::MemAlloc);
    graph.nodes[2].alloc_bytes = 4096;
    graph.nodes[2].mem_ptr = 0x100;
    retype(graph, 3, NodeType::MemFree);
    graph.nodes[3].mem_ptr = 0x100;

    EXPECT_TRUE(findings_of(run_lint(graph).result, RuleId::MemLifecycle).empty());
}

TEST(MemLifecycle, OneByteIsASingularSentence) {
    Graph graph = chained(2);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 1;
    graph.nodes[0].mem_ptr = 0x100;
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::MemLifecycle)),
              (std::vector<std::string>{
                  "mem-lifecycle warn [0] 1 byte at node 0 is never freed: live to the end and "
                  "counted in the peak"}));
}

TEST(MemLifecycle, OnePairIsASingularSentence) {
    // Two allocations live together at node 1 and only one of them is ever freed: the peak
    // fires and the churn is one pair, which the message has to spell out in the singular.
    Graph graph = chained(4);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    graph.nodes[0].mem_ptr = 0x100;
    retype(graph, 1, NodeType::MemAlloc);
    graph.nodes[1].alloc_bytes = 4096;
    graph.nodes[1].mem_ptr = 0x200;
    retype(graph, 2, NodeType::MemFree);
    graph.nodes[2].mem_ptr = 0x100;
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::MemLifecycle)),
              (std::vector<std::string>{
                  "mem-lifecycle warn [] peak 8192 bytes at node 1, 2 live, largest request "
                  "4096, 1 alloc/free pair",
                  "mem-lifecycle warn [1] 4096 bytes at node 1 are never freed: live to the end "
                  "and counted in the peak"}));
}

TEST(MemLifecycle, FreeOutsideTheGraphIsNotAnError) {
    // cudaGraphAddMemFreeNode accepts a pointer allocated before the capture: legal, so the
    // unmatched free is context and there is nothing to report.
    Graph graph = chained(2);
    retype(graph, 1, NodeType::MemFree);
    graph.nodes[1].mem_ptr = 0x999;
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::MemLifecycle).empty());
    EXPECT_EQ(status_of(run.result, RuleId::MemLifecycle),
              "mem-lifecycle applicable: mem_alloc/mem_free nodes are present");
}

TEST(MemLifecycle, ZeroPointerAllocationIsNeverReportedAsLeaked) {
    // A zero pointer has no key to correlate with, so nothing can say it is leaked: including it
    // would print "4096 bytes at node 0 are never freed" on a graph where the getter failed.
    Graph graph = chained(3);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 4096;
    const Linted run = run_lint(graph);

    EXPECT_TRUE(findings_of(run.result, RuleId::MemLifecycle).empty());
    EXPECT_EQ(status_of(run.result, RuleId::MemLifecycle),
              "mem-lifecycle applicable: mem_alloc/mem_free nodes are present");
}

TEST(MemLifecycle, ZeroPointerStaysOutOfTheSweepAndTheCounts) {
    // 8192 bytes at ptr 0 would dominate the peak and add a third live allocation if the guard
    // were gone; the two nonzero allocations are the only thing the sweep sees.
    Graph graph = chained(5);
    retype(graph, 0, NodeType::MemAlloc);
    graph.nodes[0].alloc_bytes = 8192;
    retype(graph, 1, NodeType::MemAlloc);
    graph.nodes[1].alloc_bytes = 4096;
    graph.nodes[1].mem_ptr = 0x100;
    retype(graph, 2, NodeType::MemAlloc);
    graph.nodes[2].alloc_bytes = 4096;
    graph.nodes[2].mem_ptr = 0x200;
    retype(graph, 3, NodeType::MemFree);
    graph.nodes[3].mem_ptr = 0x100;
    retype(graph, 4, NodeType::MemFree);
    graph.nodes[4].mem_ptr = 0x200;
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(findings_of(run.result, RuleId::MemLifecycle)),
              (std::vector<std::string>{
                  "mem-lifecycle warn [] peak 8192 bytes at node 2, 2 live, largest request "
                  "4096, 2 alloc/free pairs"}));
}

TEST(MemLifecycle, GraphWithoutMemNodesIsNotApplicable) {
    const Linted run = run_lint(chained(4));

    EXPECT_TRUE(findings_of(run.result, RuleId::MemLifecycle).empty());
    EXPECT_EQ(status_of(run.result, RuleId::MemLifecycle),
              "mem-lifecycle not_applicable: no mem_alloc/mem_free node");
}

// ---------------------------------------------------------------------------
// The committed artifacts: the full finding set and all seven statuses, no GPU.
// ---------------------------------------------------------------------------

TEST(Artifacts, BuiltReportsTheMinimalDiamondOnly) {
    Graph graph;
    ASSERT_TRUE(load_artifact("built.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "pointless-fork info [1 4] node 1 splits into 2 single-node branches that "
                  "rejoin at node 4: the fork buys nothing"}));
    EXPECT_EQ(statuses_of(run.result), artifact_statuses());
}

TEST(Artifacts, CapturedReportsOneRedundantEdgeAndOneFork) {
    // 2->13 is implied by 2->3->13; 0->{1,2}->3 is the real two-way split.
    Graph graph;
    ASSERT_TRUE(load_artifact("captured.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "redundant-edge warn [2 13] 1 of 29 edges is implied by a longer path; the "
                  "minimal DAG has 28 edges",
                  "pointless-fork info [0 3] node 0 splits into 2 single-node branches that "
                  "rejoin at node 3: the fork buys nothing"}));
    EXPECT_EQ(statuses_of(run.result), artifact_statuses());
}

TEST(Artifacts, PingpongReportsTenOfTwentyOne) {
    Graph graph;
    ASSERT_TRUE(load_artifact("pingpong.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "redundant-edge warn [0 1 2 3 4 5 6 7 8 9 10 11] 10 of 21 edges are implied by "
                  "a longer path; the minimal DAG has 11 edges"}));
    EXPECT_EQ(statuses_of(run.result), artifact_statuses());
}

TEST(Artifacts, ChainIsALaunchShapedWarning) {
    Graph graph;
    ASSERT_TRUE(load_artifact("chain.json", graph));
    const Linted run = run_lint(graph);

    EXPECT_EQ(describe(run.result.findings),
              (std::vector<std::string>{
                  "launch-shaped warn [] 1000 levels over 1000 nodes: no two nodes can overlap"}));
    EXPECT_EQ(statuses_of(run.result), artifact_statuses());
}

TEST(Artifacts, SevenStatusesInEveryResult) {
    const Linted run = run_lint(make_graph(1));

    ASSERT_EQ(run.result.rules.size(), graphlint::k_rule_count);
    const std::vector<RuleId> order = {RuleId::DagCheck,     RuleId::LaunchShaped,
                                       RuleId::RedundantEdge, RuleId::SyncNode,
                                       RuleId::PointlessFork, RuleId::DuplicateLaunch,
                                       RuleId::MemLifecycle};
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(run.result.rules[i].rule, order[i]) << "status " << i;
    }
}

// ---------------------------------------------------------------------------
// Order and determinism: a pure function of the fields, so two runs are identical.
// ---------------------------------------------------------------------------

TEST(Order, SortRealisesTheDocumentOrder) {
    // Every key is exercised: rank, then nodes (shorter list first), then rule, then message.
    // The {5,6} pair carries the lexical opposite in its two messages, so the rule key must win.
    std::vector<Finding> findings = {
        {RuleId::SyncNode, Severity::info, {}, "graph event"},
        {RuleId::DuplicateLaunch, Severity::info, {7, 8}, "b"},
        {RuleId::PointlessFork, Severity::info, {5, 6}, "a"},
        {RuleId::SyncNode, Severity::info, {5, 6}, "z"},
        {RuleId::DuplicateLaunch, Severity::info, {7, 8}, "a"},
        {RuleId::PointlessFork, Severity::info, {1, 4}, "a"},
        {RuleId::PointlessFork, Severity::info, {1, 2}, "b"},
        {RuleId::SyncNode, Severity::severe, {9}, "host"},
        {RuleId::LaunchShaped, Severity::warn, {0, 1, 2, 3}, "levels"},
        {RuleId::RedundantEdge, Severity::warn, {2}, "one"},
        {RuleId::RedundantEdge, Severity::warn, {}, "none"},
        {RuleId::DagCheck, Severity::error, {}, "refused"},
    };
    std::sort(findings.begin(), findings.end());

    EXPECT_EQ(describe(findings),
              (std::vector<std::string>{
                  "dag-check error [] refused",
                  "sync-node severe [9] host",
                  "redundant-edge warn [] none",
                  "launch-shaped warn [0 1 2 3] levels",
                  "redundant-edge warn [2] one",
                  "sync-node info [] graph event",
                  "pointless-fork info [1 2] b",
                  "pointless-fork info [1 4] a",
                  "sync-node info [5 6] z",
                  "pointless-fork info [5 6] a",
                  "duplicate-launch info [7 8] a",
                  "duplicate-launch info [7 8] b"}));
    EXPECT_TRUE(std::is_sorted(findings.begin(), findings.end()));
}

TEST(Determinism, TwoRunsOnTheSameGraphAgreeFieldByField) {
    // 18 levels over 20 nodes fires launch-shaped at the boundary (180 >= 180); the rest of the
    // graph fires four more rules: five rules in one result.
    const Graph graph = noisy_graph();
    const Linted first = run_lint(graph);
    const Linted second = run_lint(graph);

    ASSERT_TRUE(first.analysed);
    EXPECT_EQ(describe(first.result.findings),
              (std::vector<std::string>{
                  "sync-node severe [1] host node 1 runs a CPU callback inside the graph and "
                  "serialises its stream",
                  "sync-node severe [2] ext_semaphore node 2 waits on a producer the graph does "
                  "not contain",
                  "launch-shaped warn [] 18 levels over 20 nodes: no two nodes can overlap",
                  "mem-lifecycle warn [] peak 5120 bytes at node 6, 2 live, largest request 4096, "
                  "2 alloc/free pairs",
                  "dag-check warn [3 4] edge [3, 4] appears 2 times; the analysis counts it once",
                  "dag-check warn [18] node 18 has no in- and no out-edge: it runs concurrently, "
                  "and a forgotten dependency looks the same",
                  "dag-check warn [19] node 19 has no in- and no out-edge: it runs concurrently, "
                  "and a forgotten dependency looks the same",
                  "sync-node info [] 2 event nodes record or wait on events: this is how a fork "
                  "is expressed",
                  "duplicate-launch info [5 7 8 9 10 11 12 13 14 15 18 19] 12 kernel nodes launch "
                  "\"axpy\" with the same grid, block and shared bytes: a candidate, not a "
                  "duplicate"}));

    expect_identical(first.result, second.result);
    EXPECT_TRUE(std::is_sorted(first.result.findings.begin(), first.result.findings.end()));
}

TEST(Determinism, EveryArtifactIsIdenticalOnTwoRuns) {
    // The synthetic graph cannot fire redundant-edge or pointless-fork: without this loop those
    // two rules are pinned by single-run tests only.
    for (const char* name : {"built.json", "captured.json", "pingpong.json", "chain.json"}) {
        Graph graph;
        ASSERT_TRUE(load_artifact(name, graph)) << name;
        const Linted first = run_lint(graph);
        const Linted second = run_lint(graph);
        ASSERT_TRUE(first.analysed) << name;
        EXPECT_TRUE(std::is_sorted(first.result.findings.begin(), first.result.findings.end()))
            << name;
        expect_identical(first.result, second.result);
    }
}
