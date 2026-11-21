#pragma once

#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/rules.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace graphlint {

// Always present in the document; an unknown machine is 0/"" rather than a missing key.
struct Environment {
    std::string device;
    int cuda_runtime = 0;
};

// The analysis block as the document declares it: the gate recomputes and notes a
// disagreement, so a stale copy is a note, never a verdict.
struct ReportAnalysis {
    int depth = 0;
    int distinct_edges = 0;
    std::vector<std::pair<int, int>> edges_minimal;
    std::vector<std::pair<int, int>> edges_redundant;
    std::vector<int> width;
};

struct ReportDocument {
    int schema_version = 1;
    Graph graph;
    Environment environment;
    std::vector<Finding> findings;  // declared, in the document's order
    std::vector<RuleStatus> rules;  // declared applicability, seven in RuleId order
    ReportAnalysis analysis;
};

namespace detail {

inline void append_int_list(std::string& out, const std::vector<int>& values) {
    if (values.empty()) {
        out += "[]";
        return;
    }
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        append_int(out, values[i]);
    }
    out += ']';
}

inline void append_pair_list(std::string& out, const std::vector<std::pair<int, int>>& pairs,
                             std::size_t item_indent, std::size_t close_indent) {
    if (pairs.empty()) {
        out += "[]";
        return;
    }
    const std::string item(item_indent, ' ');
    const std::string close(close_indent, ' ');
    out += "[\n";
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        out += item;
        out += '[';
        append_int(out, pairs[i].first);
        out += ", ";
        append_int(out, pairs[i].second);
        out += ']';
        out += i + 1 < pairs.size() ? ",\n" : "\n";
    }
    out += close;
    out += ']';
}

inline void append_rule_list(std::string& out, const LintResult& lint_result, bool applicable) {
    std::vector<const RuleStatus*> listed;
    for (const RuleStatus& status : lint_result.rules) {
        if (status.applicable == applicable) {
            listed.push_back(&status);
        }
    }
    if (listed.empty()) {
        out += "[]";
        return;
    }
    out += "[\n";
    for (std::size_t i = 0; i < listed.size(); ++i) {
        out += "      {\"reason\": ";
        append_json_string(out, listed[i]->reason);
        out += ", \"rule\": \"";
        out += to_string(listed[i]->rule);
        out += "\"}";
        out += i + 1 < listed.size() ? ",\n" : "\n";
    }
    out += "    ]";
}

inline void append_findings(std::string& out, const std::vector<Finding>& findings) {
    if (findings.empty()) {
        out += "[]";
        return;
    }
    out += "[\n";
    for (std::size_t i = 0; i < findings.size(); ++i) {
        const Finding& finding = findings[i];
        out += "    {\"message\": ";
        append_json_string(out, finding.message);
        out += ", \"nodes\": ";
        append_int_list(out, finding.nodes);
        out += ", \"rule\": \"";
        out += to_string(finding.rule);
        out += "\", \"severity\": \"";
        out += to_string(finding.severity);
        out += "\"}";
        out += i + 1 < findings.size() ? ",\n" : "\n";
    }
    out += "  ]";
}

}  // namespace detail

