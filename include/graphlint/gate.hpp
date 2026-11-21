#pragma once

#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace graphlint {

enum class Verdict { pass, improvement, regression, suspect };

[[nodiscard]] inline const char* to_string(Verdict verdict) {
    switch (verdict) {
        case Verdict::pass: return "pass";
        case Verdict::improvement: return "improvement";
        case Verdict::regression: return "regression";
        case Verdict::suspect: return "suspect";
    }
    return "suspect";
}

// 0 pass/improvement, 1 regression, 2 suspect: the exit code is the verdict's contract.
[[nodiscard]] inline int exit_code(Verdict verdict) {
    switch (verdict) {
        case Verdict::pass:
        case Verdict::improvement:
            return 0;
        case Verdict::regression:
            return 1;
        case Verdict::suspect:
            return 2;
    }
    return 2;
}

// Absolute integers, strict `>`: a level is a dependency at 100 nodes and at 10 000, while
// a percentage hides it on the large graph and rounds on the small one.
struct GateThresholds {
    int max_depth_increase = 0;
    int max_redundant_increase = 0;
    int max_severe_increase = 0;
};

enum class MetricStatus { regression, improvement, none, context };

[[nodiscard]] inline const char* to_string(MetricStatus status) {
    switch (status) {
        case MetricStatus::regression: return "regression";
        case MetricStatus::improvement: return "improvement";
        case MetricStatus::none: return "none";
        case MetricStatus::context: return "context";
    }
    return "context";
}

struct GateMetric {
    std::string name;
    int baseline = 0;
    int current = 0;
    int delta = 0;
    MetricStatus status = MetricStatus::context;
};

struct GateResult {
    Verdict verdict = Verdict::suspect;
    std::string reason;
    std::vector<GateMetric> metrics;  // eight, in the contract's order
    Environment baseline_environment;
    Environment current_environment;
    std::vector<std::string> notes;
};

