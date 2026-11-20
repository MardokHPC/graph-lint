#pragma once

#include <graphlint/config.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace graphlint {

enum class NodeType { Kernel, Memcpy, Memset, Host, ChildGraph, Empty,
                      EventRecord, EventWait, MemAlloc, MemFree, ExtSemaphore, Unknown };
enum class SourceKind { Capture, Built, File };
enum class WeightKind { Levels, Nanoseconds };
enum class GraphErrorKind { None, SchemaVersion, Syntax, Trailing, UnknownKey, DuplicateKey,
                            UnsortedKey, WrongType, BadNumber, UnknownType, MissingKey,
                            FieldForType, IdNotDense, EdgeBackwards, EdgeOutOfRange,
                            ChildMismatch, WeightRule, TooDeep, Cycle };

// The document's nesting bound: the root is level 1, so seven child graphs fit.
inline constexpr int k_max_graph_depth = 8;

struct Node {
    int id = -1;
    NodeType type = NodeType::Unknown;
    std::string name;
    std::uint64_t weight = 1;
    std::array<std::uint32_t, 3> grid = {0, 0, 0};
    std::array<std::uint32_t, 3> block = {0, 0, 0};
    std::uint64_t shared_bytes = 0;
    std::uint64_t alloc_bytes = 0;
    std::uint64_t mem_ptr = 0;
    int child = -1;
};

struct Graph {
    std::vector<Node> nodes;                 // id == index in a canonical graph
    std::vector<std::pair<int, int>> edges;  // from < to, sorted by canonicalize
    // children[k] is owned by the k-th child_graph node in id order.
    std::vector<Graph> children;
    SourceKind source = SourceKind::File;
    WeightKind weight_kind = WeightKind::Levels;
};

struct GraphError {
    GraphErrorKind kind = GraphErrorKind::None;
    int line = 0;
    int column = 0;
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return kind != GraphErrorKind::None;
    }
};

[[nodiscard]] inline const char* to_string(NodeType type) {
    switch (type) {
        case NodeType::Kernel: return "kernel";
        case NodeType::Memcpy: return "memcpy";
        case NodeType::Memset: return "memset";
        case NodeType::Host: return "host";
        case NodeType::ChildGraph: return "child_graph";
        case NodeType::Empty: return "empty";
        case NodeType::EventRecord: return "event_record";
        case NodeType::EventWait: return "event_wait";
        case NodeType::MemAlloc: return "mem_alloc";
        case NodeType::MemFree: return "mem_free";
        case NodeType::ExtSemaphore: return "ext_semaphore";
        case NodeType::Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] inline bool node_type_from_string(std::string_view token, NodeType& type) {
    if (token == "kernel") { type = NodeType::Kernel; return true; }
    if (token == "memcpy") { type = NodeType::Memcpy; return true; }
    if (token == "memset") { type = NodeType::Memset; return true; }
    if (token == "host") { type = NodeType::Host; return true; }
    if (token == "child_graph") { type = NodeType::ChildGraph; return true; }
    if (token == "empty") { type = NodeType::Empty; return true; }
    if (token == "event_record") { type = NodeType::EventRecord; return true; }
    if (token == "event_wait") { type = NodeType::EventWait; return true; }
    if (token == "mem_alloc") { type = NodeType::MemAlloc; return true; }
    if (token == "mem_free") { type = NodeType::MemFree; return true; }
    if (token == "ext_semaphore") { type = NodeType::ExtSemaphore; return true; }
    if (token == "unknown") { type = NodeType::Unknown; return true; }
    return false;
}

[[nodiscard]] inline bool operator==(const Node& a, const Node& b) {
    return a.id == b.id && a.type == b.type && a.name == b.name && a.weight == b.weight &&
           a.grid == b.grid && a.block == b.block && a.shared_bytes == b.shared_bytes &&
           a.alloc_bytes == b.alloc_bytes && a.mem_ptr == b.mem_ptr && a.child == b.child;
}

[[nodiscard]] inline bool operator==(const Graph& a, const Graph& b) {
    return a.nodes == b.nodes && a.edges == b.edges && a.children == b.children &&
           a.source == b.source && a.weight_kind == b.weight_kind;
}

namespace detail {

[[nodiscard]] inline bool fail(GraphError& error, GraphErrorKind kind, std::string detail) {
    error.kind = kind;
    error.line = 0;
    error.column = 0;
    error.detail = std::move(detail);
    return false;
}

// The numeric value of NodeType is not the artifact's type order: reordering the
// enum must not rewrite existing documents.
[[nodiscard]] inline int type_rank(NodeType type) {
    switch (type) {
        case NodeType::Kernel: return 0;
        case NodeType::Memcpy: return 1;
        case NodeType::Memset: return 2;
        case NodeType::Host: return 3;
        case NodeType::ChildGraph: return 4;
        case NodeType::Empty: return 5;
        case NodeType::EventRecord: return 6;
        case NodeType::EventWait: return 7;
        case NodeType::MemAlloc: return 8;
        case NodeType::MemFree: return 9;
        case NodeType::ExtSemaphore: return 10;
        case NodeType::Unknown: return 11;
    }
    return 11;
}

struct StructuralKey {
    int type_rank = 0;
    std::string name;
    std::array<std::uint32_t, 3> grid = {0, 0, 0};
    std::array<std::uint32_t, 3> block = {0, 0, 0};
    std::uint64_t shared_bytes = 0;
    std::uint64_t alloc_bytes = 0;
    std::uint64_t mem_ptr = 0;
    std::uint32_t in_degree = 0;
    std::uint32_t out_degree = 0;
};

[[nodiscard]] inline bool key1_less(const StructuralKey& a, const StructuralKey& b) {
    return std::tie(a.type_rank, a.name, a.grid, a.block, a.shared_bytes, a.alloc_bytes,
                    a.mem_ptr, a.in_degree, a.out_degree) <
           std::tie(b.type_rank, b.name, b.grid, b.block, b.shared_bytes, b.alloc_bytes,
                    b.mem_ptr, b.in_degree, b.out_degree);
}

// FNV-1a by hand: std::hash is implementation-defined and the bytes must not
// depend on the compiler. A 64-bit collision only creates a tie, never a wrong id.
inline void fnv1a_u64(std::uint64_t& hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= 1099511628211ull;
    }
}

inline void fnv1a_bytes(std::uint64_t& hash, const char* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i]));
        hash *= 1099511628211ull;
    }
}

