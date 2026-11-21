#pragma once

#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace graphlint {

struct MermaidOptions {
    int max_nodes = 80;
};

namespace detail {

// The counts can agree while the pairs do not, and the drawing would then show an edge
// the graph does not have: compare the sorted pairs themselves, not their total.
[[nodiscard]] inline bool mermaid_edges_match(const Graph& g, const Topology& t) {
    std::vector<std::pair<int, int>> pairs = t.edges_minimal;
    pairs.insert(pairs.end(), t.edges_redundant.begin(), t.edges_redundant.end());
    std::sort(pairs.begin(), pairs.end());
    std::vector<std::pair<int, int>> model = g.edges;
    std::sort(model.begin(), model.end());
    model.erase(std::unique(model.begin(), model.end()), model.end());
    return pairs == model;
}

struct MermaidLink {
    int from = 0;
    int to = 0;
    bool dotted = false;
};

// dotted is part of the key so the surviving copy of a merged pair is chosen by the
// bytes, not by the sort's internals: a needed edge wins over an implied one.
[[nodiscard]] inline bool mermaid_link_less(const MermaidLink& a, const MermaidLink& b) {
    return std::tie(a.from, a.to, a.dotted) < std::tie(b.from, b.to, b.dotted);
}

[[nodiscard]] inline bool mermaid_link_same_pair(const MermaidLink& a, const MermaidLink& b) {
    return a.from == b.from && a.to == b.to;
}

[[nodiscard]] inline bool mermaid_is_sync(NodeType type) {
    return type == NodeType::EventRecord || type == NodeType::EventWait ||
           type == NodeType::Host || type == NodeType::ExtSemaphore;
}

inline void append_mermaid_node(std::string& out, int id, NodeType type,
                                const std::string& label) {
    out += "  n";
    append_int(out, id);
    switch (type) {
        case NodeType::Memcpy:
        case NodeType::Memset:
            out += "[/\"";
            out += label;
            out += "\"/]";
            break;
        case NodeType::MemAlloc:
        case NodeType::MemFree:
            out += "[(\"";
            out += label;
            out += "\")]";
            break;
        case NodeType::EventRecord:
        case NodeType::EventWait:
        case NodeType::Host:
        case NodeType::ExtSemaphore:
            out += "{{\"";
            out += label;
            out += "\"}}";
            break;
        case NodeType::ChildGraph:
            out += "[[\"";
            out += label;
            out += "\"]]";
            break;
        case NodeType::Kernel:
        case NodeType::Empty:
        case NodeType::Unknown:
            out += "[\"";
            out += label;
            out += "\"]";
            break;
    }
    out += '\n';
}

inline void append_mermaid_class(std::string& out, const char* kind,
                                 const std::vector<int>& ids) {
    if (ids.empty()) {
        return;
    }
    out += "  class ";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += 'n';
        append_int(out, ids[i]);
    }
    out += ' ';
    out += kind;
    out += '\n';
}

}  // namespace detail

