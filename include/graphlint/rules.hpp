#pragma once

#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace graphlint {

enum class Severity { error, severe, warn, info };

[[nodiscard]] inline const char* to_string(Severity severity) {
    switch (severity) {
        case Severity::error: return "error";
        case Severity::severe: return "severe";
        case Severity::warn: return "warn";
        case Severity::info: return "info";
    }
    return "info";
}

// The rank is the finding order; in M6 it also picks the exit code: error 2, severe 1,
// warn and info 0 unless a gate names the rule.
[[nodiscard]] inline int severity_rank(Severity severity) {
    switch (severity) {
        case Severity::error: return 0;
        case Severity::severe: return 1;
        case Severity::warn: return 2;
        case Severity::info: return 3;
    }
    return 3;
}

// Declaration order is the table's order in docs/rules.md: `rules` is filled in it, and
// operator< breaks a tie on it, so a new rule goes in its document position, never at the end.
enum class RuleId { DagCheck, LaunchShaped, RedundantEdge, SyncNode, PointlessFork,
                    DuplicateLaunch, MemLifecycle };

inline constexpr std::size_t k_rule_count = 7;

[[nodiscard]] inline const char* to_string(RuleId rule) {
    switch (rule) {
        case RuleId::DagCheck: return "dag-check";
        case RuleId::LaunchShaped: return "launch-shaped";
        case RuleId::RedundantEdge: return "redundant-edge";
        case RuleId::SyncNode: return "sync-node";
        case RuleId::PointlessFork: return "pointless-fork";
        case RuleId::DuplicateLaunch: return "duplicate-launch";
        case RuleId::MemLifecycle: return "mem-lifecycle";
    }
    return "dag-check";
}

struct Finding {
    RuleId rule = RuleId::DagCheck;
    Severity severity = Severity::info;
    std::vector<int> nodes;  // sorted graph ids; empty for a graph-level finding
    std::string message;     // one sentence, carries the numbers, never a duration or a path
};

// The report's total order: rank, then nodes element by element with the shorter list first,
// then rule, then message. It reads the fields only, so two runs cannot order two findings
// differently and the emitted bytes are stable.
[[nodiscard]] inline bool operator<(const Finding& a, const Finding& b) {
    if (severity_rank(a.severity) != severity_rank(b.severity)) {
        return severity_rank(a.severity) < severity_rank(b.severity);
    }
    if (a.nodes != b.nodes) {
        return std::lexicographical_compare(a.nodes.begin(), a.nodes.end(), b.nodes.begin(),
                                            b.nodes.end());
    }
    if (a.rule != b.rule) {
        return a.rule < b.rule;
    }
    return a.message < b.message;
}

[[nodiscard]] inline bool operator==(const Finding& a, const Finding& b) {
    return a.rule == b.rule && a.severity == b.severity && a.nodes == b.nodes &&
           a.message == b.message;
}

[[nodiscard]] inline bool operator!=(const Finding& a, const Finding& b) { return !(a == b); }

// The reason is schema text: it lands in the report and is part of the byte-identical contract,
// so it is never composed from runtime values.
struct RuleStatus {
    RuleId rule = RuleId::DagCheck;
    bool applicable = false;
    std::string reason;
};

struct LintResult {
    std::vector<Finding> findings;  // sorted by operator<
    std::vector<RuleStatus> rules;  // always seven, in RuleId order
};

