// graphlint_report: a thin main over tools/report_core.hpp. Argument and file errors are the
// CLI's own; the exit policy that a document is 0 and a failed precondition is 2 lives in the core.
#include <graphlint/analysis.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

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
                 "usage: %s <graph.json> [--format summary|json|dot|mermaid] [--out <path>] "
                 "[--detail-params] [--max-nodes N]\n",
                 program);
    return 2;
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
    ReportOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--format" && i + 1 < argc) {
            if (!format_from_token(argv[++i], options.format)) {
                return usage(argv[0]);
            }
        } else if (argument == "--out" && i + 1 < argc) {
            out_path = argv[++i];
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