// Exactly the document of docs/report.md section 1: sorted keys, two-space indent, one node
// per line, LF, no float, and a refused graph still writes its zeroed analysis.
[[nodiscard]] inline std::string dump_report(const Graph& graph, const Topology& topo,
                                             const LintResult& lint_result,
                                             const Environment& environment) {
    std::string out;
    out.reserve(512 + graph.nodes.size() * 64 + topo.edges_minimal.size() * 12);

    out += "{\n";
    out += "  \"analysis\": {\n";
    out += "    \"depth\": ";
    detail::append_int(out, topo.depth);
    out += ",\n    \"distinct_edges\": ";
    detail::append_int(out, topo.distinct_edges);
    out += ",\n    \"edges_minimal\": ";
    detail::append_pair_list(out, topo.edges_minimal, 6, 4);
    out += ",\n    \"edges_redundant\": ";
    detail::append_pair_list(out, topo.edges_redundant, 6, 4);
    out += ",\n    \"width\": ";
    detail::append_int_list(out, topo.width);
    out += "\n  },\n";

    out += "  \"applicability\": {\n";
    out += "    \"applicable\": ";
    detail::append_rule_list(out, lint_result, true);
    out += ",\n    \"not_applicable\": ";
    detail::append_rule_list(out, lint_result, false);
    out += "\n  },\n";

    out += "  \"environment\": {\"cuda_runtime\": ";
    detail::append_int(out, environment.cuda_runtime);
    out += ", \"device\": ";
    detail::append_json_string(out, environment.device);
    out += "},\n";

    out += "  \"findings\": ";
    detail::append_findings(out, lint_result.findings);
    out += ",\n";

    out += "  \"graph\": ";
    detail::dump_graph_into(out, graph, 1, true);
    out += ",\n";
    out += "  \"schema_version\": 1\n";
    out += "}\n";
    return out;
}

namespace detail {

// The tokens of rules.hpp are the schema; matching them by to_string keeps one spelling.
[[nodiscard]] inline bool rule_from_token(std::string_view token, RuleId& rule) {
    for (std::size_t i = 0; i < k_rule_count; ++i) {
        const RuleId candidate = static_cast<RuleId>(i);
        if (token == to_string(candidate)) {
            rule = candidate;
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool severity_from_token(std::string_view token, Severity& severity) {
    const Severity all[] = {Severity::error, Severity::severe, Severity::warn, Severity::info};
    for (const Severity candidate : all) {
        if (token == to_string(candidate)) {
            severity = candidate;
            return true;
        }
    }
    return false;
}

// Reads report.json only: the graph block is handed to parse_graph, everything else follows
// the same ordered-key grammar, so a document the writer produced is the only one accepted.
class ReportReader : public Scanner {
public:
    ReportReader(std::string_view text, GraphError& error) : Scanner(text, error) {}

    [[nodiscard]] bool parse(ReportDocument& out) {
        skip_whitespace();
        ReportDocument document;
        const std::size_t object_pos = pos();
        bool has_analysis = false;
        bool has_applicability = false;
        bool has_environment = false;
        bool has_findings = false;
        bool has_graph = false;
        bool has_schema_version = false;

        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "analysis") {
                has_analysis = true;
                return parse_analysis(document.analysis);
            }
            if (key == "applicability") {
                has_applicability = true;
                return parse_applicability(document.rules);
            }
            if (key == "environment") {
                has_environment = true;
                return parse_environment(document.environment);
            }
            if (key == "findings") {
                has_findings = true;
                return parse_findings(document.findings);
            }
            if (key == "graph") {
                has_graph = true;
                return parse_graph_value(document.graph);
            }
            if (key == "schema_version") {
                has_schema_version = true;
                const std::size_t value_pos = pos();
                int version = 0;
                if (!parse_int(version)) {
                    return false;
                }
                if (version != 1) {
                    return fail(GraphErrorKind::SchemaVersion, value_pos,
                                "unsupported schema_version");
                }
                document.schema_version = version;
                return true;
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }

        if (!has_analysis) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"analysis\"");
        }
        if (!has_applicability) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"applicability\"");
        }
        if (!has_environment) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"environment\"");
        }
        if (!has_findings) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"findings\"");
        }
        if (!has_graph) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"graph\"");
        }
        if (!has_schema_version) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"schema_version\"");
        }

        skip_whitespace();
        if (!at_end()) {
            return fail(GraphErrorKind::Trailing, pos(), "trailing bytes after the document");
        }
        out = std::move(document);
        return true;
    }