namespace detail {

[[nodiscard]] inline std::size_t rule_index(RuleId rule) {
    return static_cast<std::size_t>(rule);
}

inline constexpr const char* k_reason_dag_ok = "graph is a DAG";
inline constexpr const char* k_reason_dag_refused = "topology refused the graph";
inline constexpr const char* k_reason_graph_failed = "graph failed dag-check";
inline constexpr const char* k_reason_launch_ok = "depth and node count are known";
inline constexpr const char* k_reason_redundant_ok = "the reduction is exact on a DAG";
inline constexpr const char* k_reason_sync_ok = "node types are readable";
inline constexpr const char* k_reason_fork_ok = "successors and reachability are readable";
inline constexpr const char* k_reason_duplicate_ok = "at least two named kernels";
inline constexpr const char* k_reason_no_kernel_name = "no kernel name";
inline constexpr const char* k_reason_mem_ok = "mem_alloc/mem_free nodes are present";
inline constexpr const char* k_reason_no_mem = "no mem_alloc/mem_free node";

// Message tokens for GraphErrorKind, not a second name for the enum: they appear in the one
// finding a refused graph produces, so their spelling is schema text too.
[[nodiscard]] inline const char* error_kind_token(GraphErrorKind kind) {
    switch (kind) {
        case GraphErrorKind::None: return "none";
        case GraphErrorKind::SchemaVersion: return "schema_version";
        case GraphErrorKind::Syntax: return "syntax";
        case GraphErrorKind::Trailing: return "trailing";
        case GraphErrorKind::UnknownKey: return "unknown_key";
        case GraphErrorKind::DuplicateKey: return "duplicate_key";
        case GraphErrorKind::UnsortedKey: return "unsorted_key";
        case GraphErrorKind::WrongType: return "wrong_type";
        case GraphErrorKind::BadNumber: return "bad_number";
        case GraphErrorKind::UnknownType: return "unknown_type";
        case GraphErrorKind::MissingKey: return "missing_key";
        case GraphErrorKind::FieldForType: return "field_for_type";
        case GraphErrorKind::IdNotDense: return "id_not_dense";
        case GraphErrorKind::EdgeBackwards: return "edge_backwards";
        case GraphErrorKind::EdgeOutOfRange: return "edge_out_of_range";
        case GraphErrorKind::ChildMismatch: return "child_mismatch";
        case GraphErrorKind::WeightRule: return "weight_rule";
        case GraphErrorKind::TooDeep: return "too_deep";
        case GraphErrorKind::Cycle: return "cycle";
    }
    return "none";
}

// 64 bit on purpose: at 1e8 nodes 10 * nodes does not fit in an int, and a float would round
// the boundary. Never a duration: depth is a level count (§5.3).
[[nodiscard]] inline bool launch_shaped(int depth, std::size_t node_count) {
    const std::int64_t nodes = static_cast<std::int64_t>(node_count);
    const std::int64_t levels = depth;
    return nodes >= 16 && 10 * levels >= 9 * nodes;
}

// Endpoints of a hand-built graph may sit outside the node list even when a caller swears the
// topology succeeded; skipping them keeps the rules from indexing past the vector.
[[nodiscard]] inline bool endpoint_in_range(int from, int to, std::size_t size) {
    return from >= 0 && to >= 0 && static_cast<std::size_t>(from) < size &&
           static_cast<std::size_t>(to) < size;
}

inline void rule_dag_check(const Graph& graph, LintResult& out) {
    out.rules[rule_index(RuleId::DagCheck)] = {RuleId::DagCheck, true, k_reason_dag_ok};

    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t end = i + 1;
        while (end < edges.size() && edges[end] == edges[i]) {
            ++end;
        }
        if (end - i >= 2) {
            const int low = std::min(edges[i].first, edges[i].second);
            const int high = std::max(edges[i].first, edges[i].second);
            Finding finding;
            finding.rule = RuleId::DagCheck;
            finding.severity = Severity::warn;
            finding.nodes = {low, high};
            finding.message = "edge [" + std::to_string(low) + ", " + std::to_string(high) +
                              "] appears " + std::to_string(end - i) +
                              " times; the analysis counts it once";
            out.findings.push_back(std::move(finding));
        }
        i = end;
    }

    // An isolated node is legal and runs concurrently, but a forgotten dependency looks exactly
    // the same, hence the warn. With no edge at all nothing can be isolated from anything.
    if (graph.edges.empty()) {
        return;
    }
    const std::size_t n = graph.nodes.size();
    std::vector<char> touched(n, 0);
    for (const std::pair<int, int>& edge : graph.edges) {
        if (!endpoint_in_range(edge.first, edge.second, n)) {
            continue;
        }
        touched[static_cast<std::size_t>(edge.first)] = 1;
        touched[static_cast<std::size_t>(edge.second)] = 1;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (touched[i] != 0) {
            continue;
        }
        Finding finding;
        finding.rule = RuleId::DagCheck;
        finding.severity = Severity::warn;
        finding.nodes = {graph.nodes[i].id};
        finding.message = "node " + std::to_string(graph.nodes[i].id) +
                          " has no in- and no out-edge: it runs concurrently, and a forgotten "
                          "dependency looks the same";
        out.findings.push_back(std::move(finding));
    }
}