[[nodiscard]] inline std::uint64_t fnv1a_key1(const StructuralKey& key) {
    std::uint64_t hash = 14695981039346656037ull;
    fnv1a_u64(hash, static_cast<std::uint64_t>(key.type_rank));
    fnv1a_u64(hash, static_cast<std::uint64_t>(key.name.size()));
    fnv1a_bytes(hash, key.name.data(), key.name.size());
    for (const std::uint32_t dim : key.grid) {
        fnv1a_u64(hash, dim);
    }
    for (const std::uint32_t dim : key.block) {
        fnv1a_u64(hash, dim);
    }
    fnv1a_u64(hash, key.shared_bytes);
    fnv1a_u64(hash, key.alloc_bytes);
    fnv1a_u64(hash, key.mem_ptr);
    fnv1a_u64(hash, key.in_degree);
    fnv1a_u64(hash, key.out_degree);
    return hash;
}

[[nodiscard]] inline std::uint64_t fnv1a_neighbors(std::vector<std::uint64_t> keys) {
    std::sort(keys.begin(), keys.end());
    std::uint64_t hash = 14695981039346656037ull;
    fnv1a_u64(hash, static_cast<std::uint64_t>(keys.size()));
    for (const std::uint64_t key : keys) {
        fnv1a_u64(hash, key);
    }
    return hash;
}

struct SortKey {
    StructuralKey key1;
    std::uint64_t pred_hash = 0;
    std::uint64_t succ_hash = 0;
    std::size_t api_index = 0;
};

struct SortKeyGreater {
    [[nodiscard]] bool operator()(const SortKey& a, const SortKey& b) const {
        if (key1_less(a.key1, b.key1)) {
            return false;
        }
        if (key1_less(b.key1, a.key1)) {
            return true;
        }
        if (a.pred_hash != b.pred_hash) {
            return a.pred_hash > b.pred_hash;
        }
        if (a.succ_hash != b.succ_hash) {
            return a.succ_hash > b.succ_hash;
        }
        return a.api_index > b.api_index;
    }
};

}  // namespace detail

