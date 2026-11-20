// graph-lint demo: builds four cudaGraph_t shapes, reads each one back through
// from_cuda, and writes the artifacts committed in graphs/. Every number printed
// comes from a real handle on this machine, none is computed by hand.
#include <graphlint/cuda_source.hpp>
#include <graphlint/json.hpp>

#include <cuda_runtime_api.h>

#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef GRAPHLINT_GRAPH_DIR
#  error "GRAPHLINT_GRAPH_DIR must be the graphs/ directory this demo writes"
#endif

using graphlint::dump_graph;
using graphlint::from_cuda;
using graphlint::Graph;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::SourceKind;
using graphlint::to_string;

namespace {

[[noreturn]] void die(const char* call, cudaError_t status) {
    std::fprintf(stderr, "graphlint demo: %s -> %s (%d)\n", call, cudaGetErrorString(status),
                 static_cast<int>(status));
    std::exit(1);
}

void check(cudaError_t status, const char* call) {
    if (status != cudaSuccess) {
        die(call, status);
    }
}

#define GL_CUDA(expr) check((expr), #expr)

// No parameters: nothing here comes close to cudaKernelNodeParams::kernelParams.
__global__ void noop() {}

void add_kernel_params(cudaKernelNodeParams& params) {
    params = cudaKernelNodeParams{};
    params.func = reinterpret_cast<void*>(&noop);
    params.gridDim = dim3(1, 1, 1);
    params.blockDim = dim3(1, 1, 1);
}

cudaGraphNode_t add_kernel(cudaGraph_t graph, const std::vector<cudaGraphNode_t>& deps) {
    cudaKernelNodeParams params{};
    add_kernel_params(params);
    cudaGraphNode_t node = nullptr;
    GL_CUDA(cudaGraphAddKernelNode(&node, graph, deps.empty() ? nullptr : deps.data(),
                                   deps.size(), &params));
    return node;
}

void add_dependency(cudaGraph_t graph, cudaGraphNode_t from, cudaGraphNode_t to) {
    GL_CUDA(cudaGraphAddDependencies(graph, &from, &to, 1));
}

// A) hand-built transformer block: stem, 2x1 fork, 4x3 fork, linear tail of 8.
// 27 kernel nodes, 30 dependencies (the M0 shape A).
cudaGraph_t build_built() {
    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaGraphCreate(&graph, 0));
    std::vector<cudaGraphNode_t> nodes;
    nodes.reserve(27);
    for (int i = 0; i < 27; ++i) {
        nodes.push_back(add_kernel(graph, {}));
    }
    std::vector<std::pair<int, int>> deps = {{0, 1}, {1, 2}, {1, 3}, {2, 4}, {3, 4}, {4, 5}};
    for (int branch = 0; branch < 4; ++branch) {
        const int first = 6 + 3 * branch;
        deps.insert(deps.end(), {{5, first}, {first, first + 1}, {first + 1, first + 2},
                                 {first + 2, 18}});
    }
    for (int i = 18; i < 26; ++i) {
        deps.push_back({i, i + 1});
    }
    for (const std::pair<int, int>& dep : deps) {
        add_dependency(graph, nodes[static_cast<std::size_t>(dep.first)],
                       nodes[static_cast<std::size_t>(dep.second)]);
    }
    return graph;
}

// B) the same shape captured from 4 streams: 25 launches, 6 events (M0 shape B).
cudaGraph_t build_captured() {
    cudaStream_t streams[4] = {};
    cudaEvent_t events[6] = {};
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamCreate(&stream));
    }
    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
    }

    GL_CUDA(cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeGlobal));
    noop<<<1, 1, 0, streams[0]>>>();  // stem
    GL_CUDA(cudaEventRecord(events[0], streams[0]));
    noop<<<1, 1, 0, streams[0]>>>();  // branch 0 of the 2x1 fork
    GL_CUDA(cudaStreamWaitEvent(streams[1], events[0]));
    noop<<<1, 1, 0, streams[1]>>>();  // branch 1 of the 2x1 fork
    GL_CUDA(cudaEventRecord(events[1], streams[1]));
    GL_CUDA(cudaStreamWaitEvent(streams[0], events[1]));
    noop<<<1, 1, 0, streams[0]>>>();  // immediate join
    GL_CUDA(cudaEventRecord(events[2], streams[0]));
    for (int branch = 1; branch < 4; ++branch) {
        GL_CUDA(cudaStreamWaitEvent(streams[branch], events[2]));
        for (int i = 0; i < 3; ++i) {
            noop<<<1, 1, 0, streams[branch]>>>();
        }
        GL_CUDA(cudaEventRecord(events[2 + branch], streams[branch]));
    }
    for (int i = 0; i < 3; ++i) {
        noop<<<1, 1, 0, streams[0]>>>();  // fourth branch, on stream 0
    }
    for (int branch = 1; branch < 4; ++branch) {
        GL_CUDA(cudaStreamWaitEvent(streams[0], events[2 + branch]));
    }
    noop<<<1, 1, 0, streams[0]>>>();  // join of the 4 branches
    for (int i = 0; i < 8; ++i) {
        noop<<<1, 1, 0, streams[0]>>>();  // tail
    }
    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaStreamEndCapture(streams[0], &graph));

    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventDestroy(event));
    }
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamDestroy(stream));
    }
    return graph;
}