inline void rule_launch_shaped(const Graph& graph, const Topology& topo, LintResult& out) {
    out.rules[rule_index(RuleId::LaunchShaped)] = {RuleId::LaunchShaped, true,
                                                   k_reason_launch_ok};
    if (!launch_shaped(topo.depth, graph.nodes.size())) {
        return;
    }
    Finding finding;
    finding.rule = RuleId::LaunchShaped;
    finding.severity = Severity::warn;
    finding.message = std::to_string(topo.depth) + " levels over " +
                      std::to_string(graph.nodes.size()) + " nodes: no two nodes can overlap";
    out.findings.push_back(std::move(finding));
}

inline void rule_redundant_edge(const Topology& topo, LintResult& out) {
    out.rules[rule_index(RuleId::RedundantEdge)] = {RuleId::RedundantEdge, true,
                                                    k_reason_redundant_ok};
    if (topo.edges_redundant.empty()) {
        return;
    }
    std::vector<int> nodes;
    nodes.reserve(topo.edges_redundant.size() * 2);
    for (const std::pair<int, int>& edge : topo.edges_redundant) {
        nodes.push_back(edge.first);
        nodes.push_back(edge.second);
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

    Finding finding;
    finding.rule = RuleId::RedundantEdge;
    finding.severity = Severity::warn;
    finding.nodes = std::move(nodes);
    // The verb follows the count: "1 of 29 edges is implied" is a sentence somebody reads.
    finding.message = std::to_string(topo.edges_redundant.size()) + " of " +
                      std::to_string(topo.distinct_edges) + " edges " +
                      (topo.edges_redundant.size() == 1 ? "is" : "are") +
                      " implied by a longer path; the minimal DAG has " +
                      std::to_string(topo.edges_minimal.size()) + " edges";
    out.findings.push_back(std::move(finding));
}

inline void rule_sync_node(const Graph& graph, LintResult& out) {
    out.rules[rule_index(RuleId::SyncNode)] = {RuleId::SyncNode, true, k_reason_sync_ok};
    std::size_t events = 0;
    for (const Node& node : graph.nodes) {
        if (node.type == NodeType::Host) {
            out.findings.push_back({RuleId::SyncNode, Severity::severe, {node.id},
                                    "host node " + std::to_string(node.id) +
                                        " runs a CPU callback inside the graph and serialises "
                                        "its stream"});
        } else if (node.type == NodeType::ExtSemaphore) {
            out.findings.push_back({RuleId::SyncNode, Severity::severe, {node.id},
                                    "ext_semaphore node " + std::to_string(node.id) +
                                        " waits on a producer the graph does not contain"});
        } else if (node.type == NodeType::EventRecord || node.type == NodeType::EventWait) {
            ++events;
        }
    }
    // One aggregated finding at info: an event is how a fork is expressed, and ten events are
    // not ten defects. No warn tier and no count threshold; the wording follows the count
    // because the report is read.
    if (events != 0) {
        const std::string count = std::to_string(events);
        const std::string subject = events == 1 ? count + " event node records or waits"
                                                : count + " event nodes record or wait";
        out.findings.push_back({RuleId::SyncNode, Severity::info, {},
                                subject + " on events: this is how a fork is expressed"});
    }
}

inline void rule_pointless_fork(const Graph& graph, const Topology& topo, LintResult& out) {
    out.rules[rule_index(RuleId::PointlessFork)] = {RuleId::PointlessFork, true,
                                                    k_reason_fork_ok};
    const std::size_t n = graph.nodes.size();
    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::vector<int>> successors(n);
    for (const std::pair<int, int>& edge : edges) {
        if (endpoint_in_range(edge.first, edge.second, n)) {
            successors[static_cast<std::size_t>(edge.first)].push_back(edge.second);
        }
    }

    // A branch t->j holds fewer than two nodes exactly when t->j is in the reduction. Both edges
    // must be minimal: an implied u->t is a route through another node, so `t` is not a branch of
    // `u`. The reduction is the caller's, so the rule reads it instead of walking the graph again.
    const auto is_minimal = [&topo](int from, int to) {
        return std::binary_search(topo.edges_minimal.begin(), topo.edges_minimal.end(),
                                  std::make_pair(from, to));
    };
    std::vector<int> branches(n, 0);
    std::vector<int> touched;
    for (std::size_t u = 0; u < n; ++u) {
        // A fork needs two successors. Skipping the rest is what keeps a join-then-broadcast
        // graph linear instead of one binary_search per edge.
        if (successors[u].size() < 2) {
            continue;
        }
        touched.clear();
        for (const int t : successors[u]) {
            if (!is_minimal(static_cast<int>(u), t)) {
                continue;
            }
            for (const int j : successors[static_cast<std::size_t>(t)]) {
                if (!is_minimal(t, j)) {
                    continue;
                }
                if (branches[static_cast<std::size_t>(j)] == 0) {
                    touched.push_back(j);
                }
                ++branches[static_cast<std::size_t>(j)];
            }
        }
        for (const int j : touched) {
            const int count = branches[static_cast<std::size_t>(j)];
            if (count >= 2) {
                out.findings.push_back({RuleId::PointlessFork, Severity::info,
                                        {static_cast<int>(u), j},
                                        "node " + std::to_string(u) + " splits into " +
                                            std::to_string(count) +
                                            " single-node branches that rejoin at node " +
                                            std::to_string(j) + ": the fork buys nothing"});
            }
            branches[static_cast<std::size_t>(j)] = 0;
        }
    }
}

using LaunchKey = std::tuple<std::string, std::array<std::uint32_t, 3>,
                             std::array<std::uint32_t, 3>, std::uint64_t>;

inline void rule_duplicate_launch(const Graph& graph, LintResult& out) {
    std::map<LaunchKey, std::vector<int>> groups;
    std::size_t named = 0;
    for (const Node& node : graph.nodes) {
        if (node.type != NodeType::Kernel || node.name.empty()) {
            continue;
        }
        ++named;
        groups[LaunchKey{node.name, node.grid, node.block, node.shared_bytes}].push_back(node.id);
    }
    // Geometry alone would report 27 candidates on built.json and 1000 on chain.json: an empty
    // name is a reader failure (limits.md), so the rule goes dark instead of guessing.
    if (named < 2) {
        out.rules[rule_index(RuleId::DuplicateLaunch)] = {RuleId::DuplicateLaunch, false,
                                                          k_reason_no_kernel_name};
        return;
    }
    out.rules[rule_index(RuleId::DuplicateLaunch)] = {RuleId::DuplicateLaunch, true,
                                                      k_reason_duplicate_ok};
    for (const auto& group : groups) {
        if (group.second.size() < 2) {
            continue;
        }
        out.findings.push_back({RuleId::DuplicateLaunch, Severity::info, group.second,
                                std::to_string(group.second.size()) + " kernel nodes launch \"" +
                                    std::get<0>(group.first) +
                                    "\" with the same grid, block and shared bytes: a candidate, "
                                    "not a duplicate"});
    }
}

struct MemInterval {
    std::uint64_t ptr = 0;
    std::uint64_t bytes = 0;
    int alloc_node = -1;
    int free_node = -1;  // -1: no free node, live to the end
};

inline void rule_mem_lifecycle(const Graph& graph, LintResult& out) {
    bool has_mem = false;
    std::vector<MemInterval> allocations;
    std::map<std::uint64_t, std::vector<int>> frees;
    for (const Node& node : graph.nodes) {
        if (node.type == NodeType::MemAlloc) {
            has_mem = true;
            if (node.mem_ptr != 0) {
                allocations.push_back({node.mem_ptr, node.alloc_bytes, node.id, -1});
            }
        } else if (node.type == NodeType::MemFree) {
            has_mem = true;
            if (node.mem_ptr != 0) {
                frees[node.mem_ptr].push_back(node.id);
            }
        }
    }
    if (!has_mem) {
        out.rules[rule_index(RuleId::MemLifecycle)] = {RuleId::MemLifecycle, false,
                                                       k_reason_no_mem};
        return;
    }
    out.rules[rule_index(RuleId::MemLifecycle)] = {RuleId::MemLifecycle, true, k_reason_mem_ok};

    // A zero pointer correlates with nothing (the getter can fail), so it never enters the
    // sweep. Matching is FIFO per pointer in id order: ids are a topological order, so this is
    // one declared linearisation, never the allocator's timeline.
    std::map<std::uint64_t, std::size_t> cursor;
    for (MemInterval& interval : allocations) {
        const auto found = frees.find(interval.ptr);
        if (found == frees.end()) {
            continue;
        }
        std::size_t& next = cursor[interval.ptr];
        while (next < found->second.size() && found->second[next] <= interval.alloc_node) {
            ++next;
        }
        if (next == found->second.size()) {
            continue;
        }
        interval.free_node = found->second[next];
        ++next;
    }

    const int last_node = graph.nodes.empty() ? -1 : graph.nodes.back().id;
    std::uint64_t largest = 0;
    std::size_t pairs = 0;
    std::vector<std::pair<int, std::uint64_t>> ends;
    ends.reserve(allocations.size());
    for (const MemInterval& interval : allocations) {
        largest = std::max(largest, interval.bytes);
        if (interval.free_node >= 0) {
            ++pairs;
        }
        const int end = interval.free_node >= 0 ? interval.free_node : last_node;
        ends.emplace_back(end, interval.bytes);
        if (interval.free_node < 0) {
            // One byte is a legal allocation, so the noun and the verb follow the count.
            std::string message = std::to_string(interval.bytes) +
                                  (interval.bytes == 1 ? " byte at node " : " bytes at node ") +
                                  std::to_string(interval.alloc_node);
            message += interval.bytes == 1
                           ? " is never freed: live to the end and counted in the peak"
                           : " are never freed: live to the end and counted in the peak";
            out.findings.push_back(
                {RuleId::MemLifecycle, Severity::warn, {interval.alloc_node}, std::move(message)});
        }
    }

    // The peak of the allocations the graph declares on this one linearisation, not the
    // allocator's: per-stream concurrency is invisible and a reused block is never named.
    std::sort(ends.begin(), ends.end());
    std::uint64_t live_bytes = 0;
    std::uint64_t peak = 0;
    std::size_t expired = 0;
    int live = 0;
    int peak_node = -1;
    int peak_live = 0;
    for (const MemInterval& interval : allocations) {
        while (expired < ends.size() && ends[expired].first < interval.alloc_node) {
            live_bytes -= ends[expired].second;
            --live;
            ++expired;
        }
        live_bytes += interval.bytes;
        ++live;
        if (live_bytes > peak) {
            peak = live_bytes;
            peak_node = interval.alloc_node;
            peak_live = live;
        }
    }
    if (peak > largest) {
        const std::string churn =
            std::to_string(pairs) + (pairs == 1 ? " alloc/free pair" : " alloc/free pairs");
        out.findings.push_back({RuleId::MemLifecycle, Severity::warn, {},
                                "peak " + std::to_string(peak) + " bytes at node " +
                                    std::to_string(peak_node) + ", " +
                                    std::to_string(peak_live) + " live, largest request " +
                                    std::to_string(largest) + ", " + churn});
    }
}

}  // namespace detail

