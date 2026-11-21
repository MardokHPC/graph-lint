#pragma once

#include <graphlint/config.hpp>
#include <graphlint/graph.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace graphlint {

namespace detail {

inline void append_uint(std::string& out, std::uint64_t value) {
    char buffer[24];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    GRAPHLINT_ASSERT(result.ec == std::errc());
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

inline void append_int(std::string& out, int value) {
    char buffer[24];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    GRAPHLINT_ASSERT(result.ec == std::errc());
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

inline void append_hex(std::string& out, std::uint64_t value) {
    char buffer[24];
    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
    GRAPHLINT_ASSERT(result.ec == std::errc());
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

inline void append_dims(std::string& out, const std::array<std::uint32_t, 3>& dims) {
    out += '[';
    append_uint(out, dims[0]);
    out += ", ";
    append_uint(out, dims[1]);
    out += ", ";
    append_uint(out, dims[2]);
    out += ']';
}

// Bytes >= 0x20 pass through, so a UTF-8 kernel name stays readable.
inline void append_json_string(std::string& out, std::string_view value) {
    constexpr char k_hex[] = "0123456789abcdef";
    out += '"';
    for (const char ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte == '"') {
            out += "\\\"";
        } else if (byte == '\\') {
            out += "\\\\";
        } else if (byte < 0x20) {
            out += "\\u00";
            out += k_hex[byte >> 4];
            out += k_hex[byte & 0x0fu];
        } else {
            out += ch;
        }
    }
    out += '"';
}

[[nodiscard]] inline const char* source_token(SourceKind kind) {
    switch (kind) {
        case SourceKind::Capture: return "capture";
        case SourceKind::Built: return "built";
        case SourceKind::File: return "file";
    }
    return "file";
}

[[nodiscard]] inline const char* weight_token(WeightKind kind) {
    switch (kind) {
        case WeightKind::Levels: return "levels";
        case WeightKind::Nanoseconds: return "ns";
    }
    return "levels";
}

inline void dump_node_into(std::string& out, const Node& node, WeightKind weight_kind) {
    out += '{';
    bool first = true;
    const auto field = [&](const char* key) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += '"';
        out += key;
        out += "\": ";
    };

    if (node.type == NodeType::MemAlloc && node.alloc_bytes != 0) {
        field("alloc_bytes");
        append_uint(out, node.alloc_bytes);
    }
    if (node.type == NodeType::Kernel) {
        field("block");
        append_dims(out, node.block);
    }
    if (node.type == NodeType::ChildGraph) {
        field("child");
        append_int(out, node.child);
    }
    if (node.type == NodeType::Kernel) {
        field("grid");
        append_dims(out, node.grid);
    }
    field("id");
    append_int(out, node.id);
    if (node.type == NodeType::MemAlloc || node.type == NodeType::MemFree) {
        field("mem_ptr");
        out += '"';
        out += "0x";
        append_hex(out, node.mem_ptr);
        out += '"';
    }
    if (!node.name.empty()) {
        field("name");
        append_json_string(out, node.name);
    }
    if (node.type == NodeType::Kernel && node.shared_bytes != 0) {
        field("shared_bytes");
        append_uint(out, node.shared_bytes);
    }
    field("type");
    out += '"';
    out += to_string(node.type);
    out += '"';
    if (weight_kind == WeightKind::Nanoseconds) {
        field("weight");
        append_uint(out, node.weight);
    }
    out += '}';
}

inline void dump_graph_into(std::string& out, const Graph& graph, int level, bool root) {
    const std::string pad(static_cast<std::size_t>(level) * 2, ' ');
    const std::string inner(static_cast<std::size_t>(level + 1) * 2, ' ');
    const std::string item(static_cast<std::size_t>(level + 2) * 2, ' ');

    out += "{\n";
    out += inner;
    out += "\"children\": ";
    if (graph.children.empty()) {
        out += "[]";
    } else {
        out += "[\n";
        for (std::size_t i = 0; i < graph.children.size(); ++i) {
            out += item;
            dump_graph_into(out, graph.children[i], level + 2, false);
            out += i + 1 < graph.children.size() ? ",\n" : "\n";
        }
        out += inner;
        out += ']';
    }
    out += ",\n";

    out += inner;
    out += "\"edges\": ";
    std::vector<std::pair<int, int>> edges = graph.edges;
    std::sort(edges.begin(), edges.end());
    if (edges.empty()) {
        out += "[]";
    } else {
        out += "[\n";
        for (std::size_t i = 0; i < edges.size(); ++i) {
            out += item;
            out += '[';
            append_int(out, edges[i].first);
            out += ", ";
            append_int(out, edges[i].second);
            out += ']';
            out += i + 1 < edges.size() ? ",\n" : "\n";
        }
        out += inner;
        out += ']';
    }
    out += ",\n";

    out += inner;
    out += "\"nodes\": ";
    if (graph.nodes.empty()) {
        out += "[]";
    } else {
        out += "[\n";
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            out += item;
            dump_node_into(out, graph.nodes[i], graph.weight_kind);
            out += i + 1 < graph.nodes.size() ? ",\n" : "\n";
        }
        out += inner;
        out += ']';
    }

    if (root) {
        out += ",\n";
        out += inner;
        out += "\"schema_version\": 1,\n";
        out += inner;
        out += "\"source\": {\"kind\": \"";
        out += source_token(graph.source);
        out += "\"},\n";
        out += inner;
        out += "\"weight_kind\": \"";
        out += weight_token(graph.weight_kind);
        out += '"';
    }
    out += '\n';
    out += pad;
    out += '}';
}

[[nodiscard]] inline int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

inline void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xc0u | (code >> 6));
        out += static_cast<char>(0x80u | (code & 0x3fu));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0u | (code >> 12));
        out += static_cast<char>(0x80u | ((code >> 6) & 0x3fu));
        out += static_cast<char>(0x80u | (code & 0x3fu));
    } else {
        out += static_cast<char>(0xf0u | (code >> 18));
        out += static_cast<char>(0x80u | ((code >> 12) & 0x3fu));
        out += static_cast<char>(0x80u | ((code >> 6) & 0x3fu));
        out += static_cast<char>(0x80u | (code & 0x3fu));
    }
}

