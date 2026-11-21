// The README example: capture a real graph, read it with the library, print the report
// command that re-runs the same analysis from the file just written.
#include <graphlint/analysis.hpp>
#include <graphlint/cuda_source.hpp>
#include <graphlint/json.hpp>
#include <graphlint/rules.hpp>

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using graphlint::Finding;
using graphlint::Graph;
using graphlint::LintResult;
using graphlint::Topology;

namespace {

// 77 is the suite's "this machine has no card": ctest reports SKIPPED, never a green run.
constexpr int k_skipped = 77;

__global__ void noop() {}

void check(cudaError_t status, const char* call) {
    if (status == cudaSuccess) {
        return;
    }
    std::fprintf(stderr, "example: %s -> %s (%d)\n", call, cudaGetErrorString(status),
                 static_cast<int>(status));
    std::exit(1);
}

#define EXAMPLE_CHECK(expr) check((expr), #expr)

// stem -> {branch 0 on stream 0, branch 1 on stream 1 after one event} -> join.
// Two streams and one handshake is the smallest capture that has a fork and a join.
cudaGraph_t capture_fork_join() {
    cudaStream_t streams[2] = {};
    cudaEvent_t fork = nullptr;
    cudaEvent_t join = nullptr;
    EXAMPLE_CHECK(cudaStreamCreate(&streams[0]));
    EXAMPLE_CHECK(cudaStreamCreate(&streams[1]));
    EXAMPLE_CHECK(cudaEventCreateWithFlags(&fork, cudaEventDisableTiming));
    EXAMPLE_CHECK(cudaEventCreateWithFlags(&join, cudaEventDisableTiming));

    EXAMPLE_CHECK(cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeGlobal));
    noop<<<1, 1, 0, streams[0]>>>();  // stem
    EXAMPLE_CHECK(cudaGetLastError());
    EXAMPLE_CHECK(cudaEventRecord(fork, streams[0]));
    noop<<<1, 1, 0, streams[0]>>>();  // branch 0
    EXAMPLE_CHECK(cudaGetLastError());
    EXAMPLE_CHECK(cudaStreamWaitEvent(streams[1], fork));
    noop<<<1, 1, 0, streams[1]>>>();  // branch 1
    EXAMPLE_CHECK(cudaGetLastError());
    EXAMPLE_CHECK(cudaEventRecord(join, streams[1]));
    EXAMPLE_CHECK(cudaStreamWaitEvent(streams[0], join));
    noop<<<1, 1, 0, streams[0]>>>();  // join
    EXAMPLE_CHECK(cudaGetLastError());
    cudaGraph_t graph = nullptr;
    EXAMPLE_CHECK(cudaStreamEndCapture(streams[0], &graph));

    EXAMPLE_CHECK(cudaEventDestroy(fork));
    EXAMPLE_CHECK(cudaEventDestroy(join));
    EXAMPLE_CHECK(cudaStreamDestroy(streams[0]));
    EXAMPLE_CHECK(cudaStreamDestroy(streams[1]));
    return graph;
}

void print_summary(const Graph& graph, const Topology& topo, const LintResult& lint) {
    int width_max = 0;
    for (const int width : topo.width) {
        width_max = width > width_max ? width : width_max;
    }
    std::printf("captured: %zu nodes, %zu edges\n", graph.nodes.size(), graph.edges.size());
    std::printf("topology: depth %d levels, width_max %d\n", topo.depth, width_max);
    std::printf("reduction: %zu of %d edges redundant, minimal DAG %zu edges\n",
                topo.edges_redundant.size(), topo.distinct_edges, topo.edges_minimal.size());
    std::printf("findings: %zu\n", lint.findings.size());
    for (const Finding& finding : lint.findings) {
        std::printf("  %-6s %s", graphlint::to_string(finding.severity),
                    graphlint::to_string(finding.rule));
        if (!finding.nodes.empty()) {
            std::printf(" [");
            for (std::size_t i = 0; i < finding.nodes.size(); ++i) {
                std::printf(i == 0 ? "%d" : ", %d", finding.nodes[i]);
            }
            std::printf("]");
        }
        std::printf(": %s\n", finding.message.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--out" && i + 1 < argc) {
            path = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--out <path>]\n", argv[0]);
            return 2;
        }
    }
    if (path.empty()) {
        std::error_code failed;
        const std::filesystem::path directory = std::filesystem::temp_directory_path(failed);
        if (failed) {
            std::fprintf(stderr, "example: no temporary directory: %s\n", failed.message().c_str());
            return 1;
        }
        path = directory / "graphlint_three_lines.json";
    }

    int devices = 0;
    const cudaError_t counted = cudaGetDeviceCount(&devices);
    if (counted != cudaSuccess) {
        std::fprintf(stderr, "example: cudaGetDeviceCount -> %s: skipped\n",
                     cudaGetErrorString(counted));
        return k_skipped;
    }
    if (devices == 0) {
        std::fprintf(stderr, "example: no CUDA device on this machine: skipped\n");
        return k_skipped;
    }
    int device = 0;
    EXAMPLE_CHECK(cudaGetDevice(&device));
    cudaDeviceProp properties{};
    EXAMPLE_CHECK(cudaGetDeviceProperties(&properties, device));
    std::printf("graph-lint example | %s | CUDA runtime %d.%d\n", properties.name,
                CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);

    cudaGraph_t handle = capture_fork_join();
    Graph graph;
    std::string error;
    const bool read = graphlint::from_cuda(handle, graphlint::SourceKind::Capture, graph, error);
    EXAMPLE_CHECK(cudaGraphDestroy(handle));
    if (!read) {
        std::fprintf(stderr, "example: from_cuda failed: %s: skipped\n", error.c_str());
        return k_skipped;
    }

    Topology topo;
    graphlint::GraphError topo_error;
    if (!graphlint::topology(graph, topo, topo_error)) {
        std::fprintf(stderr, "example: topology refused the captured graph: %s\n",
                     topo_error.detail.c_str());
        return 1;
    }
    LintResult lint;
    static_cast<void>(graphlint::lint(graph, topo, topo_error, lint));
    print_summary(graph, topo, lint);

    std::error_code failed;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), failed);
        if (failed) {
            std::fprintf(stderr, "example: cannot create %s: %s\n",
                         path.parent_path().string().c_str(), failed.message().c_str());
            return 1;
        }
    }
    const std::string document = graphlint::dump_graph(graph);
    // Binary: a text-mode stream would turn the document's "\n" into CRLF.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "example: cannot open %s\n", path.string().c_str());
        return 1;
    }
    file.write(document.data(), static_cast<std::streamsize>(document.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "example: short write on %s\n", path.string().c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", path.string().c_str(), document.size());
    std::printf("report: graphlint_report \"%s\" --format summary\n", path.string().c_str());
    return 0;
}
