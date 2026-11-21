// What a graph pays for dependencies that imply the same work: three shapes at
// three sizes, instantiate and launch, measured in one process with cuda-timer.
#include <graphlint/analysis.hpp>
#include <graphlint/cuda_source.hpp>
#include <graphlint/json.hpp>

#include <cuda_timer/timer.cuh>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using graphlint::Graph;
using graphlint::GraphError;
using graphlint::SourceKind;
using graphlint::Topology;
using graphlint::detail::append_int;
using graphlint::detail::append_json_string;

constexpr int k_iterations = 200;  // launches per timed batch
constexpr int k_repeats = 7;       // batches per recorded pass
constexpr int k_passes = 3;        // recorded passes over the nine cells
constexpr int k_warmup = 20;       // launches discarded before a series
constexpr int k_sizes[] = {8, 32, 128};

[[noreturn]] void die(const char* call, cudaError_t status) {
    std::fprintf(stderr, "graphlint bench: %s -> %s (%d)\n", call, cudaGetErrorString(status),
                 static_cast<int>(status));
    std::exit(1);
}

void check(cudaError_t status, const char* call) {
    if (status != cudaSuccess) {
        die(call, status);
    }
}

#define GL_CUDA(expr) check((expr), #expr)

// No parameters: the cost of the graph is what is being measured, not the kernel.
__global__ void noop() {}

cudaGraphNode_t add_kernel(cudaGraph_t graph) {
    cudaKernelNodeParams params{};
    params.func = reinterpret_cast<void*>(&noop);
    params.gridDim = dim3(1, 1, 1);
    params.blockDim = dim3(1, 1, 1);
    cudaGraphNode_t node = nullptr;
    GL_CUDA(cudaGraphAddKernelNode(&node, graph, nullptr, 0, &params));
    return node;
}

enum class Shape { Chain, Redundant, ForkJoin };

const char* shape_token(Shape shape) {
    switch (shape) {
        case Shape::Chain: return "chain";
        case Shape::Redundant: return "redundant";
        case Shape::ForkJoin: return "forkjoin";
    }
    return "chain";
}

// chain is i->i+1; redundant adds i->i+2, which the long way round already implies;
// forkjoin is one source, N-2 independent middles, one sink.
std::vector<std::pair<int, int>> shape_edges(Shape shape, int n) {
    std::vector<std::pair<int, int>> edges;
    if (shape == Shape::ForkJoin) {
        for (int j = 1; j < n - 1; ++j) {
            edges.emplace_back(0, j);
            edges.emplace_back(j, n - 1);
        }
        return edges;
    }
    for (int i = 0; i + 1 < n; ++i) {
        edges.emplace_back(i, i + 1);
        if (shape == Shape::Redundant && i + 2 < n) {
            edges.emplace_back(i, i + 2);
        }
    }
    return edges;
}

cudaGraph_t build(Shape shape, int n) {
    cudaGraph_t graph = nullptr;
    GL_CUDA(cudaGraphCreate(&graph, 0));
    std::vector<cudaGraphNode_t> nodes;
    nodes.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        nodes.push_back(add_kernel(graph));
    }
    for (const std::pair<int, int>& edge : shape_edges(shape, n)) {
        const cudaGraphNode_t from = nodes[static_cast<std::size_t>(edge.first)];
        const cudaGraphNode_t to = nodes[static_cast<std::size_t>(edge.second)];
        GL_CUDA(cudaGraphAddDependencies(graph, &from, &to, 1));
    }
    return graph;
}

struct Facts {
    int nodes = 0;
    int edges = 0;
    int depth = 0;
    int redundant = 0;
};