// The primitives of a document reader: whitespace, strings, integers, ordered keys and the
// line/column convention, shared so graph.json and report.json cannot spell one fault two ways.
class Scanner {
public:
    Scanner(std::string_view text, GraphError& error) : text_(text), error_(error) {}

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    void seek(std::size_t pos) noexcept { pos_ = pos; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

    [[nodiscard]] int peek() const {
        if (pos_ >= text_.size()) {
            return -1;
        }
        return static_cast<int>(static_cast<unsigned char>(text_[pos_]));
    }

    void skip_whitespace() {
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                return;
            }
            ++pos_;
        }
    }

    [[nodiscard]] bool expect(char expected) {
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            return fail(GraphErrorKind::Syntax, pos_, std::string("expected '") + expected + "'");
        }
        ++pos_;
        return true;
    }

    // Both 1-based; an offset past the end reports the end of the text.
    void position_of(std::size_t at, int& line, int& column) const {
        std::size_t row = 1;
        std::size_t col = 1;
        const std::size_t limit = at < text_.size() ? at : text_.size();
        for (std::size_t i = 0; i < limit; ++i) {
            if (text_[i] == '\n') {
                ++row;
                col = 1;
            } else {
                ++col;
            }
        }
        line = static_cast<int>(row);
        column = static_cast<int>(col);
    }

    [[nodiscard]] bool fail(GraphErrorKind kind, std::size_t at, std::string detail) {
        int line = 0;
        int column = 0;
        position_of(at, line, column);
        return fail_at(kind, line, column, std::move(detail));
    }

    [[nodiscard]] bool fail_at(GraphErrorKind kind, int line, int column, std::string detail) {
        error_.kind = kind;
        error_.line = line;
        error_.column = column;
        error_.detail = std::move(detail);
        return false;
    }

    [[nodiscard]] bool parse_hex4(std::uint32_t& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size()) {
                return fail(GraphErrorKind::Syntax, pos_, "truncated \\u escape");
            }
            const int digit = hex_digit(text_[pos_]);
            if (digit < 0) {
                return fail(GraphErrorKind::Syntax, pos_, "invalid \\u escape");
            }
            code = code * 16 + static_cast<std::uint32_t>(digit);
            ++pos_;
        }
        return true;
    }

    [[nodiscard]] bool parse_string(std::string& out) {
        if (peek() != '"') {
            return fail(GraphErrorKind::WrongType, pos_, "expected a string");
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            ++pos_;
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                if (static_cast<unsigned char>(ch) < 0x20) {
                    return fail(GraphErrorKind::Syntax, pos_ - 1, "control character in string");
                }
                out += ch;
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail(GraphErrorKind::Syntax, pos_, "unterminated escape sequence");
            }
            const char escape = text_[pos_];
            ++pos_;
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    const std::size_t escape_pos = pos_ - 2;
                    std::uint32_t high = 0;
                    if (!parse_hex4(high)) {
                        return false;
                    }
                    if (high >= 0xd800 && high <= 0xdbff) {
                        // RFC 8259: a high surrogate is only half a character.
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            return fail(GraphErrorKind::Syntax, escape_pos,
                                        "high surrogate requires a low surrogate \\udc00-\\udfff");
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(low)) {
                            return false;
                        }
                        if (low < 0xdc00 || low > 0xdfff) {
                            return fail(GraphErrorKind::Syntax, escape_pos,
                                        "high surrogate requires a low surrogate \\udc00-\\udfff");
                        }
                        append_utf8(out, 0x10000u + ((high - 0xd800u) << 10) + (low - 0xdc00u));
                        break;
                    }
                    if (high >= 0xdc00 && high <= 0xdfff) {
                        return fail(GraphErrorKind::Syntax, escape_pos,
                                    "low surrogate without a preceding high surrogate");
                    }
                    append_utf8(out, high);
                    break;
                }
                default:
                    return fail(GraphErrorKind::Syntax, pos_ - 1, "invalid escape sequence");
            }
        }
        return fail(GraphErrorKind::Syntax, pos_, "unterminated string");
    }

    [[nodiscard]] bool parse_uint64(std::uint64_t& value) {
        const std::size_t start = pos_;
        if (peek() == '-') {
            return fail(GraphErrorKind::BadNumber, start, "expected an unsigned integer");
        }
        if (peek() < '0' || peek() > '9') {
            return fail(GraphErrorKind::WrongType, start, "expected an integer");
        }
        std::uint64_t result = 0;
        if (text_[pos_] == '0') {
            ++pos_;
            if (peek() >= '0' && peek() <= '9') {
                return fail(GraphErrorKind::BadNumber, start, "leading zero in integer");
            }
        } else {
            while (peek() >= '0' && peek() <= '9') {
                const std::uint64_t digit = static_cast<std::uint64_t>(peek() - '0');
                if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    return fail(GraphErrorKind::BadNumber, start, "integer out of range");
                }
                result = result * 10 + digit;
                ++pos_;
            }
        }
        const int next = peek();
        if (next == '.' || next == 'e' || next == 'E') {
            return fail(GraphErrorKind::BadNumber, start, "expected an integer");
        }
        value = result;
        return true;
    }

    [[nodiscard]] bool parse_int(int& value) {
        const std::size_t start = pos_;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++pos_;
        }
        std::uint64_t magnitude = 0;
        if (!parse_uint64(magnitude)) {
            return false;
        }
        const std::uint64_t max_positive =
            static_cast<std::uint64_t>(std::numeric_limits<int>::max());
        if (negative) {
            const std::uint64_t limit = max_positive + 1u;
            if (magnitude > limit) {
                return fail(GraphErrorKind::BadNumber, start, "integer out of range");
            }
            value = magnitude == limit ? std::numeric_limits<int>::min()
                                       : -static_cast<int>(magnitude);
        } else {
            if (magnitude > max_positive) {
                return fail(GraphErrorKind::BadNumber, start, "integer out of range");
            }
            value = static_cast<int>(magnitude);
        }
        return true;
    }

    template <typename Handler>
    [[nodiscard]] bool parse_object(Handler&& handler) {
        if (!expect('{')) {
            return false;
        }
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        std::string previous;
        bool first = true;
        while (true) {
            skip_whitespace();
            const std::size_t key_pos = pos_;
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            if (!first) {
                if (key == previous) {
                    return fail(GraphErrorKind::DuplicateKey, key_pos,
                                "duplicate key \"" + key + "\"");
                }
                if (key < previous) {
                    return fail(GraphErrorKind::UnsortedKey, key_pos,
                                "unsorted key \"" + key + "\"");
                }
            }
            first = false;
            previous = key;
            skip_whitespace();
            if (!expect(':')) {
                return false;
            }
            skip_whitespace();
            if (!handler(key, key_pos)) {
                return false;
            }
            skip_whitespace();
            const int next = peek();
            if (next == ',') {
                ++pos_;
                continue;
            }
            if (next == '}') {
                ++pos_;
                return true;
            }
            return fail(GraphErrorKind::Syntax, pos_, "expected ',' or '}'");
        }
    }

    template <typename Handler>
    [[nodiscard]] bool parse_array(Handler&& handler) {
        if (!expect('[')) {
            return false;
        }
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_whitespace();
            if (!handler()) {
                return false;
            }
            skip_whitespace();
            const int next = peek();
            if (next == ',') {
                ++pos_;
                continue;
            }
            if (next == ']') {
                ++pos_;
                return true;
            }
            return fail(GraphErrorKind::Syntax, pos_, "expected ',' or ']'");
        }
    }

    // End (exclusive) of the object or array starting at pos_. Strings may hold brackets and
    // their contents are the caller's grammar, so only the extent is this reader's business.
    [[nodiscard]] bool skip_compound(std::size_t& end) {
        const int open = peek();
        if (open != '{' && open != '[') {
            return fail(GraphErrorKind::WrongType, pos_, "expected an object");
        }
        int depth = 0;
        std::size_t i = pos_;
        while (i < text_.size()) {
            const char ch = text_[i];
            if (ch == '"') {
                ++i;
                while (i < text_.size() && text_[i] != '"') {
                    i += text_[i] == '\\' ? 2 : 1;
                }
                if (i >= text_.size()) {
                    return fail(GraphErrorKind::Syntax, i, "unterminated string");
                }
                ++i;
                continue;
            }
            if (ch == '{' || ch == '[') {
                ++depth;
            } else if (ch == '}' || ch == ']') {
                --depth;
                if (depth == 0) {
                    end = i + 1;
                    return true;
                }
            }
            ++i;
        }
        return fail(GraphErrorKind::Syntax, i, "unterminated object");
    }