// Graph-level invariants only: no line/column, because nothing here is a document.
[[nodiscard]] inline bool validate(const Graph& graph, GraphError& error) {
    error = GraphError{};
    const std::size_t n = graph.nodes.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (graph.nodes[i].id != static_cast<int>(i)) {
            return detail::fail(error, GraphErrorKind::IdNotDense,
                                "node at position " + std::to_string(i) + " has id " +
                                    std::to_string(graph.nodes[i].id));
        }
    }

    // Range first: the cycle pass indexes by endpoint, and a hand-built graph may
    // hold an edge that points backwards or outside the node list.
    for (const std::pair<int, int>& edge : graph.edges) {
        if (edge.first < 0 || edge.second < 0 ||
            static_cast<std::size_t>(edge.first) >= n ||
            static_cast<std::size_t>(edge.second) >= n) {
            return detail::fail(error, GraphErrorKind::EdgeOutOfRange,
                                "edge [" + std::to_string(edge.first) + ", " +
                                    std::to_string(edge.second) + "] is outside the node list");
        }
    }

    std::vector<std::uint32_t> remaining_in(n, 0);
    std::vector<std::vector<std::size_t>> successors(n);
    for (const std::pair<int, int>& edge : graph.edges) {
        const std::size_t from = static_cast<std::size_t>(edge.first);
        const std::size_t to = static_cast<std::size_t>(edge.second);
        ++remaining_in[to];
        successors[from].push_back(to);
    }
    std::vector<std::size_t> ready;
    ready.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (remaining_in[i] == 0) {
            ready.push_back(i);
        }
    }
    std::size_t reached = 0;
    while (!ready.empty()) {
        const std::size_t from = ready.back();
        ready.pop_back();
        ++reached;
        for (const std::size_t to : successors[from]) {
            if (--remaining_in[to] == 0) {
                ready.push_back(to);
            }
        }
    }
    if (reached != n) {
        std::size_t first = 0;
        while (first < n && remaining_in[first] == 0) {
            ++first;
        }
        return detail::fail(error, GraphErrorKind::Cycle,
                            "cycle: " + std::to_string(n - reached) + " of " +
                                std::to_string(n) + " nodes never became ready (first id " +
                                std::to_string(first) + ")");
    }

    for (const std::pair<int, int>& edge : graph.edges) {
        if (edge.first >= edge.second) {
            return detail::fail(error, GraphErrorKind::EdgeBackwards,
                                "edge [" + std::to_string(edge.first) + ", " +
                                    std::to_string(edge.second) + "] is not from < to");
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const Node& node = graph.nodes[i];
        if (node.type == NodeType::Kernel) {
            for (std::size_t d = 0; d < 3; ++d) {
                if (node.grid[d] == 0 || node.block[d] == 0) {
                    return detail::fail(error, GraphErrorKind::FieldForType,
                                        "kernel node " + std::to_string(i) +
                                            " has a zero launch dimension");
                }
            }
        } else {
            const bool zero_geometry = node.grid == std::array<std::uint32_t, 3>{0, 0, 0} &&
                                       node.block == std::array<std::uint32_t, 3>{0, 0, 0} &&
                                       node.shared_bytes == 0;
            if (!zero_geometry) {
                return detail::fail(error, GraphErrorKind::FieldForType,
                                    "node " + std::to_string(i) + " carries kernel geometry");
            }
        }
        if (node.alloc_bytes != 0 && node.type != NodeType::MemAlloc) {
            return detail::fail(error, GraphErrorKind::FieldForType,
                                "node " + std::to_string(i) + " carries alloc_bytes");
        }
        if (node.mem_ptr != 0 && node.type != NodeType::MemAlloc &&
            node.type != NodeType::MemFree) {
            return detail::fail(error, GraphErrorKind::FieldForType,
                                "node " + std::to_string(i) + " carries mem_ptr");
        }
        if (node.type == NodeType::ChildGraph) {
            if (node.child < 0 || static_cast<std::size_t>(node.child) >= graph.children.size()) {
                return detail::fail(error, GraphErrorKind::ChildMismatch,
                                    "node " + std::to_string(i) + " points at child " +
                                        std::to_string(node.child));
            }
        } else if (node.child != -1) {
            return detail::fail(error, GraphErrorKind::FieldForType,
                                "node " + std::to_string(i) + " carries child");
        }
        if (graph.weight_kind == WeightKind::Levels && node.weight != 1) {
            return detail::fail(error, GraphErrorKind::WeightRule,
                                "node " + std::to_string(i) + " has a weight in a levels graph");
        }
    }

    std::size_t expected_child = 0;
    for (const Node& node : graph.nodes) {
        if (node.type != NodeType::ChildGraph) {
            continue;
        }
        if (static_cast<std::size_t>(node.child) != expected_child) {
            return detail::fail(error, GraphErrorKind::ChildMismatch,
                                "child_graph nodes do not own children in id order");
        }
        ++expected_child;
    }
    if (expected_child != graph.children.size()) {
        return detail::fail(error, GraphErrorKind::ChildMismatch,
                            "child " + std::to_string(expected_child) +
                                " is not referenced by any node");
    }

    for (const Graph& child : graph.children) {
        if (!validate(child, error)) {
            return false;
        }
    }
    return true;
}