// A number measured on a graph the linter reads differently is not a number: both
// readers run here, and any disagreement is fatal rather than a warning.
Facts read_back(cudaGraph_t handle, const char* shape, int n) {
    Graph model;
    std::string error;
    if (!graphlint::from_cuda(handle, SourceKind::Built, model, error)) {
        std::fprintf(stderr, "graphlint bench: %s n=%d: from_cuda: %s\n", shape, n, error.c_str());
        std::exit(1);
    }
    Topology topo;
    GraphError problem;
    if (!graphlint::topology(model, topo, problem)) {
        std::fprintf(stderr, "graphlint bench: %s n=%d: topology: %s\n", shape, n,
                     problem.detail.c_str());
        std::exit(1);
    }
    Facts facts{};
    facts.nodes = static_cast<int>(model.nodes.size());
    facts.edges = static_cast<int>(model.edges.size());
    facts.depth = topo.depth;
    facts.redundant = static_cast<int>(topo.edges_redundant.size());
    return facts;
}

struct Expected {
    int edges = 0;
    int depth = 0;
    int redundant = 0;
};

Expected expected_facts(Shape shape, int n) {
    switch (shape) {
        case Shape::Chain: return {n - 1, n, 0};
        case Shape::Redundant: return {2 * n - 3, n, n - 2};
        case Shape::ForkJoin: return {2 * (n - 2), 3, 0};
    }
    return {};
}

// cuda_timer reports milliseconds in a float; the document carries integers.
std::int64_t to_ns(float ms) {
    return std::llround(static_cast<double>(ms) * 1.0e6);
}

// Warmup is in launches and never enters a sample: the first launches of a process
// pay module load and clock ramp, which belong to nobody's graph.
void warmup(cudaGraphExec_t exec, cudaStream_t stream) {
    for (int i = 0; i < k_warmup; ++i) {
        GL_CUDA(cudaGraphLaunch(exec, stream));
    }
    GL_CUDA(cudaStreamSynchronize(stream));
}

// One sample per batch, seven per call: per-launch event overhead would dominate a
// 1x1 noop, and a 200-launch batch is what a stream sees in a serving loop too.
void sample_launch(cudaGraphExec_t exec, cudaStream_t stream, cuda_timer::Timer& timer,
                   std::vector<std::int64_t>& samples) {
    for (int repeat = 0; repeat < k_repeats; ++repeat) {
        timer.start();
        for (int i = 0; i < k_iterations; ++i) {
            GL_CUDA(cudaGraphLaunch(exec, stream));
        }
        GL_CUDA(cudaStreamSynchronize(stream));
        samples.push_back(std::llround(static_cast<double>(to_ns(timer.stop())) / k_iterations));
    }
}

// cudaGraphInstantiate is a host call: an event pair on an idle stream collapses to ~0 when
// WDDM batches both records, so the host clock is the instrument. One exec at a time: seven
// live execs make the next instantiate pay their arena, not its own work.
void sample_instantiate(cudaGraph_t graph, std::vector<std::int64_t>& samples) {
    for (int repeat = 0; repeat < k_repeats; ++repeat) {
        cudaGraphExec_t exec = nullptr;
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        GL_CUDA(cudaGraphInstantiate(&exec, graph, 0));
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        GL_CUDA(cudaGraphExecDestroy(exec));
    }
}

struct Cell {
    Shape shape = Shape::Chain;
    Facts facts;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t exec = nullptr;
    std::vector<std::int64_t> instantiate;
    std::vector<std::int64_t> launch;
    std::vector<std::int64_t> zero_point;
    std::vector<std::int64_t> instantiate_empty;
};

// One sweep over the nine cells and the empty graph. Run once discarded, then three
// times recorded, so every cell sees the same clock and host state as its neighbours.
void measure_pass(std::vector<Cell>& cells, cudaGraph_t empty_graph, cudaGraphExec_t empty_exec,
                  cudaStream_t stream, cuda_timer::Timer& timer) {
    for (Cell& cell : cells) {
        sample_instantiate(cell.graph, cell.instantiate);
        sample_launch(cell.exec, stream, timer, cell.launch);
        sample_launch(empty_exec, stream, timer, cell.zero_point);
        sample_instantiate(empty_graph, cell.instantiate_empty);
    }
}