protected:
    std::string_view text_;
    std::size_t pos_ = 0;
    GraphError& error_;
};

struct RawNode {
    Node node;
    bool has_id = false;
    bool has_type = false;
    bool has_weight = false;
    bool has_grid = false;
    bool has_block = false;
    bool has_shared_bytes = false;
    bool has_alloc_bytes = false;
    bool has_mem_ptr = false;
    bool has_child = false;
    std::size_t object_pos = 0;
    std::size_t id_pos = 0;
    std::size_t weight_pos = 0;
    std::size_t grid_pos = 0;
    std::size_t block_pos = 0;
    std::size_t alloc_pos = 0;
    std::size_t mem_ptr_pos = 0;
    std::size_t child_pos = 0;
};

struct RawGraph {
    std::vector<RawNode> nodes;
    std::vector<std::pair<int, int>> edges;
    std::vector<std::size_t> edge_positions;
    std::vector<RawGraph> children;
    SourceKind source = SourceKind::File;
    WeightKind weight_kind = WeightKind::Levels;
    std::size_t object_pos = 0;
};

class Parser : public Scanner {
public:
    Parser(std::string_view text, GraphError& error) : Scanner(text, error) {}

    [[nodiscard]] bool parse(Graph& out) {
        skip_whitespace();
        RawGraph root;
        if (!parse_graph_object(root, 1, true)) {
            return false;
        }
        skip_whitespace();
        if (pos_ != text_.size()) {
            return fail(GraphErrorKind::Trailing, pos_, "trailing bytes after the document");
        }
        Graph graph;
        if (!finish(root, root.weight_kind, root.source, true, graph)) {
            return false;
        }
        out = std::move(graph);
        return true;
    }

private:
    [[nodiscard]] bool parse_u32(std::uint32_t& value) {
        const std::size_t start = pos_;
        std::uint64_t wide = 0;
        if (!parse_uint64(wide)) {
            return false;
        }
        if (wide > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(GraphErrorKind::BadNumber, start, "value exceeds 32 bits");
        }
        value = static_cast<std::uint32_t>(wide);
        return true;
    }