// Kahn over a min-heap of (structural key, api index). The ready set is structural,
// so the heap only reads api_index when two nodes are structurally identical (F2).
inline void canonicalize(Graph& graph) {
    const std::size_t n = graph.nodes.size();
    std::vector<std::uint32_t> in_degree(n, 0);
    std::vector<std::uint32_t> out_degree(n, 0);
    std::vector<std::vector<std::size_t>> predecessors(n);
    std::vector<std::vector<std::size_t>> successors(n);
    for (const std::pair<int, int>& edge : graph.edges) {
        GRAPHLINT_ASSERT(edge.first >= 0 && edge.second >= 0);
        const std::size_t from = static_cast<std::size_t>(edge.first);
        const std::size_t to = static_cast<std::size_t>(edge.second);
        GRAPHLINT_ASSERT(from < n && to < n);
        ++out_degree[from];
        ++in_degree[to];
        predecessors[to].push_back(from);
        successors[from].push_back(to);
    }

    std::vector<detail::StructuralKey> keys(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& node = graph.nodes[i];
        detail::StructuralKey& key = keys[i];
        key.type_rank = detail::type_rank(node.type);
        key.name = node.name;
        key.grid = node.grid;
        key.block = node.block;
        key.shared_bytes = node.shared_bytes;
        key.alloc_bytes = node.alloc_bytes;
        key.mem_ptr = node.mem_ptr;
        key.in_degree = in_degree[i];
        key.out_degree = out_degree[i];
    }

    std::vector<std::uint64_t> key1_hash(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        key1_hash[i] = detail::fnv1a_key1(keys[i]);
    }

    std::vector<std::uint64_t> pred_hash(n, 0);
    std::vector<std::uint64_t> succ_hash(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::uint64_t> preds;
        preds.reserve(predecessors[i].size());
        for (const std::size_t p : predecessors[i]) {
            preds.push_back(key1_hash[p]);
        }
        pred_hash[i] = detail::fnv1a_neighbors(std::move(preds));

        std::vector<std::uint64_t> succs;
        succs.reserve(successors[i].size());
        for (const std::size_t s : successors[i]) {
            succs.push_back(key1_hash[s]);
        }
        succ_hash[i] = detail::fnv1a_neighbors(std::move(succs));
    }

    std::priority_queue<detail::SortKey, std::vector<detail::SortKey>, detail::SortKeyGreater>
        ready;
    std::vector<std::uint32_t> remaining = in_degree;
    for (std::size_t i = 0; i < n; ++i) {
        if (remaining[i] == 0) {
            ready.push(detail::SortKey{std::move(keys[i]), pred_hash[i], succ_hash[i], i});
        }
    }

    std::vector<std::size_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        const detail::SortKey top = ready.top();
        ready.pop();
        order.push_back(top.api_index);
        for (const std::size_t next : successors[top.api_index]) {
            if (--remaining[next] == 0) {
                ready.push(detail::SortKey{std::move(keys[next]), pred_hash[next],
                                           succ_hash[next], next});
            }
        }
    }

    // A cycle means the handle map lost an edge; parse cannot produce one (§3).
    GRAPHLINT_ASSERT(order.size() == n);
    if (order.size() != n) {
        return;
    }

    std::vector<int> new_id(n, -1);
    for (std::size_t k = 0; k < order.size(); ++k) {
        new_id[order[k]] = static_cast<int>(k);
    }

    std::vector<Node> new_nodes;
    new_nodes.reserve(n);
    for (const std::size_t old : order) {
        Node node = std::move(graph.nodes[old]);
        node.id = static_cast<int>(new_nodes.size());
        new_nodes.push_back(std::move(node));
    }

    std::vector<std::pair<int, int>> new_edges;
    new_edges.reserve(graph.edges.size());
    for (const std::pair<int, int>& edge : graph.edges) {
        new_edges.emplace_back(new_id[static_cast<std::size_t>(edge.first)],
                               new_id[static_cast<std::size_t>(edge.second)]);
    }
    std::sort(new_edges.begin(), new_edges.end());

    for (Graph& child : graph.children) {
        canonicalize(child);
    }

    std::vector<std::size_t> owners;
    for (std::size_t i = 0; i < new_nodes.size(); ++i) {
        if (new_nodes[i].type == NodeType::ChildGraph) {
            owners.push_back(i);
        }
    }
    GRAPHLINT_ASSERT(owners.size() == graph.children.size());
    std::vector<bool> taken(graph.children.size(), false);
    bool permutable = owners.size() == graph.children.size();
    for (const std::size_t owner : owners) {
        const int child = new_nodes[owner].child;
        if (child < 0 || static_cast<std::size_t>(child) >= graph.children.size() ||
            taken[static_cast<std::size_t>(child)]) {
            permutable = false;
            break;
        }
        taken[static_cast<std::size_t>(child)] = true;
    }
    if (permutable) {
        std::vector<Graph> reordered;
        reordered.reserve(graph.children.size());
        for (const std::size_t owner : owners) {
            const std::size_t child = static_cast<std::size_t>(new_nodes[owner].child);
            reordered.push_back(std::move(graph.children[child]));
            new_nodes[owner].child = static_cast<int>(reordered.size() - 1);
        }
        graph.children = std::move(reordered);
    }

    graph.nodes = std::move(new_nodes);
    graph.edges = std::move(new_edges);
}

}  // namespace graphlint
