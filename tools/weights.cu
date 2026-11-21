// Topology to time, in one process: a chain of eight kernels, one enabled node at a time on
// the exec, and the two artifacts the report consumes. tools/bench.cu says what dependencies
// cost; this says what a node costs, which is the number depth cannot give.
#include <graphlint/analysis.hpp>
#include <graphlint/cuda_source.hpp>
#include <graphlint/json.hpp>
#include <graphlint/weights.hpp>

#include <cuda_timer/timer.cuh>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using graphlint::CriticalPath;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::detail::append_fingerprint;
using graphlint::detail::append_int;
using graphlint::detail::append_uint;

constexpr int k_nodes = 8;
// 20 launches per batch, not the bench's 200: a launch here carries tens of microseconds of
// work, so 20 of them are already a millisecond and 200 would multiply the run by ten without
// buying precision (the event pair's ~0.5 us is divided by 20).
constexpr int k_iterations = 20;
constexpr int k_repeats = 7;       // batches per recorded pass
constexpr int k_passes = 3;        // recorded passes over the nine configurations
constexpr int k_discarded = 2;     // launches thrown away after each enable/disable change
constexpr int k_ramp_launches = 600;  // full-chain launches that bring the clock to a regime
// Load per position: iters = step * (position + 1). Under the ~9 us a launch of this exec costs
// with every node disabled the work is hidden (400 and 3200 iterations read the same), so 55000
// per step puts the smallest node at ~38 us and makes a permuted handle map visible.
constexpr std::uint32_t k_iters_step = 55000;

// The chain's canonical order is forced by its topology, so the creation order is the only place
// a permutation can come from: built backwards, new_to_old is the reverse and an unpermuted map
// would arrive shuffled past the monotonic check.
constexpr int k_creation_order[k_nodes] = {7, 6, 5, 4, 3, 2, 1, 0};

[[noreturn]] void die(const char* call, cudaError_t status) {
    std::fprintf(stderr, "graphlint weights: %s -> %s (%d)\n", call, cudaGetErrorString(status),
                 static_cast<int>(status));
    std::exit(1);
}

void check(cudaError_t status, const char* call) {
    if (status != cudaSuccess) {
        die(call, status);
    }
}

#define GL_CUDA(expr) check((expr), #expr)

// A dependent integer chain, one multiply-add per step: the compiler cannot hoist it and the
// cost stays proportional to `iters` (~0.7 ns per step on sm_75 at a settled clock), so a
// position's load is a number and not a launch floor.
__global__ void work(std::uint32_t iters, std::uint32_t* sink) {
    std::uint32_t x = 1u + threadIdx.x;
    for (std::uint32_t i = 0; i < iters; ++i) {
        x = x * 1664525u + 1013904223u;
    }
    // Never true for these seeds, and not provable here: the loop keeps its body.
    if (x == 0x0badf00du) {
        sink[0] = x;
    }
}

// cuda_timer reports milliseconds in a float; the document carries integers.
std::int64_t to_ns(float ms) {
    return std::llround(static_cast<double>(ms) * 1.0e6);
}

// Enabling a subset is the measurement: the exec keeps its shape, so what moves is which work
// the launch does, not what the template says. A mask keeps the three states — one node,
// every node for the ramp, none for the zero point — in one call.
void set_enabled(cudaGraphExec_t exec, const std::vector<cudaGraphNode_t>& handles,
                 std::uint32_t mask) {
    for (int id = 0; id < k_nodes; ++id) {
        const unsigned int enabled = (mask >> id) & 1u;
        GL_CUDA(cudaGraphNodeSetEnabled(exec, handles[static_cast<std::size_t>(id)], enabled));
    }
}

// One sample per batch, the bench protocol: an event pair costs ~0.5 us, so keeping it around
// one batch rather than one launch holds the overhead under 1% of the smallest node.
void sample_batch(cudaGraphExec_t exec, cudaStream_t stream, cuda_timer::Timer& timer,
                  std::vector<std::int64_t>& samples) {
    timer.start();
    for (int i = 0; i < k_iterations; ++i) {
        GL_CUDA(cudaGraphLaunch(exec, stream));
    }
    GL_CUDA(cudaStreamSynchronize(stream));
    samples.push_back(std::llround(static_cast<double>(to_ns(timer.stop())) / k_iterations));
}