    [[nodiscard]] bool parse_dims(std::array<std::uint32_t, 3>& dims) {
        if (!expect('[')) {
            return false;
        }
        for (std::size_t i = 0; i < dims.size(); ++i) {
            skip_whitespace();
            if (!parse_u32(dims[i])) {
                return false;
            }
            skip_whitespace();
            if (i + 1 < dims.size() && !expect(',')) {
                return false;
            }
        }
        return expect(']');
    }

    [[nodiscard]] bool parse_mem_ptr(std::uint64_t& value) {
        const std::size_t start = pos_;
        std::string token;
        if (!parse_string(token)) {
            return false;
        }
        if (token.size() < 3 || token[0] != '0' || token[1] != 'x') {
            return fail(GraphErrorKind::BadNumber, start,
                        "mem_ptr must be 0x followed by hex digits");
        }
        std::uint64_t result = 0;
        for (std::size_t i = 2; i < token.size(); ++i) {
            const char ch = token[i];
            int digit = -1;
            if (ch >= '0' && ch <= '9') {
                digit = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                digit = ch - 'a' + 10;
            } else {
                return fail(GraphErrorKind::BadNumber, start,
                            "mem_ptr must be lowercase hexadecimal");
            }
            if (result > (std::numeric_limits<std::uint64_t>::max() -
                          static_cast<std::uint64_t>(digit)) /
                             16) {
                return fail(GraphErrorKind::BadNumber, start, "mem_ptr exceeds 64 bits");
            }
            result = result * 16 + static_cast<std::uint64_t>(digit);
        }
        value = result;
        return true;
    }