private:
    [[nodiscard]] bool parse_analysis(ReportAnalysis& analysis) {
        const std::size_t object_pos = pos();
        bool has_depth = false;
        bool has_distinct_edges = false;
        bool has_edges_minimal = false;
        bool has_edges_redundant = false;
        bool has_width = false;
        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "depth") {
                has_depth = true;
                return parse_int(analysis.depth);
            }
            if (key == "distinct_edges") {
                has_distinct_edges = true;
                return parse_int(analysis.distinct_edges);
            }
            if (key == "edges_minimal") {
                has_edges_minimal = true;
                return parse_pairs(analysis.edges_minimal);
            }
            if (key == "edges_redundant") {
                has_edges_redundant = true;
                return parse_pairs(analysis.edges_redundant);
            }
            if (key == "width") {
                has_width = true;
                return parse_int_list(analysis.width);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }
        if (!has_depth) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"depth\"");
        }
        if (!has_distinct_edges) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"distinct_edges\"");
        }
        if (!has_edges_minimal) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"edges_minimal\"");
        }
        if (!has_edges_redundant) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"edges_redundant\"");
        }
        if (!has_width) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"width\"");
        }
        return true;
    }

    [[nodiscard]] bool parse_applicability(std::vector<RuleStatus>& rules) {
        const std::size_t object_pos = pos();
        std::vector<RuleStatus> declared;
        bool has_applicable = false;
        bool has_not_applicable = false;
        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "applicable") {
                has_applicable = true;
                return parse_rule_list(declared, true);
            }
            if (key == "not_applicable") {
                has_not_applicable = true;
                return parse_rule_list(declared, false);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }
        if (!has_applicable) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"applicable\"");
        }
        if (!has_not_applicable) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"not_applicable\"");
        }

        // Seven rules, each once: the table is the document, so a short one is a broken report.
        std::vector<RuleStatus> ordered(k_rule_count);
        std::vector<char> seen(k_rule_count, 0);
        for (const RuleStatus& status : declared) {
            const std::size_t index = rule_index(status.rule);
            if (seen[index] != 0) {
                return fail(GraphErrorKind::DuplicateKey, object_pos,
                            "duplicate rule \"" + std::string(to_string(status.rule)) + "\"");
            }
            seen[index] = 1;
            ordered[index] = status;
        }
        for (std::size_t i = 0; i < k_rule_count; ++i) {
            if (seen[i] == 0) {
                return fail(GraphErrorKind::MissingKey, object_pos,
                            "missing rule \"" + std::string(to_string(static_cast<RuleId>(i))) +
                                "\"");
            }
        }
        rules = std::move(ordered);
        return true;
    }

    [[nodiscard]] bool parse_rule_list(std::vector<RuleStatus>& into, bool applicable) {
        return parse_array([&]() {
            const std::size_t entry_pos = pos();
            RuleStatus status;
            status.applicable = applicable;
            bool has_reason = false;
            bool has_rule = false;
            const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
                if (key == "reason") {
                    has_reason = true;
                    return parse_string(status.reason);
                }
                if (key == "rule") {
                    has_rule = true;
                    const std::size_t value_pos = pos();
                    std::string token;
                    if (!parse_string(token)) {
                        return false;
                    }
                    if (!rule_from_token(token, status.rule)) {
                        return fail(GraphErrorKind::UnknownType, value_pos,
                                    "unknown rule \"" + token + "\"");
                    }
                    return true;
                }
                return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
            });
            if (!parsed) {
                return false;
            }
            if (!has_reason) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"reason\"");
            }
            if (!has_rule) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"rule\"");
            }
            into.push_back(std::move(status));
            return true;
        });
    }

    [[nodiscard]] bool parse_environment(Environment& environment) {
        const std::size_t object_pos = pos();
        bool has_cuda_runtime = false;
        bool has_device = false;
        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "cuda_runtime") {
                has_cuda_runtime = true;
                return parse_int(environment.cuda_runtime);
            }
            if (key == "device") {
                has_device = true;
                return parse_string(environment.device);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }
        if (!has_cuda_runtime) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"cuda_runtime\"");
        }
        if (!has_device) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"device\"");
        }
        return true;
    }

    [[nodiscard]] bool parse_findings(std::vector<Finding>& findings) {
        return parse_array([&]() {
            const std::size_t entry_pos = pos();
            Finding finding;
            bool has_message = false;
            bool has_nodes = false;
            bool has_rule = false;
            bool has_severity = false;
            const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
                if (key == "message") {
                    has_message = true;
                    return parse_string(finding.message);
                }
                if (key == "nodes") {
                    has_nodes = true;
                    return parse_int_list(finding.nodes);
                }
                if (key == "rule") {
                    has_rule = true;
                    const std::size_t value_pos = pos();
                    std::string token;
                    if (!parse_string(token)) {
                        return false;
                    }
                    if (!rule_from_token(token, finding.rule)) {
                        return fail(GraphErrorKind::UnknownType, value_pos,
                                    "unknown rule \"" + token + "\"");
                    }
                    return true;
                }
                if (key == "severity") {
                    has_severity = true;
                    const std::size_t value_pos = pos();
                    std::string token;
                    if (!parse_string(token)) {
                        return false;
                    }
                    if (!severity_from_token(token, finding.severity)) {
                        return fail(GraphErrorKind::UnknownType, value_pos,
                                    "unknown severity \"" + token + "\"");
                    }
                    return true;
                }
                return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
            });
            if (!parsed) {
                return false;
            }
            if (!has_message) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"message\"");
            }
            if (!has_nodes) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"nodes\"");
            }
            if (!has_rule) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"rule\"");
            }
            if (!has_severity) {
                return fail(GraphErrorKind::MissingKey, entry_pos, "missing key \"severity\"");
            }
            findings.push_back(std::move(finding));
            return true;
        });
    }

    [[nodiscard]] bool parse_int_list(std::vector<int>& values) {
        return parse_array([&]() {
            int value = 0;
            if (!parse_int(value)) {
                return false;
            }
            values.push_back(value);
            return true;
        });
    }

    [[nodiscard]] bool parse_pairs(std::vector<std::pair<int, int>>& pairs) {
        return parse_array([&]() {
            if (!expect('[')) {
                return false;
            }
            std::pair<int, int> pair{0, 0};
            skip_whitespace();
            if (!parse_int(pair.first)) {
                return false;
            }
            skip_whitespace();
            if (!expect(',')) {
                return false;
            }
            skip_whitespace();
            if (!parse_int(pair.second)) {
                return false;
            }
            skip_whitespace();
            if (!expect(']')) {
                return false;
            }
            pairs.push_back(pair);
            return true;
        });
    }

    // The graph block is graph.json nested one level: find its extent with a balanced scan
    // that respects strings, hand the bytes to parse_graph, then map its position back.
    [[nodiscard]] bool parse_graph_value(Graph& graph) {
        const std::size_t start = pos();
        std::size_t end = 0;
        if (!skip_compound(end)) {
            return false;
        }
        Graph parsed_graph;
        GraphError inner;
        const std::string_view block(text_.data() + start, end - start);
        if (!parse_graph(block, parsed_graph, inner)) {
            int line = 1;
            int column = 1;
            position_of(start, line, column);
            const int nested_line = inner.line < 1 ? line : line + inner.line - 1;
            const int nested_column =
                inner.line <= 1 ? column + inner.column - 1 : inner.column;
            return fail_at(inner.kind, nested_line, nested_column, inner.detail);
        }
        seek(end);
        graph = std::move(parsed_graph);
        return true;
    }
};

}  // namespace detail

// False leaves `error` with a 1-based line and column in the report, the graph block included.
[[nodiscard]] inline bool parse_report(std::string_view text, ReportDocument& out,
                                       GraphError& error) {
    detail::ReportReader reader(text, error);
    ReportDocument document;
    if (!reader.parse(document)) {
        return false;
    }
    out = std::move(document);
    error = GraphError{};
    return true;
}

}  // namespace graphlint
