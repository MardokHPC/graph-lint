#pragma once

// The report CLI's body, separated from argv and file I/O so the exit policy is testable by
// name: 0 whenever a document is produced, 2 when an emitter's precondition fails, never 1.

#include <graphlint/analysis.hpp>
#include <graphlint/emit_dot.hpp>
#include <graphlint/emit_mermaid.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace graphlint {
namespace tools {

enum class ReportFormat { summary, json, dot, mermaid };

struct ReportOptions {
    ReportFormat format = ReportFormat::summary;
    bool detail_params = false;
    int max_nodes = 80;
    Environment environment;  // filled by the CLI, never by the core
    std::string source;       // prefix for a stderr position; empty when there is no file
};

[[nodiscard]] inline int max_width(const Topology& topo) {
    int width = 0;
    for (const int level_width : topo.width) {
        width = level_width > width ? level_width : width;
    }
    return width;
}

inline void append_ids(std::string& out, const std::vector<int>& ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        graphlint::detail::append_int(out, ids[i]);
    }
}

[[nodiscard]] inline std::string summary_document(const Graph& graph, const Topology& topo,
                                                  const LintResult& lint_result) {
    std::string out;
    out += "source: ";
    out += graphlint::detail::source_token(graph.source);
    out += "\nweight_kind: ";
    out += graphlint::detail::weight_token(graph.weight_kind);
    out += "\nnodes: ";
    graphlint::detail::append_int(out, static_cast<int>(graph.nodes.size()));
    out += "\nedges: ";
    graphlint::detail::append_int(out, static_cast<int>(graph.edges.size()));
    out += "\ndistinct_edges: ";
    graphlint::detail::append_int(out, topo.distinct_edges);
    out += "\ndepth: ";
    graphlint::detail::append_int(out, topo.depth);
    out += " levels\nwidth_max: ";
    graphlint::detail::append_int(out, max_width(topo));
    out += "\nminimal_dag: ";
    graphlint::detail::append_int(out, static_cast<int>(topo.edges_minimal.size()));
    out += " edges\nfindings: ";
    graphlint::detail::append_int(out, static_cast<int>(lint_result.findings.size()));
    out += '\n';
    for (const Finding& finding : lint_result.findings) {
        out += "finding: ";
        out += to_string(finding.severity);
        out += ' ';
        out += to_string(finding.rule);
        out += " [";
        append_ids(out, finding.nodes);
        out += "]: ";
        out += finding.message;
        out += '\n';
    }
    for (const RuleStatus& status : lint_result.rules) {
        if (status.applicable) {
            continue;
        }
        out += "not_applicable: ";
        out += to_string(status.rule);
        out += " (";
        out += status.reason;
        out += ")\n";
    }
    return out;
}

[[nodiscard]] inline int run_report(const Graph& graph, const ReportOptions& options,
                                    std::ostream& out, std::ostream& err) {
    Topology topo;
    GraphError error;
    const bool analysed = topology(graph, topo, error);
    LintResult lint_result;
    static_cast<void>(lint(graph, topo, error, lint_result));

    // The emitters draw a topology that exists; summary and json still answer a refused graph.
    if (!analysed && (options.format == ReportFormat::dot ||
                      options.format == ReportFormat::mermaid)) {
        if (!options.source.empty()) {
            err << options.source << ':';
        }
        err << error.line << ':' << error.column << ": " << error.detail << '\n';
        return 2;
    }

    std::string document;
    switch (options.format) {
        case ReportFormat::summary:
            document = summary_document(graph, topo, lint_result);
            break;
        case ReportFormat::json:
            document = dump_report(graph, topo, lint_result, options.environment);
            break;
        case ReportFormat::dot: {
            DotOptions dot;
            dot.detail_params = options.detail_params;
            document = to_dot(graph, topo, dot);
            break;
        }
        case ReportFormat::mermaid: {
            MermaidOptions mermaid;
            mermaid.max_nodes = options.max_nodes;
            document = to_mermaid(graph, topo, mermaid);
            break;
        }
    }
    out.write(document.data(), static_cast<std::streamsize>(document.size()));
    // An error finding still produces a document: a report reports, it never judges.
    return 0;
}

}  // namespace tools
}  // namespace graphlint