// C) two streams passing one event back and forth: 12 launches, 12 events.
// Every node depends on i-1 by stream order and on i-2 by the event, so the
// edge set is a formula: 11 edges i->i+1 plus 10 edges i->i+2 (M0 shape D).
cudaGraph_t build_pingpong() {
    cudaStream_t streams[2] = {};
    cudaEvent_t events[12] = {};
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamCreate(&stream));
    }
    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
    }

    GL_CUDA(cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeGlobal));
    for (int i = 0; i < 12; ++i) {
        cudaStream_t current = streams[i % 2];
        if (i > 0) {
            GL_CUDA(cudaStreamWaitEvent(current, events[i - 1]));
        }
        noop<<<1, 1, 0, current>>>();
        GL_CUDA(cudaEventRecord(events[i], current));
    }
    GL_CUDA(cudaStreamWaitEvent(streams[0], events[11]));  // without this the capture does not close
    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaStreamEndCapture(streams[0], &graph));

    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventDestroy(event));
    }
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamDestroy(stream));
    }
    return graph;
}

// D) 1000 sequential launches: the only shape whose ids Kahn makes unique.
cudaGraph_t build_chain(int count) {
    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaGraphCreate(&graph, 0));
    cudaGraphNode_t previous = add_kernel(graph, {});
    for (int i = 1; i < count; ++i) {
        previous = add_kernel(graph, {previous});
    }
    return graph;
}

const char* source_token(SourceKind source) {
    switch (source) {
        case SourceKind::Capture: return "capture";
        case SourceKind::Built: return "built";
        case SourceKind::File: return "file";
    }
    return "file";
}

void print_shape(const char* name, const Graph& graph) {
    std::size_t counts[12] = {};
    for (const Node& node : graph.nodes) {
        ++counts[static_cast<std::size_t>(node.type)];
    }
    std::printf("%-8s nodes %4zu  edges %4zu  source %-7s  types:", name, graph.nodes.size(),
                graph.edges.size(), source_token(graph.source));
    for (std::size_t i = 0; i < 12; ++i) {
        if (counts[i] != 0) {
            std::printf(" %s %zu", to_string(static_cast<NodeType>(i)), counts[i]);
        }
    }
    if (!graph.children.empty()) {
        std::printf(" children %zu", graph.children.size());
    }
    std::printf("\n");
}

bool write_json(const std::filesystem::path& directory, const std::string& name,
                const Graph& graph) {
    std::error_code failed;
    std::filesystem::create_directories(directory, failed);
    if (failed) {
        std::fprintf(stderr, "graphlint demo: cannot create %s: %s\n", directory.string().c_str(),
                     failed.message().c_str());
        return false;
    }
    const std::string text = dump_graph(graph);
    const std::filesystem::path path = directory / name;
    // Binary: a text-mode stream would turn the document's "\n" into CRLF.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "graphlint demo: cannot open %s\n", path.string().c_str());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr, "graphlint demo: short write on %s\n", path.string().c_str());
        return false;
    }
    std::printf("         wrote %s (%zu bytes)\n", path.string().c_str(), text.size());
    return true;
}

int run_shape(const char* name, cudaGraph_t handle, SourceKind source,
              const std::filesystem::path& directory) {
    Graph model;
    std::string error;
    if (!from_cuda(handle, source, model, error)) {
        std::fprintf(stderr, "graphlint demo: %s: from_cuda failed: %s\n", name, error.c_str());
        return 1;
    }
    print_shape(name, model);
    return write_json(directory, std::string(name) + ".json", model) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path directory = GRAPHLINT_GRAPH_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--out" && i + 1 < argc) {
            directory = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--out <directory>]\n", argv[0]);
            return 2;
        }
    }

    int device = 0;
    GL_CUDA(cudaGetDevice(&device));
    cudaDeviceProp properties{};
    GL_CUDA(cudaGetDeviceProperties(&properties, device));
    int driver_api = 0;
    GL_CUDA(cudaDriverGetVersion(&driver_api));
    std::printf("graphlint demo | %s sm_%d%d | driver CUDA API %d.%d | runtime %d.%d\n",
                properties.name, properties.major, properties.minor, driver_api / 1000,
                (driver_api % 1000) / 10, CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
    std::printf("artifacts: %s\n\n", directory.string().c_str());

    int failures = 0;
    {
        cudaGraph_t graph = build_built();
        failures += run_shape("built", graph, SourceKind::Built, directory);
        GL_CUDA(cudaGraphDestroy(graph));
    }
    {
        cudaGraph_t graph = build_captured();
        failures += run_shape("captured", graph, SourceKind::Capture, directory);
        GL_CUDA(cudaGraphDestroy(graph));
    }
    {
        cudaGraph_t graph = build_pingpong();
        failures += run_shape("pingpong", graph, SourceKind::Capture, directory);
        GL_CUDA(cudaGraphDestroy(graph));
    }
    {
        cudaGraph_t graph = build_chain(1000);
        failures += run_shape("chain", graph, SourceKind::Built, directory);
        GL_CUDA(cudaGraphDestroy(graph));
    }

    std::printf("\n%s\n", failures == 0 ? "all four shapes written" : "FAILED");
    return failures == 0 ? 0 : 1;
}
