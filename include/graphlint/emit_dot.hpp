#pragma once

#include <graphlint/analysis.hpp>
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

struct DotOptions {
    bool detail_params = false;
};

namespace detail {

// The counts can agree while the pairs do not, and the drawing would then show an edge
// the graph does not have: compare the sorted pairs themselves, not their total.
[[nodiscard]] inline bool dot_edges_match(const Graph& g, const Topology& t) {
    std::vector<std::pair<int, int>> pairs = t.edges_minimal;
    pairs.insert(pairs.end(), t.edges_redundant.begin(), t.edges_redundant.end());
    std::sort(pairs.begin(), pairs.end());
    std::vector<std::pair<int, int>> model = g.edges;
    std::sort(model.begin(), model.end());
    model.erase(std::unique(model.begin(), model.end()), model.end());
    return pairs == model;
}

// A byte check, never mbrtowc: LC_ALL decides nothing about what a document holds.
[[nodiscard]] inline bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80u) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0u) == 0xc0u) {
            extra = 1;
            code = lead & 0x1fu;
            minimum = 0x80u;
        } else if ((lead & 0xf0u) == 0xe0u) {
            extra = 2;
            code = lead & 0x0fu;
            minimum = 0x800u;
        } else if ((lead & 0xf8u) == 0xf0u) {
            extra = 3;
            code = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (text.size() - i - 1 < extra) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned byte = static_cast<unsigned char>(text[i + k]);
            if ((byte & 0xc0u) != 0x80u) {
                return false;
            }
            code = (code << 6) | (byte & 0x3fu);
        }
        if (code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// Controls become spaces before the two escapes: a newline left in a label ends the
// line and hands the renderer a different document.
inline void append_dot_escaped(std::string& out, std::string_view text) {
    for (const char ch : text) {
        const unsigned byte = static_cast<unsigned char>(ch);
        if (byte < 0x20u || byte == 0x7fu) {
            out += ' ';
        } else if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
}

[[nodiscard]] inline std::string dot_detail(const Node& node) {
    std::string detail;
    switch (node.type) {
        case NodeType::Kernel:
            detail += "grid ";
            append_uint(detail, node.grid[0]);
            detail += 'x';
            append_uint(detail, node.grid[1]);
            detail += 'x';
            append_uint(detail, node.grid[2]);
            detail += " block ";
            append_uint(detail, node.block[0]);
            detail += 'x';
            append_uint(detail, node.block[1]);
            detail += 'x';
            append_uint(detail, node.block[2]);
            if (node.shared_bytes != 0) {
                detail += " shared ";
                append_uint(detail, node.shared_bytes);
                detail += " B";
            }
            break;
        case NodeType::MemAlloc:
            if (node.alloc_bytes != 0) {
                append_uint(detail, node.alloc_bytes);
                detail += " B";
            }
            break;
        case NodeType::ChildGraph:
            detail += "child ";
            append_int(detail, node.child);
            break;
        default:
            break;
    }
    if (node.type == NodeType::MemAlloc || node.type == NodeType::MemFree) {
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += "ptr 0x";
        append_hex(detail, node.mem_ptr);
    }
    return detail;
}

// The name is drawn only for a kernel and only when it is UTF-8: a label is the one
// place where an untrusted string reaches the document.
[[nodiscard]] inline std::string dot_label(const Node& node, bool detail_params) {
    std::string label;
    if (node.type == NodeType::Kernel && !node.name.empty() && valid_utf8(node.name)) {
        append_dot_escaped(label, node.name);
    } else {
        label = to_string(node.type);
        label += '#';
        append_int(label, node.id);
    }
    if (detail_params) {
        const std::string detail = dot_detail(node);
        if (!detail.empty()) {
            // The escape is inserted after escaping: it is syntax, not part of the name.
            label += "\\n";
            label += detail;
        }
    }
    return label;
}

inline void append_dot_attrs(std::string& out, NodeType type) {
    switch (type) {
        case NodeType::Memcpy:
        case NodeType::Memset:
            out += ", shape=parallelogram";
            break;
        case NodeType::MemAlloc:
        case NodeType::MemFree:
            out += ", style=filled, fillcolor=gray85";
            break;
        case NodeType::EventRecord:
        case NodeType::EventWait:
        case NodeType::Host:
        case NodeType::ExtSemaphore:
            out += ", shape=octagon, style=filled, fillcolor=orange";
            break;
        case NodeType::ChildGraph:
            out += ", shape=box3d";
            break;
        case NodeType::Kernel:
        case NodeType::Empty:
        case NodeType::Unknown:
            break;
    }
}

}  // namespace detail

// Root graph only: a child_graph is one box, and the drawing shares ids with the JSON.
[[nodiscard]] inline std::string to_dot(const Graph& g, const Topology& t,
                                        const DotOptions& options = {}) {
    GRAPHLINT_ASSERT(t.acyclic);
    GRAPHLINT_ASSERT(t.level.size() == g.nodes.size());
    GRAPHLINT_ASSERT(detail::dot_edges_match(g, t));

    std::string out;
    out.reserve(96 + g.nodes.size() * 48 +
                (t.edges_minimal.size() + t.edges_redundant.size()) * 24);
    out += "digraph graphlint {\n";
    out += "  rankdir=LR;\n";
    out += "  node [shape=box];\n";

    for (const Node& node : g.nodes) {
        out += "  n";
        detail::append_int(out, node.id);
        out += " [label=\"";
        out += detail::dot_label(node, options.detail_params);
        out += '"';
        detail::append_dot_attrs(out, node.type);
        out += "];\n";
    }

    std::vector<std::pair<int, int>> edges = t.edges_minimal;
    edges.insert(edges.end(), t.edges_redundant.begin(), t.edges_redundant.end());
    std::sort(edges.begin(), edges.end());
    if (!g.nodes.empty() && !edges.empty()) {
        out += '\n';
    }
    for (const std::pair<int, int>& edge : edges) {
        out += "  n";
        detail::append_int(out, edge.first);
        out += " -> n";
        detail::append_int(out, edge.second);
        if (std::binary_search(t.edges_redundant.begin(), t.edges_redundant.end(), edge)) {
            out += " [style=dashed]";
        }
        out += ";\n";
    }
    out += "}\n";
    return out;
}

}  // namespace graphlint