// One sweep over the eight nodes and the zero point, in id order: every node sees the same
// clock as its neighbours, which a per-node loop does not — measured, the first three read
// 1.03 ns per iteration and the rest 0.75 after the clock ramped.
void measure_pass(cudaGraphExec_t exec, const std::vector<cudaGraphNode_t>& handles,
                  cudaStream_t stream, cuda_timer::Timer& timer,
                  std::vector<std::vector<std::int64_t>>& samples) {
    for (int id = 0; id <= k_nodes; ++id) {
        // k_nodes is the zero point: every node disabled, the launch alone.
        set_enabled(exec, handles, id == k_nodes ? 0u : (1u << id));
        for (int i = 0; i < k_discarded; ++i) {
            GL_CUDA(cudaGraphLaunch(exec, stream));
        }
        GL_CUDA(cudaStreamSynchronize(stream));
        for (int repeat = 0; repeat < k_repeats; ++repeat) {
            sample_batch(exec, stream, timer, samples[static_cast<std::size_t>(id)]);
        }
    }
}

// docs/weights.md section 1: sorted keys, one row per id, integers, LF. The reader is the
// contract, not this writer, so the shape is the fixture's byte for byte.
std::string dump_weights(const Graph& graph, const std::vector<std::uint64_t>& ns) {
    std::string out;
    out.reserve(ns.size() * 16 + 160);
    out += "{\n  \"graph\": \"";
    append_fingerprint(out, graph_fingerprint(graph));
    out += "\", \"node_count\": ";
    append_int(out, static_cast<int>(ns.size()));
    out += ", \"schema_version\": 1, \"unit\": \"ns\",\n  \"weights\": [";
    for (std::size_t id = 0; id < ns.size(); ++id) {
        out += id == 0 ? "[" : ", [";
        append_int(out, static_cast<int>(id));
        out += ", ";
        append_uint(out, ns[id]);
        out += ']';
    }
    out += "]\n}\n";
    return out;
}

bool write_document(const std::filesystem::path& path, const std::string& document) {
    std::error_code failed;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), failed);
        if (failed) {
            std::fprintf(stderr, "graphlint weights: cannot create %s: %s\n",
                         path.parent_path().string().c_str(), failed.message().c_str());
            return false;
        }
    }
    // Binary: a text-mode stream would turn every LF into CRLF on a committed artifact.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "graphlint weights: cannot write %s\n", path.string().c_str());
        return false;
    }
    file.write(document.data(), static_cast<std::streamsize>(document.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "graphlint weights: short write on %s\n", path.string().c_str());
        return false;
    }
    return true;
}

bool slurp(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "graphlint weights: cannot read %s\n", path.string().c_str());
        return false;
    }
    std::string bytes;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        std::fprintf(stderr, "graphlint weights: short read on %s\n", path.string().c_str());
        return false;
    }
    text = std::move(bytes);
    return true;
}

