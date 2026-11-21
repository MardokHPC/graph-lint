// What the committed benchmark document promises: schema 1, nine rows, the shape facts
// the builder knew, and spreads that contain their median. Timings are measurements.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef GRAPHLINT_BENCH_JSON
#  error "GRAPHLINT_BENCH_JSON must be the path of docs/bench/bench.json"
#endif

namespace {

struct BenchRow {
    std::string shape;
    std::int64_t nodes = 0;
    std::int64_t edges = 0;
    std::int64_t depth = 0;
    std::int64_t redundant_edges = 0;
    std::int64_t zero_point_ns = 0;
    std::int64_t instantiate_empty_ns = 0;
    std::int64_t instantiate_ns = 0;
    std::int64_t instantiate_min_ns = 0;
    std::int64_t instantiate_max_ns = 0;
    std::int64_t instantiate_net_ns = 0;
    std::int64_t launch_ns = 0;
    std::int64_t launch_min_ns = 0;
    std::int64_t launch_max_ns = 0;
    std::int64_t launch_net_ns = 0;
};

struct BenchDocument {
    std::int64_t schema_version = 0;
    std::string device;
    std::int64_t compiled_runtime = 0;
    std::int64_t cuda_runtime = 0;
    std::int64_t driver = 0;
    std::int64_t iterations = 0;
    std::int64_t passes = 0;
    std::int64_t repeats = 0;
    std::vector<BenchRow> results;
};

// Strict on purpose: sorted keys, integers only, no escape. The document is ours, and a
// reader that silently accepts a float would let "12.5" into a schema that says ns.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    void expect(char token) {
        skip();
        if (at_ >= text_.size() || text_[at_] != token) {
            fail(std::string("expected '") + token + "'");
        }
        ++at_;
    }

    [[nodiscard]] bool next_is(char token) {
        skip();
        return at_ < text_.size() && text_[at_] == token;
    }

    void key(std::string_view expected) {
        const std::string actual = string();
        if (actual != expected) {
            fail("expected key \"" + std::string(expected) + "\", found \"" + actual + "\"");
        }
        expect(':');
    }

    std::int64_t integer() {
        skip();
        const std::size_t begin = at_;
        if (at_ < text_.size() && text_[at_] == '-') {
            ++at_;
        }
        const std::size_t digits = at_;
        std::int64_t value = 0;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            value = value * 10 + (text_[at_] - '0');
            ++at_;
        }
        if (at_ == digits) {
            fail("expected an integer");
        }
        if (at_ < text_.size() && (text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E')) {
            fail("a non-integer number: the document holds counts and ns as integers");
        }
        return text_[begin] == '-' ? -value : value;
    }

    std::string string() {
        skip();
        if (at_ >= text_.size() || text_[at_] != '"') {
            fail("expected a string");
        }
        ++at_;
        std::string value;
        while (at_ < text_.size() && text_[at_] != '"') {
            if (text_[at_] == '\\') {
                fail("the bench document carries no escape sequence");
            }
            value += text_[at_++];
        }
        if (at_ >= text_.size()) {
            fail("unterminated string");
        }
        ++at_;
        return value;
    }

    void end() {
        skip();
        if (at_ != text_.size()) {
            fail("trailing bytes after the document");
        }
    }

private:
    void skip() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\t')) {
            ++at_;
        }
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument("offset " + std::to_string(at_) + ": " + message);
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

bool parse_bench(std::string_view text, BenchDocument& out, std::string& error) {
    try {
        Reader reader(text);
        reader.expect('{');
        reader.key("compiled_runtime");
        out.compiled_runtime = reader.integer();
        reader.expect(',');
        reader.key("cuda_runtime");
        out.cuda_runtime = reader.integer();
        reader.expect(',');
        reader.key("device");
        out.device = reader.string();
        reader.expect(',');
        reader.key("driver");
        out.driver = reader.integer();
        reader.expect(',');
        reader.key("iterations");
        out.iterations = reader.integer();
        reader.expect(',');
        reader.key("passes");
        out.passes = reader.integer();
        reader.expect(',');
        reader.key("repeats");
        out.repeats = reader.integer();
        reader.expect(',');
        reader.key("results");
        reader.expect('[');
        while (!reader.next_is(']')) {
            if (!out.results.empty()) {
                reader.expect(',');
            }
            BenchRow row;
            reader.expect('{');
            reader.key("depth");
            row.depth = reader.integer();
            reader.expect(',');
            reader.key("edges");
            row.edges = reader.integer();
            reader.expect(',');
            reader.key("instantiate_empty_ns");
            row.instantiate_empty_ns = reader.integer();
            reader.expect(',');
            reader.key("instantiate_max_ns");
            row.instantiate_max_ns = reader.integer();
            reader.expect(',');
            reader.key("instantiate_min_ns");
            row.instantiate_min_ns = reader.integer();
            reader.expect(',');
            reader.key("instantiate_net_ns");
            row.instantiate_net_ns = reader.integer();
            reader.expect(',');
            reader.key("instantiate_ns");
            row.instantiate_ns = reader.integer();
            reader.expect(',');
            reader.key("launch_max_ns");
            row.launch_max_ns = reader.integer();
            reader.expect(',');
            reader.key("launch_min_ns");
            row.launch_min_ns = reader.integer();
            reader.expect(',');
            reader.key("launch_net_ns");
            row.launch_net_ns = reader.integer();
            reader.expect(',');
            reader.key("launch_ns");
            row.launch_ns = reader.integer();
            reader.expect(',');
            reader.key("nodes");
            row.nodes = reader.integer();
            reader.expect(',');
            reader.key("redundant_edges");
            row.redundant_edges = reader.integer();
            reader.expect(',');
            reader.key("shape");
            row.shape = reader.string();
            reader.expect(',');
            reader.key("zero_point_ns");
            row.zero_point_ns = reader.integer();
            reader.expect('}');
            out.results.push_back(std::move(row));
        }
        reader.expect(']');
        reader.expect(',');
        reader.key("schema_version");
        out.schema_version = reader.integer();
        reader.expect('}');
        reader.end();
        return true;
    } catch (const std::invalid_argument& problem) {
        error = problem.what();
        return false;
    }
}

