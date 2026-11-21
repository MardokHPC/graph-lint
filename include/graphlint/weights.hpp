#pragma once

#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace graphlint {

// 1e12 ns is an hour; a wider value is a wrong unit, not a slow node.
inline constexpr std::uint64_t k_max_weight_ns = 1000000000000ull;

struct WeightsTable {
    std::vector<std::uint64_t> ns;  // by node id
    // The `graph` binding: apply_weights recomputes it, and a mismatch names another graph.
    std::uint64_t fingerprint = 0;
};

namespace detail {

// The structural key canonicalize sorts by, at one node: the refinement below is the only
// place a neighbour's key enters, so this stays a pure function of the node and its degrees.
[[nodiscard]] inline std::uint64_t node_key(const Node& node, std::uint32_t in_degree,
                                            std::uint32_t out_degree) {
    std::uint64_t hash = 14695981039346656037ull;
    fnv1a_u64(hash, static_cast<std::uint64_t>(type_rank(node.type)));
    fnv1a_u64(hash, static_cast<std::uint64_t>(node.name.size()));
    fnv1a_bytes(hash, node.name.data(), node.name.size());
    for (const std::uint32_t dim : node.grid) {
        fnv1a_u64(hash, dim);
    }
    for (const std::uint32_t dim : node.block) {
        fnv1a_u64(hash, dim);
    }
    fnv1a_u64(hash, node.shared_bytes);
    fnv1a_u64(hash, node.alloc_bytes);
    fnv1a_u64(hash, node.mem_ptr);
    fnv1a_u64(hash, in_degree);
    fnv1a_u64(hash, out_degree);
    return hash;
}

}  // namespace detail

// FNV-1a over one refinement round of each node's key and its neighbours' keys, then over the
// sorted multisets of those keys and of the edge keys: no canonical id enters the hash, so a
// relabelled copy of the same graph keeps the binding.
[[nodiscard]] inline std::uint64_t graph_fingerprint(const Graph& graph) {
    const std::size_t n = graph.nodes.size();

    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    // An endpoint outside the node list cannot name a neighbour; the node count still covers
    // such a graph bit for bit, and canonicalize never produces one.
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [n](const std::pair<int, int>& edge) {
                                   return edge.first < 0 || edge.second < 0 ||
                                          static_cast<std::size_t>(edge.first) >= n ||
                                          static_cast<std::size_t>(edge.second) >= n;
                               }),
                edges.end());

    std::vector<std::uint32_t> in_degree(n, 0);
    std::vector<std::uint32_t> out_degree(n, 0);
    for (const std::pair<int, int>& edge : edges) {
        ++out_degree[static_cast<std::size_t>(edge.first)];
        ++in_degree[static_cast<std::size_t>(edge.second)];
    }

    std::vector<std::uint64_t> key(n, 0);
    std::vector<std::vector<std::uint64_t>> successors(n);
    std::vector<std::vector<std::uint64_t>> predecessors(n);
    for (std::size_t i = 0; i < n; ++i) {
        key[i] = detail::node_key(graph.nodes[i], in_degree[i], out_degree[i]);
    }
    for (const std::pair<int, int>& edge : edges) {
        const std::size_t from = static_cast<std::size_t>(edge.first);
        const std::size_t to = static_cast<std::size_t>(edge.second);
        successors[from].push_back(key[to]);
        predecessors[to].push_back(key[from]);
    }

    // One round: a node is its own key plus the sorted keys of its neighbours.
    std::vector<std::uint64_t> refined(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t mixed = key[i];
        detail::fnv1a_u64(mixed, detail::fnv1a_neighbors(std::move(successors[i])));
        detail::fnv1a_u64(mixed, detail::fnv1a_neighbors(std::move(predecessors[i])));
        refined[i] = mixed;
    }

    std::uint64_t hash = 14695981039346656037ull;
    std::vector<std::uint64_t> nodes = refined;
    std::sort(nodes.begin(), nodes.end());
    detail::fnv1a_u64(hash, static_cast<std::uint64_t>(nodes.size()));
    for (const std::uint64_t value : nodes) {
        detail::fnv1a_u64(hash, value);
    }

    // Direction stays in the pair: a reversed chain is a different graph, not a relabelling.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(edges.size());
    for (const std::pair<int, int>& edge : edges) {
        pairs.emplace_back(refined[static_cast<std::size_t>(edge.first)],
                           refined[static_cast<std::size_t>(edge.second)]);
    }
    std::sort(pairs.begin(), pairs.end());
    detail::fnv1a_u64(hash, static_cast<std::uint64_t>(pairs.size()));
    for (const std::pair<std::uint64_t, std::uint64_t>& pair : pairs) {
        detail::fnv1a_u64(hash, pair.first);
        detail::fnv1a_u64(hash, pair.second);
    }
    return hash;
}

