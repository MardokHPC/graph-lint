#pragma once

#include <graphlint/config.hpp>
#include <graphlint/graph.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace graphlint {

// Hop counts, not durations: weights are ignored here (§5.3), so a graph in ns gets the
// same levels as the same graph in levels.
struct Topology {
    std::vector<int> level;                           // indexed by node.id, 0 at a source
    int depth = 0;                                    // number of levels; 0 for no nodes
    std::vector<int> width;                           // nodes per level, width[level]
    double parallelism = 0.0;                         // nodes / depth; 0 for no nodes
    std::vector<std::pair<int, int>> edges_minimal;    // transitive reduction, sorted
    std::vector<std::pair<int, int>> edges_redundant;  // implied by a longer path, sorted
    int distinct_edges = 0;                            // graph.edges after deduplication
    bool acyclic = true;                               // false only when the call failed
};

// parse_graph's contract: false resets `out` with acyclic = false and fills `error`, with
// validate's preconditions (dense ids, endpoints in range, from < to). Nothing aborts or throws.
[[nodiscard]] inline bool topology(const Graph& graph, Topology& out, GraphError& error) {
    error = GraphError{};
    out = Topology{};
    out.acyclic = false;

    const std::size_t n = graph.nodes.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (graph.nodes[i].id != static_cast<int>(i)) {
            return detail::fail(error, GraphErrorKind::IdNotDense,
                                "node at position " + std::to_string(i) + " has id " +
                                    std::to_string(graph.nodes[i].id));
        }
    }
    for (const std::pair<int, int>& edge : graph.edges) {
        if (edge.first < 0 || edge.second < 0 ||
            static_cast<std::size_t>(edge.first) >= n ||
            static_cast<std::size_t>(edge.second) >= n) {
            return detail::fail(error, GraphErrorKind::EdgeOutOfRange,
                                "edge [" + std::to_string(edge.first) + ", " +
                                    std::to_string(edge.second) + "] is outside the node list");
        }
    }
    // Same precondition as validate, checked before Kahn: `edges_minimal` is consumed downstream
    // as canonical (M5 compresses chains on it), so a backwards pair must not reach the analysis.
    for (const std::pair<int, int>& edge : graph.edges) {
        if (edge.first >= edge.second) {
            return detail::fail(error, GraphErrorKind::EdgeBackwards,
                                "edge [" + std::to_string(edge.first) + ", " +
                                    std::to_string(edge.second) + "] is not from < to");
        }
    }

    // The parser keeps duplicate edges on purpose (dag-check reports them); the analysis
    // asks about the relation, so one copy per pair is the right input.
    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    std::vector<std::vector<int>> successors(n);
    std::vector<int> in_degree(n, 0);
    for (const std::pair<int, int>& edge : edges) {
        successors[static_cast<std::size_t>(edge.first)].push_back(edge.second);
        ++in_degree[static_cast<std::size_t>(edge.second)];
    }

    std::vector<int> level(n, 0);
    std::vector<int> remaining = in_degree;
    std::vector<std::size_t> ready;
    ready.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (remaining[i] == 0) {
            ready.push_back(i);
        }
    }
    std::size_t reached = 0;
    while (!ready.empty()) {
        const std::size_t from = ready.back();
        ready.pop_back();
        ++reached;
        for (const int to : successors[from]) {
            const std::size_t target = static_cast<std::size_t>(to);
            level[target] = std::max(level[target], level[from] + 1);
            if (--remaining[target] == 0) {
                ready.push_back(target);
            }
        }
    }
    if (reached != n) {
        std::size_t first = 0;
        while (first < n && remaining[first] == 0) {
            ++first;
        }
        // from < to above already orders the ids topologically, so no input can get here: the
        // branch stays because it is what forbids returning a partial `level` as a success.
        return detail::fail(error, GraphErrorKind::Cycle,
                            "cycle: " + std::to_string(n - reached) + " of " + std::to_string(n) +
                                " nodes never became ready (first id " + std::to_string(first) +
                                ")");
    }

    int depth = 0;
    for (const int value : level) {
        depth = std::max(depth, value + 1);
    }
    std::vector<int> width(static_cast<std::size_t>(depth), 0);
    for (const int value : level) {
        ++width[static_cast<std::size_t>(value)];
    }

    std::vector<std::pair<int, int>> minimal;
    std::vector<std::pair<int, int>> redundant;
    minimal.reserve(edges.size());
    std::vector<std::size_t> stamp(n, 0);
    std::vector<std::size_t> stack;
    std::size_t generation = 0;

    for (const std::pair<int, int>& edge : edges) {
        const int from = edge.first;
        const int to = edge.second;
        // An alternative path ends on a second predecessor of `to`: with a single in-edge
        // no other route can reach it, so the answer is settled without touching the graph.
        if (in_degree[static_cast<std::size_t>(to)] < 2) {
            minimal.push_back(edge);
            continue;
        }
        // Levels rise along every edge, so nothing at or past `to`'s level can sit on an
        // alternative route: the search stays below that ceiling instead of walking on.
        ++generation;
        const int ceiling = level[static_cast<std::size_t>(to)];
        bool implied = false;
        stack.clear();
        for (const int next : successors[static_cast<std::size_t>(from)]) {
            if (next == to) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(next);
            if (level[index] >= ceiling || stamp[index] == generation) {
                continue;
            }
            stamp[index] = generation;
            stack.push_back(index);
        }
        while (!stack.empty() && !implied) {
            const std::size_t at = stack.back();
            stack.pop_back();
            for (const int next : successors[at]) {
                if (next == to) {
                    implied = true;
                    break;
                }
                const std::size_t index = static_cast<std::size_t>(next);
                if (level[index] >= ceiling || stamp[index] == generation) {
                    continue;
                }
                stamp[index] = generation;
                stack.push_back(index);
            }
        }
        if (implied) {
            redundant.push_back(edge);
        } else {
            minimal.push_back(edge);
        }
    }

    out.level = std::move(level);
    out.depth = depth;
    out.width = std::move(width);
    out.parallelism = depth == 0 ? 0.0 : static_cast<double>(n) / static_cast<double>(depth);
    out.edges_minimal = std::move(minimal);
    out.edges_redundant = std::move(redundant);
    out.distinct_edges = static_cast<int>(edges.size());
    out.acyclic = true;
    return true;
}

// children[k] mirrors Graph::children[k]: the two trees are walked with one index, so a
// caller never has to re-derive which Topology belongs to which nested graph.
struct TopologyTree {
    Topology self;
    std::vector<TopologyTree> children;
};

// A nested graph is analysed on its own: a child that fails does not erase its siblings,
// and the error reported is the first one met in pre-order.
[[nodiscard]] inline bool topology_tree(const Graph& graph, TopologyTree& out, GraphError& error) {
    error = GraphError{};
    TopologyTree tree;
    bool ok = topology(graph, tree.self, error);
    tree.children.resize(graph.children.size());
    for (std::size_t k = 0; k < graph.children.size(); ++k) {
        GraphError child_error;
        if (!topology_tree(graph.children[k], tree.children[k], child_error)) {
            if (ok) {
                error = child_error;
            }
            ok = false;
        }
    }
    out = std::move(tree);
    return ok;
}

}  // namespace graphlint