// Both cases skip when the artifact is absent; the main reports 77 then, ctest SKIPPED.
bool slurp_bench(std::string& text) {
    std::ifstream in(GRAPHLINT_BENCH_JSON, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

const BenchRow* find_row(const BenchDocument& document, std::string_view shape,
                         std::int64_t nodes) {
    for (const BenchRow& row : document.results) {
        if (row.shape == shape && row.nodes == nodes) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

TEST(BenchReport, DocumentIsSchemaOneWithNineIntegerRows) {
    std::string text;
    if (!slurp_bench(text)) {
        GTEST_SKIP() << GRAPHLINT_BENCH_JSON
                     << " is not there: build-device.bat bench writes it, it is opt-in";
    }
    EXPECT_EQ(text.find('\r'), std::string::npos) << "the artifact is LF-only";

    BenchDocument document;
    std::string error;
    ASSERT_TRUE(parse_bench(text, document, error)) << error;
    EXPECT_EQ(document.schema_version, 1);
    EXPECT_EQ(document.iterations, 200);
    EXPECT_EQ(document.repeats, 7);
    EXPECT_EQ(document.passes, 3);
    EXPECT_FALSE(document.device.empty());
    EXPECT_GT(document.driver, 0);
    // The tool refuses to write a document when the two disagree; the field proves it.
    EXPECT_EQ(document.cuda_runtime, document.compiled_runtime);
    ASSERT_EQ(document.results.size(), 9u);
}

TEST(BenchReport, ShapesAgreeWithTheirBuilders) {
    std::string text;
    if (!slurp_bench(text)) {
        GTEST_SKIP() << GRAPHLINT_BENCH_JSON
                     << " is not there: build-device.bat bench writes it, it is opt-in";
    }
    BenchDocument document;
    std::string error;
    ASSERT_TRUE(parse_bench(text, document, error)) << error;

    // 8 / 32 / 128 nodes, the three sizes tools/bench.cu builds.
    for (const std::int64_t n : {std::int64_t{8}, std::int64_t{32}, std::int64_t{128}}) {
        const BenchRow* chain = find_row(document, "chain", n);
        const BenchRow* redundant = find_row(document, "redundant", n);
        const BenchRow* forkjoin = find_row(document, "forkjoin", n);
        ASSERT_NE(chain, nullptr) << "no chain row with " << n << " nodes";
        ASSERT_NE(redundant, nullptr) << "no redundant row with " << n << " nodes";
        ASSERT_NE(forkjoin, nullptr) << "no forkjoin row with " << n << " nodes";

        // chain of n: n-1 edges, depth n, nothing implied.
        EXPECT_EQ(chain->edges, n - 1);
        EXPECT_EQ(chain->depth, n);
        EXPECT_EQ(chain->redundant_edges, 0);
        // same n nodes with n-2 extra edges, all implied: same depth, the reduction finds them.
        EXPECT_EQ(redundant->edges, 2 * n - 3);
        EXPECT_GT(redundant->edges, chain->edges);
        EXPECT_EQ(redundant->depth, n);
        EXPECT_EQ(redundant->redundant_edges, n - 2);
        // one source, n-2 middles, one sink: three levels, no edge implied.
        EXPECT_EQ(forkjoin->edges, 2 * (n - 2));
        EXPECT_EQ(forkjoin->depth, 3);
        EXPECT_EQ(forkjoin->redundant_edges, 0);

        for (const BenchRow* row : {chain, redundant, forkjoin}) {
            // The interval contains its median, and the nets are subtractions: a clamp
            // or a hand-edited max would show up here. Both are arithmetic, not timings.
            EXPECT_LE(row->launch_min_ns, row->launch_ns) << row->shape;
            EXPECT_LE(row->launch_ns, row->launch_max_ns) << row->shape;
            EXPECT_LE(row->instantiate_min_ns, row->instantiate_ns) << row->shape;
            EXPECT_LE(row->instantiate_ns, row->instantiate_max_ns) << row->shape;
            EXPECT_EQ(row->launch_net_ns, row->launch_ns - row->zero_point_ns) << row->shape;
            EXPECT_EQ(row->instantiate_net_ns, row->instantiate_ns - row->instantiate_empty_ns)
                << row->shape;
        }
    }
}