    [[nodiscard]] bool parse_graph_object(RawGraph& raw, std::size_t depth, bool root) {
        if (depth > static_cast<std::size_t>(k_max_graph_depth)) {
            return fail(GraphErrorKind::TooDeep, pos_,
                        "graph nesting deeper than " + std::to_string(k_max_graph_depth));
        }
        raw.object_pos = pos_;
        bool has_children = false;
        bool has_edges = false;
        bool has_nodes = false;
        bool has_schema_version = false;
        bool has_source = false;
        bool has_weight_kind = false;

        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key == "children") {
                has_children = true;
                return parse_array([&]() {
                    raw.children.emplace_back();
                    return parse_graph_object(raw.children.back(), depth + 1, false);
                });
            }
            if (key == "edges") {
                has_edges = true;
                return parse_array([&]() {
                    const std::size_t edge_pos = pos_;
                    if (!expect('[')) {
                        return false;
                    }
                    std::pair<int, int> edge{0, 0};
                    skip_whitespace();
                    if (!parse_int(edge.first)) {
                        return false;
                    }
                    skip_whitespace();
                    if (!expect(',')) {
                        return false;
                    }
                    skip_whitespace();
                    if (!parse_int(edge.second)) {
                        return false;
                    }
                    skip_whitespace();
                    if (!expect(']')) {
                        return false;
                    }
                    raw.edges.push_back(edge);
                    raw.edge_positions.push_back(edge_pos);
                    return true;
                });
            }
            if (key == "nodes") {
                has_nodes = true;
                return parse_array([&]() {
                    raw.nodes.emplace_back();
                    return parse_node_object(raw.nodes.back());
                });
            }
            if (root && key == "schema_version") {
                has_schema_version = true;
                const std::size_t value_pos = pos_;
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
            if (root && key == "source") {
                has_source = true;
                return parse_source(raw.source);
            }
            if (root && key == "weight_kind") {
                has_weight_kind = true;
                return parse_weight_kind(raw.weight_kind);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }

        if (!has_children) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"children\"");
        }
        if (!has_edges) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"edges\"");
        }
        if (!has_nodes) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"nodes\"");
        }
        if (root && !has_schema_version) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos,
                        "missing key \"schema_version\"");
        }
        if (root && !has_source) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"source\"");
        }
        if (root && !has_weight_kind) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos,
                        "missing key \"weight_kind\"");
        }
        return true;
    }

    [[nodiscard]] bool parse_source(SourceKind& kind) {
        bool has_kind = false;
        const std::size_t object_pos = pos_;
        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            if (key != "kind") {
                return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
            }
            has_kind = true;
            const std::size_t value_pos = pos_;
            std::string token;
            if (!parse_string(token)) {
                return false;
            }
            if (token == "capture") {
                kind = SourceKind::Capture;
                return true;
            }
            if (token == "built") {
                kind = SourceKind::Built;
                return true;
            }
            if (token == "file") {
                kind = SourceKind::File;
                return true;
            }
            return fail(GraphErrorKind::UnknownType, value_pos,
                        "unknown source kind \"" + token + "\"");
        });
        if (!parsed) {
            return false;
        }
        if (!has_kind) {
            return fail(GraphErrorKind::MissingKey, object_pos, "missing key \"kind\"");
        }
        return true;
    }

    [[nodiscard]] bool parse_weight_kind(WeightKind& weight_kind) {
        const std::size_t value_pos = pos_;
        std::string token;
        if (!parse_string(token)) {
            return false;
        }
        if (token == "levels") {
            weight_kind = WeightKind::Levels;
            return true;
        }
        if (token == "ns") {
            weight_kind = WeightKind::Nanoseconds;
            return true;
        }
        return fail(GraphErrorKind::UnknownType, value_pos,
                    "unknown weight_kind \"" + token + "\"");
    }

    [[nodiscard]] bool parse_node_object(RawNode& raw) {
        raw.object_pos = pos_;
        const bool parsed = parse_object([&](const std::string& key, std::size_t key_pos) {
            const std::size_t value_pos = pos_;
            if (key == "alloc_bytes") {
                raw.has_alloc_bytes = true;
                raw.alloc_pos = value_pos;
                return parse_uint64(raw.node.alloc_bytes);
            }
            if (key == "block") {
                raw.has_block = true;
                raw.block_pos = value_pos;
                return parse_dims(raw.node.block);
            }
            if (key == "child") {
                raw.has_child = true;
                raw.child_pos = value_pos;
                return parse_int(raw.node.child);
            }
            if (key == "grid") {
                raw.has_grid = true;
                raw.grid_pos = value_pos;
                return parse_dims(raw.node.grid);
            }
            if (key == "id") {
                raw.has_id = true;
                raw.id_pos = value_pos;
                return parse_int(raw.node.id);
            }
            if (key == "mem_ptr") {
                raw.has_mem_ptr = true;
                raw.mem_ptr_pos = value_pos;
                return parse_mem_ptr(raw.node.mem_ptr);
            }
            if (key == "name") {
                return parse_string(raw.node.name);
            }
            if (key == "shared_bytes") {
                raw.has_shared_bytes = true;
                return parse_uint64(raw.node.shared_bytes);
            }
            if (key == "type") {
                raw.has_type = true;
                std::string token;
                if (!parse_string(token)) {
                    return false;
                }
                if (!node_type_from_string(token, raw.node.type)) {
                    return fail(GraphErrorKind::UnknownType, value_pos,
                                "unknown node type \"" + token + "\"");
                }
                return true;
            }
            if (key == "weight") {
                raw.has_weight = true;
                raw.weight_pos = value_pos;
                return parse_uint64(raw.node.weight);
            }
            return fail(GraphErrorKind::UnknownKey, key_pos, "unknown key \"" + key + "\"");
        });
        if (!parsed) {
            return false;
        }

        if (!raw.has_id) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"id\"");
        }
        if (!raw.has_type) {
            return fail(GraphErrorKind::MissingKey, raw.object_pos, "missing key \"type\"");
        }

        const NodeType type = raw.node.type;
        const bool is_kernel = type == NodeType::Kernel;
        const bool is_alloc = type == NodeType::MemAlloc;
        const bool is_free = type == NodeType::MemFree;
        const bool is_child_graph = type == NodeType::ChildGraph;

        if (is_kernel) {
            if (!raw.has_grid) {
                return fail(GraphErrorKind::MissingKey, raw.object_pos,
                            "kernel node requires \"grid\"");
            }
            if (!raw.has_block) {
                return fail(GraphErrorKind::MissingKey, raw.object_pos,
                            "kernel node requires \"block\"");
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (raw.node.grid[i] == 0 || raw.node.block[i] == 0) {
                    return fail(GraphErrorKind::FieldForType, raw.grid_pos,
                                "kernel launch dimensions must be >= 1");
                }
            }
        } else {
            if (raw.has_grid) {
                return fail(GraphErrorKind::FieldForType, raw.grid_pos,
                            "field \"grid\" is not valid for type \"" +
                                std::string(to_string(type)) + "\"");
            }
            if (raw.has_block) {
                return fail(GraphErrorKind::FieldForType, raw.block_pos,
                            "field \"block\" is not valid for type \"" +
                                std::string(to_string(type)) + "\"");
            }
            if (raw.has_shared_bytes) {
                return fail(GraphErrorKind::FieldForType, raw.object_pos,
                            "field \"shared_bytes\" is not valid for type \"" +
                                std::string(to_string(type)) + "\"");
            }
        }

        if (!is_alloc && raw.has_alloc_bytes) {
            return fail(GraphErrorKind::FieldForType, raw.alloc_pos,
                        "field \"alloc_bytes\" is not valid for type \"" +
                            std::string(to_string(type)) + "\"");
        }
        if (is_alloc || is_free) {
            if (!raw.has_mem_ptr) {
                return fail(GraphErrorKind::MissingKey, raw.object_pos,
                            "memory node requires \"mem_ptr\"");
            }
        } else if (raw.has_mem_ptr) {
            return fail(GraphErrorKind::FieldForType, raw.mem_ptr_pos,
                        "field \"mem_ptr\" is not valid for type \"" +
                            std::string(to_string(type)) + "\"");
        }
        if (is_child_graph) {
            if (!raw.has_child) {
                return fail(GraphErrorKind::MissingKey, raw.object_pos,
                            "child_graph node requires \"child\"");
            }
        } else if (raw.has_child) {
            return fail(GraphErrorKind::FieldForType, raw.child_pos,
                        "field \"child\" is not valid for type \"" +
                            std::string(to_string(type)) + "\"");
        }
        return true;
    }

    [[nodiscard]] bool finish(RawGraph& raw, WeightKind inherited_weight_kind,
                              SourceKind inherited_source, bool root, Graph& out) {
        const WeightKind weight_kind = root ? raw.weight_kind : inherited_weight_kind;
        const SourceKind source = root ? raw.source : inherited_source;

        for (const RawNode& raw_node : raw.nodes) {
            if (weight_kind == WeightKind::Levels && raw_node.has_weight) {
                return fail(GraphErrorKind::WeightRule, raw_node.weight_pos,
                            "weight is not allowed when weight_kind is \"levels\"");
            }
            if (weight_kind == WeightKind::Nanoseconds && !raw_node.has_weight) {
                return fail(GraphErrorKind::WeightRule, raw_node.object_pos,
                            "weight is required when weight_kind is \"ns\"");
            }
        }

        const std::size_t n = raw.nodes.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (raw.nodes[i].node.id != static_cast<int>(i)) {
                return fail(GraphErrorKind::IdNotDense, raw.nodes[i].id_pos,
                            "expected id " + std::to_string(i) + ", found " +
                                std::to_string(raw.nodes[i].node.id));
            }
        }
        for (std::size_t i = 0; i < raw.edges.size(); ++i) {
            const std::pair<int, int> edge = raw.edges[i];
            if (edge.first >= edge.second) {
                return fail(GraphErrorKind::EdgeBackwards, raw.edge_positions[i],
                            "edge [" + std::to_string(edge.first) + ", " +
                                std::to_string(edge.second) + "] is not from < to");
            }
            if (edge.first < 0 || edge.second < 0 ||
                static_cast<std::size_t>(edge.second) >= n) {
                return fail(GraphErrorKind::EdgeOutOfRange, raw.edge_positions[i],
                            "edge [" + std::to_string(edge.first) + ", " +
                                std::to_string(edge.second) + "] is outside the node list");
            }
        }

        std::size_t expected_child = 0;
        for (const RawNode& raw_node : raw.nodes) {
            if (raw_node.node.type != NodeType::ChildGraph) {
                continue;
            }
            const int child = raw_node.node.child;
            if (child < 0 || static_cast<std::size_t>(child) >= raw.children.size()) {
                return fail(GraphErrorKind::ChildMismatch, raw_node.child_pos,
                            "child index " + std::to_string(child) + " is out of range");
            }
            if (static_cast<std::size_t>(child) != expected_child) {
                return fail(GraphErrorKind::ChildMismatch, raw_node.child_pos,
                            "child_graph nodes must own children in id order");
            }
            ++expected_child;
        }
        if (expected_child != raw.children.size()) {
            return fail(GraphErrorKind::ChildMismatch, raw.object_pos,
                        "child " + std::to_string(expected_child) +
                            " is not referenced by any node");
        }

        Graph graph;
        graph.source = source;
        graph.weight_kind = weight_kind;
        graph.nodes.reserve(n);
        for (RawNode& raw_node : raw.nodes) {
            graph.nodes.push_back(std::move(raw_node.node));
        }
        graph.edges = std::move(raw.edges);
        graph.children.resize(raw.children.size());
        for (std::size_t i = 0; i < raw.children.size(); ++i) {
            if (!finish(raw.children[i], weight_kind, source, false, graph.children[i])) {
                return false;
            }
        }
        out = std::move(graph);
        return true;
    }
};

}  // namespace detail

[[nodiscard]] inline std::string dump_graph(const Graph& graph) {
#if GRAPHLINT_ENABLE_ASSERTS
    GraphError error;
    GRAPHLINT_ASSERT(validate(graph, error));
#endif
    std::string out;
    out.reserve(256 + graph.nodes.size() * 64);
    detail::dump_graph_into(out, graph, 0, true);
    out += '\n';
    return out;
}

[[nodiscard]] inline bool parse_graph(std::string_view text, Graph& out, GraphError& error) {
    detail::Parser parser(text, error);
    Graph graph;
    if (!parser.parse(graph)) {
        return false;
    }
    out = std::move(graph);
    error = GraphError{};
    return true;
}

}  // namespace graphlint