namespace detail {

// 16 lowercase hex, zero padded: the writer and the reader share one spelling of the binding.
inline void append_fingerprint(std::string& out, std::uint64_t value) {
    constexpr char k_hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += k_hex[(value >> shift) & 0x0fu];
    }
}

[[nodiscard]] inline bool fingerprint_from_token(std::string_view token, std::uint64_t& value) {
    if (token.size() != 16) {
        return false;
    }
    std::uint64_t result = 0;
    for (const char ch : token) {
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = ch - 'a' + 10;
        }
        if (digit < 0) {
            return false;
        }
        result = result * 16 + static_cast<std::uint64_t>(digit);
    }
    value = result;
    return true;
}

// weights.json, docs/weights.md section 1: the same ordered-key grammar as graph.json, so a
// document this reader produced is the only one accepted.
class WeightsReader : public Scanner {
public:
    WeightsReader(std::string_view text, GraphError& error) : Scanner(text, error) {}

    [[nodiscard]] bool parse(WeightsTable& out) {
        skip_whitespace();
        const std::size_t object_pos = pos();
        std::vector<Row> rows;
        std::uint64_t fingerprint = 0;
        int node_count = 0;
        bool has_graph = false;
        bool has_node_count = false;
        bool has_schema_version = false;
        bool has_unit = false;
        bool has_weights = false;

        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "graph") {
                has_graph = true;
                return parse_fingerprint(fingerprint);
            }
            if (key == "node_count") {
                has_node_count = true;
                const std::size_t value_pos = pos();
                if (!parse_int(node_count)) {
                    return false;
                }
                if (node_count < 0) {
                    return fail(GraphErrorKind::BadNumber, value_pos,
                                "node_count is negative");
                }
                return true;
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
                return true;
            }
            if (key == "unit") {
                has_unit = true;
                return parse_unit();
            }
            if (key == "weights") {
                has_weights = true;
                return parse_rows(rows);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }

        if (!has_graph) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"graph\"");
        }
        if (!has_node_count) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"node_count\"");
        }
        if (!has_schema_version) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"schema_version\"");
        }
        if (!has_unit) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"unit\"");
        }
        if (!has_weights) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"weights\"");
        }
        if (!check_rows(rows, node_count, object_pos)) {
            return false;
        }

        skip_whitespace();
        if (!at_end()) {
            return fail(GraphErrorKind::Trailing, pos(), "trailing bytes after the document");
        }

        WeightsTable table;
        table.ns.reserve(rows.size());
        for (const Row& row : rows) {
            table.ns.push_back(row.ns);
        }
        table.fingerprint = fingerprint;
        out = std::move(table);
        return true;
    }