// The label is the type token and the id, never the name: a closed alphabet is the
// defence, and the cap banner is the only text outside it.
[[nodiscard]] inline std::string to_mermaid(const Graph& g, const Topology& t,
                                            const MermaidOptions& options = {}) {
    GRAPHLINT_ASSERT(t.acyclic);
    GRAPHLINT_ASSERT(t.level.size() == g.nodes.size());
    GRAPHLINT_ASSERT(detail::mermaid_edges_match(g, t));
    GRAPHLINT_ASSERT(options.max_nodes >= 1);

    const std::size_t n = g.nodes.size();
    std::vector<std::pair<int, int>> edges = t.edges_minimal;
    edges.insert(edges.end(), t.edges_redundant.begin(), t.edges_redundant.end());
    std::sort(edges.begin(), edges.end());

    std::vector<int> in_degree(n, 0);
    std::vector<int> out_degree(n, 0);
    std::vector<int> only_in(n, -1);
    for (const std::pair<int, int>& edge : edges) {
        const std::size_t from = static_cast<std::size_t>(edge.first);
        const std::size_t to = static_cast<std::size_t>(edge.second);
        ++out_degree[from];
        ++in_degree[to];
        only_in[to] = edge.first;
    }

    // Ids are a topological order: a kernel joins the chain of its only predecessor
    // when neither has a second edge, so the head is always the smallest id.
    std::vector<int> drawn_of(n, -1);
    std::vector<int> chain_size(n, 0);
    std::vector<int> heads;
    heads.reserve(n);
    for (std::size_t index = 0; index < n; ++index) {
        const Node& node = g.nodes[index];
        int head = -1;
        if (node.type == NodeType::Kernel && in_degree[index] == 1 && only_in[index] >= 0) {
            const std::size_t before = static_cast<std::size_t>(only_in[index]);
            if (g.nodes[before].type == NodeType::Kernel && out_degree[before] == 1) {
                head = drawn_of[before];
            }
        }
        if (head >= 0) {
            drawn_of[index] = head;
            ++chain_size[static_cast<std::size_t>(head)];
        } else {
            drawn_of[index] = static_cast<int>(index);
            chain_size[index] = 1;
            heads.push_back(static_cast<int>(index));
        }
    }

    // The path is weighted in ns and a hop count in levels; with weights 1 the two agree
    // (§3 of docs/weights.md), so the committed artifacts stay byte-identical.
    CriticalPath path;
    GraphError path_error;
    static_cast<void>(critical_path(g, t, path, path_error));
    std::vector<char> on_path(n, 0);
    for (const int id : path.nodes) {
        on_path[static_cast<std::size_t>(id)] = 1;
    }

    std::vector<char> critical(n, 0);
    for (std::size_t index = 0; index < n; ++index) {
        if (on_path[index] != 0) {
            critical[static_cast<std::size_t>(drawn_of[index])] = 1;
        }
    }

    std::vector<int> critical_ids;
    std::vector<int> sync_ids;
    for (const int head : heads) {
        const std::size_t at = static_cast<std::size_t>(head);
        if (critical[at] != 0) {
            critical_ids.push_back(head);
        }
        if (chain_size[at] == 1 && detail::mermaid_is_sync(g.nodes[at].type)) {
            sync_ids.push_back(head);
        }
    }

    std::vector<detail::MermaidLink> links;
    links.reserve(edges.size());
    for (const std::pair<int, int>& edge : edges) {
        const int from = drawn_of[static_cast<std::size_t>(edge.first)];
        const int to = drawn_of[static_cast<std::size_t>(edge.second)];
        if (from == to) {
            continue;
        }
        const bool dotted = std::binary_search(t.edges_redundant.begin(),
                                               t.edges_redundant.end(), edge);
        links.push_back(detail::MermaidLink{from, to, dotted});
    }
    std::sort(links.begin(), links.end(), detail::mermaid_link_less);
    links.erase(std::unique(links.begin(), links.end(), detail::mermaid_link_same_pair),
                links.end());

    std::string out;
    out.reserve(96 + heads.size() * 32 + links.size() * 20 + critical_ids.size() * 8);
    out += "flowchart LR\n";
    if (static_cast<int>(heads.size()) > options.max_nodes) {
        // A node, not a %% comment: GitHub hides the comment and the reader never learns
        // the drawing was meant to be smaller.
        out += "  limit[\"";
        detail::append_int(out, static_cast<int>(heads.size()));
        out += " nodes after compression, over --max-nodes ";
        detail::append_int(out, options.max_nodes);
        out += ": see the DOT output\"]\n";
    }
    for (const int head : heads) {
        const std::size_t at = static_cast<std::size_t>(head);
        std::string label = to_string(g.nodes[at].type);
        label += '#';
        detail::append_int(label, head);
        if (chain_size[at] > 1) {
            label += "\xc3\x97";
            detail::append_uint(label, static_cast<std::uint64_t>(chain_size[at]));
        }
        detail::append_mermaid_node(out, head, g.nodes[at].type, label);
    }
    for (const detail::MermaidLink& link : links) {
        out += "  n";
        detail::append_int(out, link.from);
        if (link.dotted) {
            out += " -.-> n";
        } else {
            out += " --> n";
        }
        detail::append_int(out, link.to);
        out += '\n';
    }
    out += "  classDef critical stroke:#c00,stroke-width:2px\n";
    out += "  classDef sync fill:#f90\n";
    detail::append_mermaid_class(out, "critical", critical_ids);
    detail::append_mermaid_class(out, "sync", sync_ids);
    return out;
}

}  // namespace graphlint
