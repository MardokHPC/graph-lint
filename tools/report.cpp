// graphlint_report: a thin main over tools/report_core.hpp. Argument and file errors are the
// CLI's own; the exit policy that a document is 0 and a failed precondition is 2 lives in the core.
#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>
#include <graphlint/weights.hpp>

#include <tools/report_core.hpp>

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#if defined(GRAPHLINT_CLI_WITH_CUDA)
#include <cuda_runtime_api.h>
#endif

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using graphlint::Environment;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::tools::ReportFormat;
using graphlint::tools::ReportOptions;

// A text-mode stdout turns every LF into CRLF on Windows, and the report is a byte contract.
void enable_binary_stdout() {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

int usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s <graph.json> [--format summary|json|dot|mermaid] [--weights <path>] "
                 "[--out <path>] [--detail-params] [--max-nodes N]\n"
                 "       %s --template <out.json>\n",
                 program, program);
    return 2;
}

// The starting point for a hand-written document: two kernels in a chain is the smallest
// shape with an edge, a depth of two and nothing the linter would flag. It is built here,
// not stored as a string, so the template cannot drift from dump_graph's format.
Graph starting_graph() {
    Graph graph;
    graph.source = graphlint::SourceKind::File;
    graph.weight_kind = graphlint::WeightKind::Levels;
    const char* const names[] = {"kernel_a", "kernel_b"};
    for (const char* name : names) {
        graphlint::Node node;
        node.id = static_cast<int>(graph.nodes.size());
        node.type = graphlint::NodeType::Kernel;
        node.name = name;
        node.grid = {1, 1, 1};
        node.block = {128, 1, 1};
        graph.nodes.push_back(node);
    }
    graph.edges.emplace_back(0, 1);
    return graph;
}

bool parse_int(const char* text, int& value) {
    const std::size_t length = std::strlen(text);
    const std::from_chars_result parsed = std::from_chars(text, text + length, value);
    return parsed.ec == std::errc() && parsed.ptr == text + length;
}

bool format_from_token(const std::string& token, ReportFormat& format) {
    if (token == "summary") {
        format = ReportFormat::summary;
        return true;
    }
    if (token == "json") {
        format = ReportFormat::json;
        return true;
    }
    if (token == "dot") {
        format = ReportFormat::dot;
        return true;
    }
    if (token == "mermaid") {
        format = ReportFormat::mermaid;
        return true;
    }
    return false;
}

Environment local_environment() {
    Environment environment;
#if defined(GRAPHLINT_CLI_WITH_CUDA)
    int runtime = 0;
    if (cudaRuntimeGetVersion(&runtime) == cudaSuccess) {
        environment.cuda_runtime = runtime;
    }
    int device = 0;
    if (cudaGetDevice(&device) == cudaSuccess) {
        cudaDeviceProp properties{};
        if (cudaGetDeviceProperties(&properties, device) == cudaSuccess) {
            environment.device = properties.name;
        }
    }
#endif
    return environment;
}

int write_document(const std::string& path, const std::string& document) {
    // Binary: a text-mode stream would turn every LF into CRLF on a committed artifact.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "%s:0:0: cannot write the file\n", path.c_str());
        return 2;
    }
    file.write(document.data(), static_cast<std::streamsize>(document.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "%s:0:0: short write\n", path.c_str());
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace graphlint;

    enable_binary_stdout();

    std::string path;
    std::string out_path;
    std::string weights_path;
    std::string template_path;
    ReportOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--format" && i + 1 < argc) {
            if (!format_from_token(argv[++i], options.format)) {
                return usage(argv[0]);
            }
        } else if (argument == "--weights" && i + 1 < argc) {
            weights_path = argv[++i];
        } else if (argument == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (argument == "--template" && i + 1 < argc) {
            template_path = argv[++i];
        } else if (argument == "--detail-params") {
            options.detail_params = true;
        } else if (argument == "--max-nodes" && i + 1 < argc) {
            if (!parse_int(argv[++i], options.max_nodes)) {
                return usage(argv[0]);
            }
        } else if (!argument.empty() && argument[0] == '-') {
            return usage(argv[0]);
        } else if (path.empty()) {
            path = argument;
        } else {
            return usage(argv[0]);
        }
    }
    if (!template_path.empty()) {
        if (!path.empty()) {
            std::fprintf(stderr,
                         "graphlint_report: --template writes the starting document and takes "
                         "no graph\n");
            return usage(argv[0]);
        }
        // A template is a starting point: overwriting one would delete the edits made to it.
        bool exists = false;
        {
            std::ifstream probe(template_path, std::ios::binary);
            exists = static_cast<bool>(probe);
        }
        if (exists) {
            std::fprintf(stderr, "%s:0:0: file exists, refusing to overwrite\n",
                         template_path.c_str());
            return 2;
        }
        return write_document(template_path, dump_graph(starting_graph()));
    }
    if (path.empty()) {
        return usage(argv[0]);
    }
    // A cap below one is not a document, and the emitter only asserts on it in a debug build.
    if (options.max_nodes < 1) {
        std::fprintf(stderr, "graphlint_report: --max-nodes must be >= 1\n");
        return 2;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "%s:0:0: cannot read the file\n", path.c_str());
        return 2;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    Graph graph;
    GraphError error;
    if (!parse_graph(text, graph, error)) {
        std::fprintf(stderr, "%s:%d:%d: %s\n", path.c_str(), error.line, error.column,
                     error.detail.c_str());
        return 2;
    }

    // The table names the graph it measured: a refused one is a position in the table, or a
    // binding that does not hold, never a silent 1 on a number that lands in a report.
    if (!weights_path.empty()) {
        std::ifstream weights_in(weights_path, std::ios::binary);
        if (!weights_in) {
            std::fprintf(stderr, "%s:0:0: cannot read the file\n", weights_path.c_str());
            return 2;
        }
        std::ostringstream weights_buffer;
        weights_buffer << weights_in.rdbuf();
        WeightsTable table;
        if (!parse_weights(weights_buffer.str(), table, error) ||
            !apply_weights(graph, table, error)) {
            std::fprintf(stderr, "%s:%d:%d: %s\n", weights_path.c_str(), error.line, error.column,
                         error.detail.c_str());
            return 2;
        }
    }

    options.source = path;
    options.environment = local_environment();

    // Captured first so --out and stdout write the same bytes and stdout can stay empty.
    std::ostringstream document;
    const int code = tools::run_report(graph, options, document, std::cerr);
    if (code != 0) {
        return code;
    }
    if (!out_path.empty()) {
        return write_document(out_path, document.str());
    }
    const std::string bytes = document.str();
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
    return 0;
}