private:
    struct Row {
        int id = 0;
        std::uint64_t ns = 0;
        std::size_t pos = 0;
    };

    [[nodiscard]] bool parse_fingerprint(std::uint64_t& fingerprint) {
        const std::size_t value_pos = pos();
        std::string token;
        if (!parse_string(token)) {
            return false;
        }
        if (!fingerprint_from_token(token, fingerprint)) {
            return fail(GraphErrorKind::WeightRule, value_pos,
                        "graph must be 16 lowercase hex digits");
        }
        return true;
    }

    // Never rescaled: a table in another unit is a producer bug, not a factor of 1000.
    [[nodiscard]] bool parse_unit() {
        const std::size_t value_pos = pos();
        std::string token;
        if (!parse_string(token)) {
            return false;
        }
        if (token != "ns") {
            return fail(GraphErrorKind::WeightRule, value_pos,
                        "unit \"" + token + "\" is not \"ns\"");
        }
        return true;
    }

    [[nodiscard]] bool parse_rows(std::vector<Row>& rows) {
        return parse_array([&]() {
            Row row;
            row.pos = pos();
            if (!expect('[')) {
                return false;
            }
            skip_whitespace();
            if (!parse_int(row.id)) {
                return false;
            }
            skip_whitespace();
            if (!expect(',')) {
                return false;
            }
            skip_whitespace();
            if (!parse_uint64(row.ns)) {
                return false;
            }
            skip_whitespace();
            if (!expect(']')) {
                return false;
            }
            rows.push_back(row);
            return true;
        });
    }

    [[nodiscard]] bool check_rows(const std::vector<Row>& rows, int node_count,
                                  std::size_t object_pos) {
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const Row& row = rows[k];
            const std::string at = "weight row " + std::to_string(k);
            if (row.id < 0) {
                return fail(GraphErrorKind::WeightRule, row.pos,
                            at + " has negative id " + std::to_string(row.id));
            }
            if (k != 0 && row.id == rows[k - 1].id) {
                return fail(GraphErrorKind::WeightRule, row.pos,
                            at + " repeats id " + std::to_string(row.id));
            }
            if (k != 0 && row.id < rows[k - 1].id) {
                return fail(GraphErrorKind::WeightRule, row.pos,
                            at + " carries id " + std::to_string(row.id) + " after " +
                                std::to_string(rows[k - 1].id) + ": rows ascend by id");
            }
            if (row.id != static_cast<int>(k)) {
                return fail(GraphErrorKind::WeightRule, row.pos,
                            at + ": expected id " + std::to_string(k) + ", found " +
                                std::to_string(row.id));
            }
            if (row.ns > k_max_weight_ns) {
                return fail(GraphErrorKind::WeightRule, row.pos,
                            at + " has id " + std::to_string(row.id) + " at " +
                                std::to_string(row.ns) + " ns, over 1e12");
            }
        }
        if (rows.size() != static_cast<std::size_t>(node_count)) {
            return fail(GraphErrorKind::WeightRule, object_pos,
                        "node_count " + std::to_string(node_count) + " does not match " +
                            std::to_string(rows.size()) + " weight rows");
        }
        return true;
    }
};

}  // namespace detail

[[nodiscard]] inline bool parse_weights(std::string_view text, WeightsTable& out,
                                        GraphError& error) {
    detail::WeightsReader reader(text, error);
    WeightsTable table;
    if (!reader.parse(table)) {
        return false;
    }
    out = std::move(table);
    error = GraphError{};
    return true;
}

[[nodiscard]] inline bool apply_weights(Graph& graph, const WeightsTable& table,
                                        GraphError& error) {
    error = GraphError{};
    if (!graph.children.empty()) {
        return detail::fail(error, GraphErrorKind::WeightRule,
                            "the table covers the root graph and this graph owns " +
                                std::to_string(graph.children.size()) + " child graphs");
    }
    if (graph.weight_kind == WeightKind::Nanoseconds) {
        return detail::fail(error, GraphErrorKind::WeightRule,
                            "the graph already carries ns weights");
    }
    if (table.ns.size() != graph.nodes.size()) {
        return detail::fail(error, GraphErrorKind::WeightRule,
                            "node_count " + std::to_string(table.ns.size()) +
                                " does not match the graph's " +
                                std::to_string(graph.nodes.size()) + " nodes");
    }
    const std::uint64_t measured = graph_fingerprint(graph);
    if (measured != table.fingerprint) {
        std::string named;
        detail::append_fingerprint(named, table.fingerprint);
        std::string actual;
        detail::append_fingerprint(actual, measured);
        return detail::fail(error, GraphErrorKind::WeightRule,
                            "the table names graph " + named + " and this graph is " + actual);
    }

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        graph.nodes[i].weight = table.ns[i];
    }
    graph.weight_kind = WeightKind::Nanoseconds;
    return true;
}

}  // namespace graphlint
