#include <graphlint/cuda_source.hpp>

#include <cuda_runtime_api.h>

// The map below calls getters that exist since 11.4; one guard, one job.
#if CUDART_VERSION < 11040
#  error "graphlint reads cudaGraph_t through APIs introduced in CUDA 11.4"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace graphlint {
namespace {

[[nodiscard]] std::string cuda_failure(const char* call, cudaError_t status) {
    return std::string(call) + ": " + cudaGetErrorString(status) + " (" +
           std::to_string(static_cast<int>(status)) + ")";
}

[[nodiscard]] bool fail(std::string& error, const char* call, cudaError_t status) {
    error = cuda_failure(call, status);
    return false;
}

[[nodiscard]] bool read_graph(cudaGraph_t graph, SourceKind source, Graph& out, int depth,
                              std::string& error);

[[nodiscard]] bool fill_kernel(cudaGraphNode_t handle, Node& node, std::string& error) {
    cudaKernelNodeParams params{};
    const cudaError_t status = cudaGraphKernelNodeGetParams(handle, &params);
    if (status != cudaSuccess) {
        return fail(error, "cudaGraphKernelNodeGetParams", status);
    }
    // Geometry only. kernelParams is node-owned and carries no length: reading
    // through it would be a guess about the caller's memory.
    node.grid = {params.gridDim.x, params.gridDim.y, params.gridDim.z};
    node.block = {params.blockDim.x, params.blockDim.y, params.blockDim.z};
    node.shared_bytes = params.sharedMemBytes;
    return true;
}

[[nodiscard]] bool fill_mem_alloc(cudaGraphNode_t handle, Node& node, std::string& error) {
    cudaMemAllocNodeParams params{};
    const cudaError_t status = cudaGraphMemAllocNodeGetParams(handle, &params);
    if (status != cudaSuccess) {
        return fail(error, "cudaGraphMemAllocNodeGetParams", status);
    }
    node.alloc_bytes = params.bytesize;
    node.mem_ptr = reinterpret_cast<std::uintptr_t>(params.dptr);
    return true;
}

[[nodiscard]] bool fill_mem_free(cudaGraphNode_t handle, Node& node, std::string& error) {
    void* freed = nullptr;
    // dptr_out is declared void* but documented as "pointer to return the device
    // address" and written through (a null value returns cudaErrorInvalidValue).
    // void* holds a void**; dropping mem_ptr would make the pair uncorrelatable.
    const cudaError_t status =
        cudaGraphMemFreeNodeGetParams(handle, reinterpret_cast<void*>(&freed));
    if (status != cudaSuccess) {
        return fail(error, "cudaGraphMemFreeNodeGetParams", status);
    }
    node.mem_ptr = reinterpret_cast<std::uintptr_t>(freed);
    return true;
}

[[nodiscard]] bool read_graph(cudaGraph_t graph, SourceKind source, Graph& out, int depth,
                              std::string& error) {
    if (depth > k_max_graph_depth) {
        error = "cudaGraphChildGraphNodeGetGraph: child graphs nested deeper than " +
                std::to_string(k_max_graph_depth) + " levels";
        return false;
    }

    std::size_t node_count = 0;
    cudaError_t status = cudaGraphGetNodes(graph, nullptr, &node_count);
    if (status != cudaSuccess) {
        return fail(error, "cudaGraphGetNodes", status);
    }
    std::vector<cudaGraphNode_t> handles(node_count);
    if (node_count != 0) {
        status = cudaGraphGetNodes(graph, handles.data(), &node_count);
        if (status != cudaSuccess) {
            return fail(error, "cudaGraphGetNodes", status);
        }
        handles.resize(node_count);  // the API rewrites the count it filled
    }

    std::size_t edge_count = 0;
    status = cudaGraphGetEdges(graph, nullptr, nullptr, &edge_count);
    if (status != cudaSuccess) {
        return fail(error, "cudaGraphGetEdges", status);
    }
    std::vector<cudaGraphNode_t> edge_from(edge_count);
    std::vector<cudaGraphNode_t> edge_to(edge_count);
    if (edge_count != 0) {
        status = cudaGraphGetEdges(graph, edge_from.data(), edge_to.data(), &edge_count);
        if (status != cudaSuccess) {
            return fail(error, "cudaGraphGetEdges", status);
        }
        edge_from.resize(edge_count);
        edge_to.resize(edge_count);
    }

    out.nodes.assign(handles.size(), Node{});
    out.children.clear();
    std::unordered_map<cudaGraphNode_t, int> index;
    index.reserve(handles.size() * 2 + 1);

    for (std::size_t i = 0; i < handles.size(); ++i) {
        Node& node = out.nodes[i];
        node.id = static_cast<int>(i);
        cudaGraphNodeType type = cudaGraphNodeTypeKernel;
        status = cudaGraphNodeGetType(handles[i], &type);
        if (status != cudaSuccess) {
            return fail(error, "cudaGraphNodeGetType", status);
        }
        index.emplace(handles[i], static_cast<int>(i));

        switch (type) {
            case cudaGraphNodeTypeKernel:
                node.type = NodeType::Kernel;
                if (!fill_kernel(handles[i], node, error)) {
                    return false;
                }
                break;
            case cudaGraphNodeTypeMemcpy:
                node.type = NodeType::Memcpy;
                break;
            case cudaGraphNodeTypeMemset:
                node.type = NodeType::Memset;
                break;
            case cudaGraphNodeTypeHost:
                node.type = NodeType::Host;
                break;
            case cudaGraphNodeTypeGraph: {
                node.type = NodeType::ChildGraph;
                cudaGraph_t child = nullptr;
                status = cudaGraphChildGraphNodeGetGraph(handles[i], &child);
                if (status != cudaSuccess) {
                    return fail(error, "cudaGraphChildGraphNodeGetGraph", status);
                }
                if (child == nullptr) {
                    error = "cudaGraphChildGraphNodeGetGraph: returned a null graph";
                    return false;
                }
                out.children.emplace_back();
                node.child = static_cast<int>(out.children.size()) - 1;
                if (!read_graph(child, source, out.children.back(), depth + 1, error)) {
                    return false;
                }
                break;
            }
            case cudaGraphNodeTypeEmpty:
                node.type = NodeType::Empty;
                break;
            case cudaGraphNodeTypeWaitEvent:
                node.type = NodeType::EventWait;
                break;
            case cudaGraphNodeTypeEventRecord:
                node.type = NodeType::EventRecord;
                break;
            case cudaGraphNodeTypeExtSemaphoreSignal:
            case cudaGraphNodeTypeExtSemaphoreWait:
                node.type = NodeType::ExtSemaphore;
                break;
            case cudaGraphNodeTypeMemAlloc:
                node.type = NodeType::MemAlloc;
                if (!fill_mem_alloc(handles[i], node, error)) {
                    return false;
                }
                break;
            case cudaGraphNodeTypeMemFree:
                node.type = NodeType::MemFree;
                if (!fill_mem_free(handles[i], node, error)) {
                    return false;
                }
                break;
            default:
                // 12.6's Conditional (0x0d) lands here on purpose: no rule needs it.
                node.type = NodeType::Unknown;
                break;
        }
    }

    out.edges.clear();
    out.edges.reserve(edge_from.size());
    for (std::size_t i = 0; i < edge_from.size(); ++i) {
        const auto from = index.find(edge_from[i]);
        const auto to = index.find(edge_to[i]);
        if (from == index.end() || to == index.end()) {
            error = "cudaGraphGetEdges: an endpoint is not among the nodes cudaGraphGetNodes returned";
            return false;
        }
        out.edges.emplace_back(from->second, to->second);
    }

    // A handle carries no timings and no provenance: levels, and the caller's kind.
    out.weight_kind = WeightKind::Levels;
    out.source = source;
    return true;
}

}  // namespace

bool from_cuda(const cudaGraph_t& graph, SourceKind source, Graph& out, std::string& error) {
    Graph scratch;
    error.clear();
    if (!read_graph(graph, source, scratch, 1, error)) {
        return false;
    }
    canonicalize(scratch);
    GraphError problem;
    if (!validate(scratch, problem)) {
        error = "validate: " + problem.detail;
        return false;
    }
    out = std::move(scratch);
    return true;
}

}  // namespace graphlint