// The Topology is the caller's and lint recomputes nothing of it; the GraphError a failed
// topology() returned comes in beside it, the only channel that can name a cycle. False means
// the graph was not analysable: one error finding, six not_applicable, `out` whole either way.
[[nodiscard]] inline bool lint(const Graph& graph, const Topology& topo,
                               const GraphError& topo_error, LintResult& out) {
    LintResult result;
    result.rules = {
        {RuleId::DagCheck, true, detail::k_reason_dag_refused},
        {RuleId::LaunchShaped, false, detail::k_reason_graph_failed},
        {RuleId::RedundantEdge, false, detail::k_reason_graph_failed},
        {RuleId::SyncNode, false, detail::k_reason_graph_failed},
        {RuleId::PointlessFork, false, detail::k_reason_graph_failed},
        {RuleId::DuplicateLaunch, false, detail::k_reason_graph_failed},
        {RuleId::MemLifecycle, false, detail::k_reason_graph_failed},
    };

    if (!topo.acyclic) {
        Finding finding;
        finding.rule = RuleId::DagCheck;
        finding.severity = Severity::error;
        const std::string token = detail::error_kind_token(topo_error.kind);
        // One sentence: a caller-built error may carry no detail, or a detail that already
        // opens with the token. Neither may leave a dangling separator or a stutter.
        if (topo_error.kind == GraphErrorKind::None) {
            finding.message = "graph failed dag-check: no error detail was passed";
        } else if (topo_error.detail.empty() || topo_error.detail.rfind(token, 0) == 0) {
            finding.message = topo_error.detail.empty() ? token : topo_error.detail;
        } else {
            finding.message = token + ": " + topo_error.detail;
        }
        result.findings.push_back(std::move(finding));
        out = std::move(result);
        return false;
    }

    detail::rule_dag_check(graph, result);
    detail::rule_launch_shaped(graph, topo, result);
    detail::rule_redundant_edge(topo, result);
    detail::rule_sync_node(graph, result);
    detail::rule_pointless_fork(graph, topo, result);
    detail::rule_duplicate_launch(graph, result);
    detail::rule_mem_lifecycle(graph, result);

    // Sorted here, not in the emitter: the order is a function of the fields, so no caller can
    // produce two different byte sequences for one graph.
    std::sort(result.findings.begin(), result.findings.end());
    out = std::move(result);
    return true;
}

}  // namespace graphlint