struct Spread {
    std::int64_t min = 0;
    std::int64_t median = 0;
    std::int64_t max = 0;
};

Spread spread_of(std::vector<std::int64_t> samples) {
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2], samples.back()};
}

struct Row {
    const char* shape = "";
    int nodes = 0;
    int edges = 0;
    int depth = 0;
    int redundant = 0;
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

void append_signed(std::string& out, std::int64_t value) {
    out += std::to_string(value);
}

// Sorted keys, integers only, LF: the same contract as every document in graphs/.
std::string dump_bench(const std::string& device, int driver, int runtime,
                       const std::vector<Row>& rows) {
    std::string out;
    out.reserve(rows.size() * 420 + 320);
    out += "{\n";

    out += "  \"compiled_runtime\": ";
    append_int(out, CUDART_VERSION);
    out += ",\n";

    out += "  \"cuda_runtime\": ";
    append_int(out, runtime);
    out += ",\n";

    out += "  \"device\": ";
    append_json_string(out, device);
    out += ",\n";

    out += "  \"driver\": ";
    append_int(out, driver);
    out += ",\n";

    out += "  \"iterations\": ";
    append_int(out, k_iterations);
    out += ",\n";

    out += "  \"passes\": ";
    append_int(out, k_passes);
    out += ",\n";

    out += "  \"repeats\": ";
    append_int(out, k_repeats);
    out += ",\n";

    out += "  \"results\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        out += "    {\"depth\": ";
        append_int(out, row.depth);
        out += ", \"edges\": ";
        append_int(out, row.edges);
        out += ", \"instantiate_empty_ns\": ";
        append_signed(out, row.instantiate_empty_ns);
        out += ", \"instantiate_max_ns\": ";
        append_signed(out, row.instantiate_max_ns);
        out += ", \"instantiate_min_ns\": ";
        append_signed(out, row.instantiate_min_ns);
        out += ", \"instantiate_net_ns\": ";
        append_signed(out, row.instantiate_net_ns);
        out += ", \"instantiate_ns\": ";
        append_signed(out, row.instantiate_ns);
        out += ", \"launch_max_ns\": ";
        append_signed(out, row.launch_max_ns);
        out += ", \"launch_min_ns\": ";
        append_signed(out, row.launch_min_ns);
        out += ", \"launch_net_ns\": ";
        append_signed(out, row.launch_net_ns);
        out += ", \"launch_ns\": ";
        append_signed(out, row.launch_ns);
        out += ", \"nodes\": ";
        append_int(out, row.nodes);
        out += ", \"redundant_edges\": ";
        append_int(out, row.redundant);
        out += ", \"shape\": ";
        append_json_string(out, row.shape);
        out += ", \"zero_point_ns\": ";
        append_signed(out, row.zero_point_ns);
        out += '}';
        out += i + 1 == rows.size() ? "\n" : ",\n";
    }
    out += "  ],\n";

    out += "  \"schema_version\": 1\n";
    out += "}\n";
    return out;
}

bool write_document(const std::filesystem::path& path, const std::string& document) {
    std::error_code failed;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), failed);
        if (failed) {
            std::fprintf(stderr, "graphlint bench: cannot create %s: %s\n",
                         path.parent_path().string().c_str(), failed.message().c_str());
            return false;
        }
    }
    // Binary: a text-mode stream would turn every LF into CRLF on a committed artifact.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "graphlint bench: cannot write %s\n", path.string().c_str());
        return false;
    }
    file.write(document.data(), static_cast<std::streamsize>(document.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "graphlint bench: short write on %s\n", path.string().c_str());
        return false;
    }
    return true;
}