double micros(std::int64_t ns) {
    return static_cast<double>(ns) / 1e3;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path out_dir = "docs/bench";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
            continue;
        }
        std::fprintf(stderr, "usage: %s [--out-dir <path>]\n", argv[0]);
        return 2;
    }

    int device_index = 0;
    GL_CUDA(cudaGetDevice(&device_index));
    cudaDeviceProp properties{};
    GL_CUDA(cudaGetDeviceProperties(&properties, device_index));
    int driver = 0;
    GL_CUDA(cudaDriverGetVersion(&driver));
    int runtime = 0;
    GL_CUDA(cudaRuntimeGetVersion(&runtime));

    char device_label[320];
    std::snprintf(device_label, sizeof(device_label), "%s (sm_%d%d)", properties.name,
                  properties.major, properties.minor);
    std::printf("graphlint weights | %s | driver CUDA API %d.%d\n", device_label, driver / 1000,
                (driver % 1000) / 10);
    std::printf("runtime %d.%d loaded, %d.%d compiled (CUDART_VERSION)\n", runtime / 1000,
                (runtime % 1000) / 10, CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
    if (runtime != CUDART_VERSION) {
        // A weight is a claim about a toolkit: a table written by 12.1 while the file says
        // 12.6 is a false number in the repo, so the mismatch stops the run.
        std::fflush(stdout);
        std::fprintf(stderr,
                     "graphlint weights: the loaded runtime is not the one this binary was "
                     "compiled against; these numbers would come from %d.%d, not %d.%d.\n"
                     "Put the matching toolkit's bin first in PATH (build-device.bat bench "
                     "does).\n",
                     runtime / 1000, (runtime % 1000) / 10, CUDART_VERSION / 1000,
                     (CUDART_VERSION % 1000) / 10);
        return 1;
    }

    cudaStream_t stream = nullptr;
    GL_CUDA(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    cuda_timer::Timer timer(stream);
    std::uint32_t* sink = nullptr;
    GL_CUDA(cudaMalloc(reinterpret_cast<void**>(&sink), sizeof(std::uint32_t)));

    // The parameter blocks live for the whole run: cudaGraphAddKernelNode copies the values it
    // is handed, and keeping them alive costs eight integers.
    std::vector<std::uint32_t> iters(static_cast<std::size_t>(k_nodes), 0);
    std::vector<std::array<void*, 2>> arguments(static_cast<std::size_t>(k_nodes));
    std::vector<cudaGraphNode_t> built(static_cast<std::size_t>(k_nodes), nullptr);
    for (int j = 0; j < k_nodes; ++j) {
        const std::size_t at = static_cast<std::size_t>(j);
        iters[at] = k_iters_step * static_cast<std::uint32_t>(j + 1);
        arguments[at] = {&iters[at], &sink};
    }

    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaGraphCreate(&graph, 0));
    for (const int j : k_creation_order) {
        const std::size_t at = static_cast<std::size_t>(j);
        cudaKernelNodeParams params{};
        params.func = reinterpret_cast<void*>(&work);
        // Geometry is the structural key the sort can see: distinct per position, so the
        // chain is not eight identical nodes even before the edges are added.
        params.gridDim = dim3(static_cast<unsigned int>(j) + 1, 1, 1);
        params.blockDim = dim3(32, 1, 1);
        params.kernelParams = arguments[at].data();
        GL_CUDA(cudaGraphAddKernelNode(&built[at], graph, nullptr, 0, &params));
    }
    for (int j = 0; j + 1 < k_nodes; ++j) {
        const cudaGraphNode_t from = built[static_cast<std::size_t>(j)];
        const cudaGraphNode_t to = built[static_cast<std::size_t>(j) + 1];
        GL_CUDA(cudaGraphAddDependencies(graph, &from, &to, 1));
    }

    Graph model;
    std::string error;
    std::vector<cudaGraphNode_t> handles;
    if (!graphlint::from_cuda(graph, SourceKind::Built, model, error, &handles)) {
        std::fprintf(stderr, "graphlint weights: from_cuda: %s\n", error.c_str());
        return 1;
    }
    if (model.nodes.size() != static_cast<std::size_t>(k_nodes) ||
        model.edges.size() != static_cast<std::size_t>(k_nodes - 1) ||
        handles.size() != static_cast<std::size_t>(k_nodes)) {
        std::fprintf(stderr,
                     "graphlint weights: the chain reads back as %zu nodes, %zu edges, %zu "
                     "handles\n",
                     model.nodes.size(), model.edges.size(), handles.size());
        return 1;
    }
    // The map's direct oracle, independent of the timings: each node was built with gridDim.x equal
    // to its chain position, so the handle the model calls id must report that geometry. Swapped
    // handles pass every earlier check and die here.
    for (int id = 0; id < k_nodes; ++id) {
        cudaKernelNodeParams params{};
        GL_CUDA(cudaGraphKernelNodeGetParams(handles[static_cast<std::size_t>(id)], &params));
        if (params.gridDim.x != static_cast<unsigned int>(id) + 1 || params.gridDim.y != 1u ||
            params.gridDim.z != 1u) {
            std::fprintf(stderr,
                         "graphlint weights: handle %d reports grid %ux%ux%u, while the node the "
                         "builder calls %d has %ux1x1: the id/handle map is not the one built\n",
                         id, params.gridDim.x, params.gridDim.y, params.gridDim.z, id,
                         static_cast<unsigned int>(id) + 1);
            return 1;
        }
    }
    // The monotonic check is only a map test if canonical id i is the i-th link of the chain.
    for (int id = 0; id < k_nodes; ++id) {
        if (model.nodes[static_cast<std::size_t>(id)].grid[0] !=
            static_cast<std::uint32_t>(id) + 1) {
            std::fprintf(stderr,
                         "graphlint weights: canonical id %d is not chain position %d; the "
                         "weights would be compared against the wrong load\n",
                         id, id);
            return 1;
        }
    }

    std::size_t enumerated_count = 0;
    GL_CUDA(cudaGraphGetNodes(graph, nullptr, &enumerated_count));
    std::vector<cudaGraphNode_t> enumerated(enumerated_count);
    GL_CUDA(cudaGraphGetNodes(graph, enumerated.data(), &enumerated_count));
    enumerated.resize(enumerated_count);
    std::printf("canonical id -> API enumeration index:");
    for (int id = 0; id < k_nodes; ++id) {
        const auto found = std::find(enumerated.begin(), enumerated.end(),
                                     handles[static_cast<std::size_t>(id)]);
        if (found == enumerated.end()) {
            std::fprintf(stderr, "graphlint weights: handle %d is not a node of this graph\n", id);
            return 1;
        }
        std::printf(" %d->%d", id,
                    static_cast<int>(std::distance(enumerated.begin(), found)));
    }
    std::printf("\n");

    cudaGraphExec_t exec = nullptr;
    GL_CUDA(cudaGraphInstantiate(&exec, graph, 0));

    // The clock ramps under load: a per-node pass read 27% slower per iteration on the first ids
    // than on the last. A full-chain ramp first, then round-robin passes, so drift touches every
    // id alike.
    constexpr std::uint32_t k_all_enabled = (1u << k_nodes) - 1u;
    set_enabled(exec, handles, k_all_enabled);
    for (int i = 0; i < k_ramp_launches; ++i) {
        GL_CUDA(cudaGraphLaunch(exec, stream));
    }
    GL_CUDA(cudaStreamSynchronize(stream));

    std::vector<std::vector<std::int64_t>> samples(
        static_cast<std::size_t>(k_nodes) + 1);
    for (std::vector<std::int64_t>& series : samples) {
        series.reserve(static_cast<std::size_t>(k_passes * k_repeats));
    }
    for (int pass = 0; pass < k_passes; ++pass) {
        measure_pass(exec, handles, stream, timer, samples);
    }
    std::vector<std::int64_t> medians(static_cast<std::size_t>(k_nodes) + 1, 0);
    for (std::size_t id = 0; id < samples.size(); ++id) {
        std::vector<std::int64_t>& series = samples[id];
        std::sort(series.begin(), series.end());
        medians[id] = series[series.size() / 2];
    }
    // Every node disabled: what a launch of this exec costs when it does no work. Without it
    // the weight would be the sum of a node and a launch.
    const std::int64_t zero_point = medians[static_cast<std::size_t>(k_nodes)];

    std::vector<std::uint64_t> ns(static_cast<std::size_t>(k_nodes), 0);
    std::vector<int> clamped;
    for (int id = 0; id < k_nodes; ++id) {
        const std::int64_t net = medians[static_cast<std::size_t>(id)] - zero_point;
        if (net <= 0) {
            clamped.push_back(id);
        } else {
            ns[static_cast<std::size_t>(id)] = static_cast<std::uint64_t>(net);
        }
    }

    std::printf("%d launches x %d repeats x %d passes, %d ramp launches at the start\n",
                k_iterations, k_repeats, k_passes, k_ramp_launches);
    std::printf("zero_point (all nodes disabled): %lld ns\n", static_cast<long long>(zero_point));
    std::printf("%4s %8s %12s %12s %10s\n", "id", "iters", "median ns", "weight ns", "us");
    for (int id = 0; id < k_nodes; ++id) {
        const std::size_t at = static_cast<std::size_t>(id);
        std::printf("%4d %8u %12lld %12llu %10.3f\n", id, iters[at],
                    static_cast<long long>(medians[at]),
                    static_cast<unsigned long long>(ns[at]),
                    micros(static_cast<std::int64_t>(ns[at])));
    }
    if (!clamped.empty()) {
        std::printf("clamped to 0 after the zero point: %zu of %d (", clamped.size(), k_nodes);
        for (std::size_t k = 0; k < clamped.size(); ++k) {
            std::printf("%s%d", k == 0 ? "" : ", ", clamped[k]);
        }
        std::printf("): under the timer's resolution, the file has no field to say it\n");
    }

    // The map test, as an assertion and not a glance: the load grows with the chain position,
    // so a weight that does not grow with the id means the handles went to the wrong nodes.
    for (int id = 0; id + 1 < k_nodes; ++id) {
        if (ns[static_cast<std::size_t>(id)] >= ns[static_cast<std::size_t>(id) + 1]) {
            std::fprintf(stderr,
                         "graphlint weights: weight %d (%llu ns) is not below %d (%llu ns) "
                         "while the load grows: the id/handle map is not the one measured\n",
                         id, static_cast<unsigned long long>(ns[static_cast<std::size_t>(id)]),
                         id + 1,
                         static_cast<unsigned long long>(ns[static_cast<std::size_t>(id) + 1]));
            return 1;
        }
    }

    const std::filesystem::path measured_path = out_dir / "measured.json";
    const std::filesystem::path weights_path = out_dir / "weights.json";
    const std::string measured = graphlint::dump_graph(model);
    if (!write_document(measured_path, measured) ||
        !write_document(weights_path, dump_weights(model, ns))) {
        return 1;
    }
    // The fingerprint is of the bytes on disk, and the bytes on disk are what the report reads.
    const std::uint64_t fingerprint = graph_fingerprint(model);
    std::string printed;
    append_fingerprint(printed, fingerprint);
    std::printf("wrote %s (%zu bytes) and %s (graph %s)\n", measured_path.string().c_str(),
                measured.size(), weights_path.string().c_str(), printed.c_str());

    // The invariant, checked through the files and not through the model: parse both, apply the
    // table, and ask for the path a reader would get. A mismatch here is a broken artifact.
    std::string measured_text;
    std::string weights_text;
    if (!slurp(measured_path, measured_text) || !slurp(weights_path, weights_text)) {
        return 1;
    }
    Graph reloaded;
    GraphError problem;
    if (!graphlint::parse_graph(measured_text, reloaded, problem)) {
        std::fprintf(stderr, "graphlint weights: measured.json: %s\n", problem.detail.c_str());
        return 1;
    }
    graphlint::WeightsTable table;
    if (!graphlint::parse_weights(weights_text, table, problem)) {
        std::fprintf(stderr, "graphlint weights: weights.json: %d:%d: %s\n", problem.line,
                     problem.column, problem.detail.c_str());
        return 1;
    }
    if (!graphlint::apply_weights(reloaded, table, problem)) {
        std::fprintf(stderr, "graphlint weights: apply_weights: %s\n", problem.detail.c_str());
        return 1;
    }
    Topology topo;
    if (!graphlint::topology(reloaded, topo, problem)) {
        std::fprintf(stderr, "graphlint weights: topology: %s\n", problem.detail.c_str());
        return 1;
    }
    CriticalPath path;
    if (!graphlint::critical_path(reloaded, topo, path, problem)) {
        std::fprintf(stderr, "graphlint weights: critical_path: %s\n", problem.detail.c_str());
        return 1;
    }
    std::string ids;
    for (const int id : path.nodes) {
        if (!ids.empty()) {
            ids += ' ';
        }
        append_int(ids, id);
    }
    std::printf("path: %s\n", ids.c_str());
    std::printf("path_ns: %llu\n", static_cast<unsigned long long>(path.weight_sum));
    if (path.weight_sum == 0) {
        std::fprintf(stderr,
                     "graphlint weights: the applied table gives a zero-length path, so the "
                     "two artifacts do not describe one graph\n");
        return 1;
    }

    GL_CUDA(cudaGraphExecDestroy(exec));
    GL_CUDA(cudaGraphDestroy(graph));
    GL_CUDA(cudaStreamDestroy(stream));
    GL_CUDA(cudaFree(sink));
    return 0;
}