namespace detail {

struct Recomputed {
    Topology topo;
    LintResult lint_result;
    GraphError error;
    bool analysed = false;
};

[[nodiscard]] inline Recomputed recompute(const Graph& graph) {
    Recomputed result;
    result.analysed = topology(graph, result.topo, result.error);
    // The error finding, not the bool, is what the verdict reads.
    static_cast<void>(lint(graph, result.topo, result.error, result.lint_result));
    return result;
}

[[nodiscard]] inline int count_severity(const LintResult& lint_result, Severity severity) {
    int count = 0;
    for (const Finding& finding : lint_result.findings) {
        if (finding.severity == severity) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] inline int width_max(const Topology& topo) {
    int max = 0;
    for (const int width : topo.width) {
        max = std::max(max, width);
    }
    return max;
}

// The emitted block sorts its edges, so edge order is not a difference: same model, same bytes.
[[nodiscard]] inline bool same_graph_document(const Graph& a, const Graph& b) {
    if (a.source != b.source || a.weight_kind != b.weight_kind || a.nodes != b.nodes ||
        a.children != b.children) {
        return false;
    }
    std::vector<std::pair<int, int>> left = a.edges;
    std::vector<std::pair<int, int>> right = b.edges;
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

[[nodiscard]] inline bool same_analysis(const ReportAnalysis& declared, const Topology& topo) {
    return declared.depth == topo.depth && declared.distinct_edges == topo.distinct_edges &&
           declared.edges_minimal == topo.edges_minimal &&
           declared.edges_redundant == topo.edges_redundant && declared.width == topo.width;
}

[[nodiscard]] inline bool same_findings(const std::vector<Finding>& declared,
                                        const std::vector<Finding>& computed) {
    return declared == computed;
}

[[nodiscard]] inline bool same_rules(const std::vector<RuleStatus>& declared,
                                     const std::vector<RuleStatus>& computed) {
    if (declared.size() != computed.size()) {
        return false;
    }
    for (std::size_t i = 0; i < declared.size(); ++i) {
        if (declared[i].rule != computed[i].rule ||
            declared[i].applicable != computed[i].applicable ||
            declared[i].reason != computed[i].reason) {
            return false;
        }
    }
    return true;
}

// The document's metric order: the three a threshold judges first, the five context after.
inline void add_metric(GateResult& result, std::string name, int baseline, int current,
                       int threshold, bool decisive) {
    GateMetric metric;
    metric.name = std::move(name);
    metric.baseline = baseline;
    metric.current = current;
    metric.delta = current - baseline;
    if (!decisive) {
        metric.status = MetricStatus::context;
    } else if (metric.delta > threshold) {
        metric.status = MetricStatus::regression;
    } else if (metric.delta < -threshold) {
        metric.status = MetricStatus::improvement;
    } else {
        metric.status = MetricStatus::none;
    }
    result.metrics.push_back(std::move(metric));
}

[[nodiscard]] inline std::string first_error_finding(const char* side,
                                                     const LintResult& lint_result) {
    for (const Finding& finding : lint_result.findings) {
        if (finding.severity == Severity::error) {
            return std::string(side) + " carries an error finding: " + finding.message;
        }
    }
    return std::string();
}

[[nodiscard]] inline std::string join_problems(const std::vector<std::string>& problems) {
    std::string joined;
    for (std::size_t i = 0; i < problems.size(); ++i) {
        if (i != 0) {
            joined += "; ";
        }
        joined += problems[i];
    }
    return joined;
}

}  // namespace detail

// Decides on the recomputed topology and lint, never on a declared block: a stale `analysis`
// or `findings` yields a note. Order is suspect, regression, improvement, pass.
[[nodiscard]] inline GateResult compare(const ReportDocument& baseline_report,
                                        const ReportDocument& current_report,
                                        const GateThresholds& thresholds) {
    GateResult result;
    result.baseline_environment = baseline_report.environment;
    result.current_environment = current_report.environment;

    const detail::Recomputed baseline = detail::recompute(baseline_report.graph);
    const detail::Recomputed current = detail::recompute(current_report.graph);

    if (!detail::same_analysis(baseline_report.analysis, baseline.topo)) {
        result.notes.push_back(
            "baseline: declared analysis disagrees with the recomputed topology");
    }
    if (!detail::same_findings(baseline_report.findings, baseline.lint_result.findings)) {
        result.notes.push_back("baseline: declared findings disagree with the recomputed lint");
    }
    if (!detail::same_rules(baseline_report.rules, baseline.lint_result.rules)) {
        result.notes.push_back(
            "baseline: declared applicability disagrees with the recomputed lint");
    }
    if (!detail::same_analysis(current_report.analysis, current.topo)) {
        result.notes.push_back(
            "current: declared analysis disagrees with the recomputed topology");
    }
    if (!detail::same_findings(current_report.findings, current.lint_result.findings)) {
        result.notes.push_back("current: declared findings disagree with the recomputed lint");
    }
    if (!detail::same_rules(current_report.rules, current.lint_result.rules)) {
        result.notes.push_back(
            "current: declared applicability disagrees with the recomputed lint");
    }
    if (detail::same_graph_document(baseline_report.graph, current_report.graph)) {
        result.notes.push_back("baseline and current carry the same graph");
    }
    if (baseline_report.environment.device.empty() != current_report.environment.device.empty()) {
        result.notes.push_back(std::string("device unknown on ") +
                               (baseline_report.environment.device.empty() ? "baseline"
                                                                          : "current") +
                               "; the analysis is toolkit-independent");
    }
    if (baseline_report.environment.cuda_runtime != current_report.environment.cuda_runtime) {
        result.notes.push_back("cuda_runtime differs: " +
                               std::to_string(baseline_report.environment.cuda_runtime) + " vs " +
                               std::to_string(current_report.environment.cuda_runtime) +
                               "; the analysis is toolkit-independent");
    }

    detail::add_metric(result, "depth", baseline.topo.depth, current.topo.depth,
                       thresholds.max_depth_increase, true);
    detail::add_metric(result, "edges_redundant",
                       static_cast<int>(baseline.topo.edges_redundant.size()),
                       static_cast<int>(current.topo.edges_redundant.size()),
                       thresholds.max_redundant_increase, true);
    detail::add_metric(result, "findings_severe",
                       detail::count_severity(baseline.lint_result, Severity::severe),
                       detail::count_severity(current.lint_result, Severity::severe),
                       thresholds.max_severe_increase, true);
    detail::add_metric(result, "nodes", static_cast<int>(baseline_report.graph.nodes.size()),
                       static_cast<int>(current_report.graph.nodes.size()), 0, false);
    detail::add_metric(result, "edges_distinct", baseline.topo.distinct_edges,
                       current.topo.distinct_edges, 0, false);
    detail::add_metric(result, "width_max", detail::width_max(baseline.topo),
                       detail::width_max(current.topo), 0, false);
    detail::add_metric(result, "findings_warn",
                       detail::count_severity(baseline.lint_result, Severity::warn),
                       detail::count_severity(current.lint_result, Severity::warn), 0, false);
    detail::add_metric(result, "findings_info",
                       detail::count_severity(baseline.lint_result, Severity::info),
                       detail::count_severity(current.lint_result, Severity::info), 0, false);

    std::vector<std::string> problems;
    if (baseline_report.graph.source != current_report.graph.source) {
        problems.push_back(std::string("different source kind: ") +
                           detail::source_token(baseline_report.graph.source) + " vs " +
                           detail::source_token(current_report.graph.source));
    }
    if (baseline_report.graph.weight_kind == WeightKind::Nanoseconds ||
        current_report.graph.weight_kind == WeightKind::Nanoseconds) {
        problems.push_back("weight_kind ns has no threshold before M7");
    } else if (baseline_report.graph.weight_kind != current_report.graph.weight_kind) {
        problems.push_back(std::string("different weight_kind: ") +
                           detail::weight_token(baseline_report.graph.weight_kind) + " vs " +
                           detail::weight_token(current_report.graph.weight_kind));
    }
    const std::string baseline_error =
        detail::first_error_finding("baseline", baseline.lint_result);
    if (!baseline_error.empty()) {
        problems.push_back(baseline_error);
    }
    const std::string current_error =
        detail::first_error_finding("current", current.lint_result);
    if (!current_error.empty()) {
        problems.push_back(current_error);
    }
    const std::string& baseline_device = baseline_report.environment.device;
    const std::string& current_device = current_report.environment.device;
    if (!baseline_device.empty() && !current_device.empty() &&
        baseline_device != current_device) {
        problems.push_back("different device: " + baseline_device + " vs " + current_device);
    }

    if (!problems.empty()) {
        result.verdict = Verdict::suspect;
        result.reason = detail::join_problems(problems);
        return result;
    }

    const auto threshold_of = [&thresholds](const std::string& name) {
        if (name == "depth") {
            return thresholds.max_depth_increase;
        }
        if (name == "edges_redundant") {
            return thresholds.max_redundant_increase;
        }
        return thresholds.max_severe_increase;
    };

    for (const GateMetric& metric : result.metrics) {
        if (metric.status == MetricStatus::regression) {
            result.verdict = Verdict::regression;
            result.reason = metric.name + " grew " + std::to_string(metric.delta) + " > " +
                            std::to_string(threshold_of(metric.name));
            return result;
        }
    }
    for (const GateMetric& metric : result.metrics) {
        if (metric.status == MetricStatus::improvement) {
            result.verdict = Verdict::improvement;
            result.reason = metric.name + " fell " + std::to_string(-metric.delta) + " > " +
                            std::to_string(threshold_of(metric.name));
            return result;
        }
    }

    result.verdict = Verdict::pass;
    result.reason = "all metrics inside their thresholds";
    return result;
}

// Deterministic JSON, sorted keys, LF, no float: the same two reports always produce it.
[[nodiscard]] inline std::string dump_gate(const GateResult& result,
                                           const GateThresholds& thresholds) {
    std::string out;
    out += "{\n";
    out += "  \"baseline\": {\"cuda_runtime\": ";
    detail::append_int(out, result.baseline_environment.cuda_runtime);
    out += ", \"device\": ";
    detail::append_json_string(out, result.baseline_environment.device);
    out += "},\n";
    out += "  \"current\": {\"cuda_runtime\": ";
    detail::append_int(out, result.current_environment.cuda_runtime);
    out += ", \"device\": ";
    detail::append_json_string(out, result.current_environment.device);
    out += "},\n";

    out += "  \"metrics\": ";
    if (result.metrics.empty()) {
        out += "[]";
    } else {
        out += "[\n";
        for (std::size_t i = 0; i < result.metrics.size(); ++i) {
            const GateMetric& metric = result.metrics[i];
            out += "    {\"baseline\": ";
            detail::append_int(out, metric.baseline);
            out += ", \"current\": ";
            detail::append_int(out, metric.current);
            out += ", \"delta\": ";
            detail::append_int(out, metric.delta);
            out += ", \"name\": ";
            detail::append_json_string(out, metric.name);
            out += ", \"status\": \"";
            out += to_string(metric.status);
            out += "\"}";
            out += i + 1 < result.metrics.size() ? ",\n" : "\n";
        }
        out += "  ]";
    }
    out += ",\n";

    out += "  \"notes\": ";
    if (result.notes.empty()) {
        out += "[]";
    } else {
        out += "[\n";
        for (std::size_t i = 0; i < result.notes.size(); ++i) {
            out += "    ";
            detail::append_json_string(out, result.notes[i]);
            out += i + 1 < result.notes.size() ? ",\n" : "\n";
        }
        out += "  ]";
    }
    out += ",\n";

    out += "  \"reason\": ";
    detail::append_json_string(out, result.reason);
    out += ",\n";
    out += "  \"schema_version\": 1,\n";
    out += "  \"thresholds\": {\"max_depth_increase\": ";
    detail::append_int(out, thresholds.max_depth_increase);
    out += ", \"max_redundant_increase\": ";
    detail::append_int(out, thresholds.max_redundant_increase);
    out += ", \"max_severe_increase\": ";
    detail::append_int(out, thresholds.max_severe_increase);
    out += "},\n";
    out += "  \"verdict\": \"";
    out += to_string(result.verdict);
    out += "\"\n";
    out += "}\n";
    return out;
}

}  // namespace graphlint