std::string micro_span(std::int64_t min_ns, std::int64_t median_ns, std::int64_t max_ns) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.1f [%.1f-%.1f]", static_cast<double>(median_ns) / 1e3,
                  static_cast<double>(min_ns) / 1e3, static_cast<double>(max_ns) / 1e3);
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path destination = "docs/bench/bench.json";
    bool allow_runtime_mismatch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--out" && i + 1 < argc) {
            destination = argv[++i];
            continue;
        }
        if (argument == "--allow-runtime-mismatch") {
            allow_runtime_mismatch = true;
            continue;
        }
        std::fprintf(stderr, "usage: %s [--out <path>] [--allow-runtime-mismatch]\n", argv[0]);
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
    // `driver` is cudaDriverGetVersion: the CUDA API level the installed driver supports,
    // the only driver fact the runtime exposes (the 610.88 package version needs NVML).
    std::printf("graphlint bench | %s | driver CUDA API %d.%d\n", device_label, driver / 1000,
                (driver % 1000) / 10);
    std::printf("runtime %d.%d loaded, %d.%d compiled (CUDART_VERSION)\n", runtime / 1000,
                (runtime % 1000) / 10, CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
    if (runtime != CUDART_VERSION) {
        // A document that says 12.6 while a 12.1 DLL produced the numbers is a false
        // number in the repo, so the mismatch stops the run unless it is acknowledged.
        std::fflush(stdout);
        std::fprintf(stderr,
                     "graphlint bench: the loaded runtime is not the one this binary was "
                     "compiled against; these numbers would come from %d.%d, not %d.%d.\n"
                     "Put the matching toolkit's bin first in PATH (build-device.bat bench "
                     "does), or pass --allow-runtime-mismatch to record the run anyway.\n",
                     runtime / 1000, (runtime % 1000) / 10, CUDART_VERSION / 1000,
                     (CUDART_VERSION % 1000) / 10);
        if (!allow_runtime_mismatch) {
            return 1;
        }
        std::printf("(allowed by --allow-runtime-mismatch)\n");
    }

    cudaStream_t stream = nullptr;
    GL_CUDA(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    cuda_timer::Timer timer(stream);

    cudaGraph_t empty_graph = nullptr;
    GL_CUDA(cudaGraphCreate(&empty_graph, 0));
    const Facts empty = read_back(empty_graph, "empty", 0);
    if (empty.nodes != 0 || empty.edges != 0 || empty.depth != 0 || empty.redundant != 0) {
        std::fprintf(stderr, "graphlint bench: the empty graph is not empty\n");
        return 1;
    }
    cudaGraphExec_t empty_exec = nullptr;
    GL_CUDA(cudaGraphInstantiate(&empty_exec, empty_graph, 0));
    warmup(empty_exec, stream);

    std::vector<Cell> cells;
    cells.reserve(9);
    int cross_checked = 0;
    for (const Shape shape : {Shape::Chain, Shape::Redundant, Shape::ForkJoin}) {
        for (const int n : k_sizes) {
            Cell cell;
            cell.shape = shape;
            cell.graph = build(shape, n);
            cell.facts = read_back(cell.graph, shape_token(shape), n);
            const Expected expected = expected_facts(shape, n);
            if (cell.facts.nodes != n || cell.facts.edges != expected.edges ||
                cell.facts.depth != expected.depth || cell.facts.redundant != expected.redundant) {
                std::fprintf(stderr,
                             "graphlint bench: %s n=%d: the builder and the linter disagree "
                             "(nodes %d/%d, edges %d/%d, depth %d/%d, redundant %d/%d)\n",
                             shape_token(shape), n, cell.facts.nodes, n, cell.facts.edges,
                             expected.edges, cell.facts.depth, expected.depth, cell.facts.redundant,
                             expected.redundant);
                return 1;
            }
            ++cross_checked;
            GL_CUDA(cudaGraphInstantiate(&cell.exec, cell.graph, 0));
            warmup(cell.exec, stream);
            cell.instantiate.reserve(k_passes * k_repeats);
            cell.launch.reserve(k_passes * k_repeats);
            cell.zero_point.reserve(k_passes * k_repeats);
            cell.instantiate_empty.reserve(k_passes * k_repeats);
            cells.push_back(std::move(cell));
        }
    }

    // The first sweep is thrown away: clocks, caches and the driver's first-touch costs
    // are paid once per process, and a lone chain sample that pays them is not a baseline.
    {
        std::vector<Cell> discarded = cells;
        measure_pass(discarded, empty_graph, empty_exec, stream, timer);
    }
    for (int pass = 0; pass < k_passes; ++pass) {
        measure_pass(cells, empty_graph, empty_exec, stream, timer);
    }

    std::vector<Row> rows;
    rows.reserve(cells.size());
    for (Cell& cell : cells) {
        const Spread instantiate = spread_of(std::move(cell.instantiate));
        const Spread launch = spread_of(std::move(cell.launch));
        const Spread zero_point = spread_of(std::move(cell.zero_point));
        const Spread instantiate_empty = spread_of(std::move(cell.instantiate_empty));

        Row row{};
        row.shape = shape_token(cell.shape);
        row.nodes = cell.facts.nodes;
        row.edges = cell.facts.edges;
        row.depth = cell.facts.depth;
        row.redundant = cell.facts.redundant;
        row.zero_point_ns = zero_point.median;
        row.instantiate_empty_ns = instantiate_empty.median;
        row.instantiate_ns = instantiate.median;
        row.instantiate_min_ns = instantiate.min;
        row.instantiate_max_ns = instantiate.max;
        row.launch_ns = launch.median;
        row.launch_min_ns = launch.min;
        row.launch_max_ns = launch.max;
        // Not clamped: a negative net is what a small graph does on this GPU.
        row.instantiate_net_ns = row.instantiate_ns - row.instantiate_empty_ns;
        row.launch_net_ns = row.launch_ns - row.zero_point_ns;
        rows.push_back(row);

        GL_CUDA(cudaGraphExecDestroy(cell.exec));
        GL_CUDA(cudaGraphDestroy(cell.graph));
    }

    std::printf("%d batch x %d launches, %d passes, one discarded sweep: %d samples per cell\n",
                k_repeats, k_iterations, k_passes, k_passes * k_repeats);
    std::printf("net = median(row) - median(empty graph, same procedure), never clamped\n");
    std::printf("launch: one event pair per batch, node execution included, not submit latency\n");
    std::printf("instantiate: host wall clock around the call, the empty-graph row is its "
                "floor\n\n");

    // `nodes` is the N of the row: the three shapes share one size, so one column says it.
    std::printf("%-10s %5s %5s %5s %4s %12s %12s %22s %22s\n", "shape", "nodes", "edges",
                "depth", "red", "zero_point", "empty_inst", "inst_net us", "launch_net us");
    for (const Row& row : rows) {
        const std::string inst = micro_span(row.instantiate_min_ns - row.instantiate_empty_ns,
                                            row.instantiate_net_ns,
                                            row.instantiate_max_ns - row.instantiate_empty_ns);
        const std::string launched = micro_span(row.launch_min_ns - row.zero_point_ns,
                                                row.launch_net_ns,
                                                row.launch_max_ns - row.zero_point_ns);
        std::printf("%-10s %5d %5d %5d %4d %12lld %12lld %22s %22s\n", row.shape, row.nodes,
                    row.edges, row.depth, row.redundant,
                    static_cast<long long>(row.zero_point_ns),
                    static_cast<long long>(row.instantiate_empty_ns), inst.c_str(),
                    launched.c_str());
    }

    const std::string document = dump_bench(device_label, driver, runtime, rows);
    if (!write_document(destination, document)) {
        return 1;
    }
    std::printf("\ncross-check %d/%d rows agree with their builder (from_cuda + topology)\n",
                cross_checked, static_cast<int>(rows.size()));
    std::printf("wrote %s (%zu bytes)\n", destination.string().c_str(), document.size());

    GL_CUDA(cudaStreamDestroy(stream));
    return 0;
}
