// The reader on real handles: chain ids and geometry, memory nodes, a child graph,
// and the captured shape graphs/captured.json must carry (25 nodes, 29 edges).
#include <graphlint/cuda_source.hpp>
#include <graphlint/json.hpp>

#include <cuda_runtime_api.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using graphlint::dump_graph;
using graphlint::from_cuda;
using graphlint::Graph;
using graphlint::GraphError;
using graphlint::k_max_graph_depth;
using graphlint::Node;
using graphlint::NodeType;
using graphlint::parse_graph;
using graphlint::SourceKind;
using graphlint::WeightKind;

namespace {

constexpr std::array<std::uint32_t, 3> k_zero = {0, 0, 0};

// Named, because EXPECT_EQ cannot deduce a template parameter from a braced list.
std::array<std::uint32_t, 3> x_only(std::uint32_t x) {
    return {x, 1, 1};
}

__global__ void noop() {}

#define GL_CUDA(expr)                                                                     \
    do {                                                                                 \
        const cudaError_t gl_status_ = (expr);                                           \
        ASSERT_EQ(gl_status_, cudaSuccess) << #expr << ": " << cudaGetErrorString(gl_status_); \
    } while (0)

cudaError_t add_kernel(cudaGraph_t graph, const std::vector<cudaGraphNode_t>& deps, dim3 grid,
                       dim3 block, cudaGraphNode_t& node) {
    cudaKernelNodeParams params{};
    params.func = reinterpret_cast<void*>(&noop);
    params.gridDim = grid;
    params.blockDim = block;
    return cudaGraphAddKernelNode(&node, graph, deps.empty() ? nullptr : deps.data(), deps.size(),
                                  &params);
}

// One kernel leaf wrapped in `nested_children` child graph nodes.
cudaError_t nested_graph(int nested_children, cudaGraph_t& out) {
    out = nullptr;
    cudaGraph_t graph = nullptr;
    cudaError_t status = cudaGraphCreate(&graph, 0);
    cudaGraphNode_t leaf = nullptr;
    if (status == cudaSuccess) {
        status = add_kernel(graph, {}, dim3(1, 1, 1), dim3(1, 1, 1), leaf);
    }
    for (int level = 0; level < nested_children && status == cudaSuccess; ++level) {
        cudaGraph_t parent = nullptr;
        status = cudaGraphCreate(&parent, 0);
        if (status != cudaSuccess) {
            break;
        }
        cudaGraphNode_t child = nullptr;
        status = cudaGraphAddChildGraphNode(&child, parent, nullptr, 0, graph);
        if (status == cudaSuccess) {
            cudaGraphDestroy(graph);  // the parent holds a clone
            graph = parent;
        } else {
            cudaGraphDestroy(parent);
        }
    }
    if (status == cudaSuccess) {
        out = graph;
    } else if (graph != nullptr) {
        cudaGraphDestroy(graph);
    }
    return status;
}

class CudaSource : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(cudaGraphCreate(&graph_, 0), cudaSuccess);
    }

    void TearDown() override {
        if (graph_ != nullptr) {
            cudaGraphDestroy(graph_);
        }
    }

    cudaGraph_t graph_ = nullptr;
};

}  // namespace

TEST_F(CudaSource, ChainOfEightIsCanonical) {
    std::vector<cudaGraphNode_t> built;
    for (int i = 0; i < 8; ++i) {
        cudaGraphNode_t node = nullptr;
        const std::vector<cudaGraphNode_t> deps =
            built.empty() ? std::vector<cudaGraphNode_t>{} : std::vector<cudaGraphNode_t>{built.back()};
        GL_CUDA(add_kernel(graph_, deps, dim3(static_cast<unsigned int>(i) + 1, 1, 1), dim3(64, 1, 1),
                           node));
        built.push_back(node);
    }

    Graph model;
    std::string error;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, model, error)) << error;

    EXPECT_EQ(model.source, SourceKind::Built);
    EXPECT_EQ(model.weight_kind, WeightKind::Levels);
    ASSERT_EQ(model.nodes.size(), 8u);
    ASSERT_EQ(model.edges.size(), 7u);
    for (int i = 0; i < 8; ++i) {
        const Node& node = model.nodes[static_cast<std::size_t>(i)];
        EXPECT_EQ(node.id, i);
        EXPECT_EQ(node.type, NodeType::Kernel);
        EXPECT_EQ(node.grid, x_only(static_cast<std::uint32_t>(i) + 1));
        EXPECT_EQ(node.block, x_only(64));
        EXPECT_EQ(node.shared_bytes, 0u);
        EXPECT_TRUE(node.name.empty());
        if (i + 1 < 8) {
            EXPECT_EQ(model.edges[static_cast<std::size_t>(i)], std::make_pair(i, i + 1));
        }
    }

    const std::string text = dump_graph(model);
    Graph parsed;
    GraphError problem;
    ASSERT_TRUE(parse_graph(text, parsed, problem)) << problem.detail;
    EXPECT_TRUE(parsed == model);
}

// The handle map is the whole point of tools/weights.cu: id i has to name the node the model
// calls i. Created backwards and chained afterwards, so the API order is the reverse of the
// chain and a map that skipped the permutation would hand id 0 the handle of the last node.
TEST_F(CudaSource, HandlesFollowTheCanonicalIds) {
    std::vector<cudaGraphNode_t> built(8, nullptr);
    for (int j = 7; j >= 0; --j) {
        GL_CUDA(add_kernel(graph_, {}, dim3(static_cast<unsigned int>(j) + 1, 1, 1),
                           dim3(64, 1, 1), built[static_cast<std::size_t>(j)]));
    }
    for (int j = 0; j + 1 < 8; ++j) {
        const cudaGraphNode_t from = built[static_cast<std::size_t>(j)];
        const cudaGraphNode_t to = built[static_cast<std::size_t>(j) + 1];
        GL_CUDA(cudaGraphAddDependencies(graph_, &from, &to, 1));
    }

    Graph model;
    std::string error;
    std::vector<cudaGraphNode_t> handles;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, model, error, &handles)) << error;

    ASSERT_EQ(model.nodes.size(), 8u);
    ASSERT_EQ(handles.size(), 8u);
    for (int id = 0; id < 8; ++id) {
        const std::size_t at = static_cast<std::size_t>(id);
        EXPECT_EQ(model.nodes[at].grid, x_only(static_cast<std::uint32_t>(id) + 1));
        ASSERT_NE(handles[at], nullptr);
        cudaKernelNodeParams params{};
        GL_CUDA(cudaGraphKernelNodeGetParams(handles[at], &params));
        EXPECT_EQ(params.gridDim.x, static_cast<unsigned int>(id) + 1)
            << "handle " << id << " is not the node the model calls " << id;
    }

    // Without the out-parameter the model is the same one: the map is opt-in, not a second path.
    Graph unmapped;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, unmapped, error, nullptr)) << error;
    EXPECT_TRUE(unmapped == model);
}

// Two middles identical in every field canonicalize reads: their ids are one tie class, and
// the map must still be a bijection onto distinct handles.
TEST_F(CudaSource, SymmetricNodesGetDistinctHandles) {
    cudaGraphNode_t root = nullptr;
    cudaGraphNode_t left = nullptr;
    cudaGraphNode_t right = nullptr;
    cudaGraphNode_t join = nullptr;
    GL_CUDA(add_kernel(graph_, {}, dim3(1, 1, 1), dim3(32, 1, 1), root));
    GL_CUDA(add_kernel(graph_, {root}, dim3(2, 2, 1), dim3(32, 1, 1), left));
    GL_CUDA(add_kernel(graph_, {root}, dim3(2, 2, 1), dim3(32, 1, 1), right));
    GL_CUDA(add_kernel(graph_, {left, right}, dim3(4, 1, 1), dim3(32, 1, 1), join));

    Graph model;
    std::string error;
    std::vector<cudaGraphNode_t> handles;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, model, error, &handles)) << error;

    ASSERT_EQ(handles.size(), 4u);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        ASSERT_NE(handles[i], nullptr);
        for (std::size_t j = i + 1; j < handles.size(); ++j) {
            EXPECT_NE(handles[i], handles[j]) << "ids " << i << " and " << j << " share a handle";
        }
        cudaKernelNodeParams params{};
        GL_CUDA(cudaGraphKernelNodeGetParams(handles[i], &params));
        EXPECT_EQ(params.gridDim.x, model.nodes[i].grid[0]) << "id " << i;
        EXPECT_EQ(params.gridDim.y, model.nodes[i].grid[1]) << "id " << i;
    }
    // The two middles are the same node to the model, and the ids below 3 are the two of them.
    const std::array<std::uint32_t, 3> middle = {2, 2, 1};
    EXPECT_EQ(model.nodes[1].grid, middle);
    EXPECT_EQ(model.nodes[2].grid, middle);
}

TEST_F(CudaSource, MemAllocAndFreeAreReadBack) {
    int pools = 0;
    GL_CUDA(cudaDeviceGetAttribute(&pools, cudaDevAttrMemoryPoolsSupported, 0));
    if (pools == 0) {
        GTEST_SKIP() << "device without memory pools";
    }

    cudaMemAllocNodeParams alloc_params{};
    alloc_params.poolProps.allocType = cudaMemAllocationTypePinned;
    alloc_params.poolProps.location.type = cudaMemLocationTypeDevice;
    alloc_params.poolProps.location.id = 0;
    alloc_params.bytesize = 4096;
    cudaGraphNode_t alloc = nullptr;
    GL_CUDA(cudaGraphAddMemAllocNode(&alloc, graph_, nullptr, 0, &alloc_params));
    ASSERT_NE(alloc_params.dptr, nullptr);
    cudaGraphNode_t release = nullptr;
    GL_CUDA(cudaGraphAddMemFreeNode(&release, graph_, &alloc, 1, alloc_params.dptr));

    Graph model;
    std::string error;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, model, error)) << error;

    ASSERT_EQ(model.nodes.size(), 2u);
    ASSERT_EQ(model.edges.size(), 1u);
    EXPECT_EQ(model.nodes[0].type, NodeType::MemAlloc);
    EXPECT_EQ(model.nodes[0].alloc_bytes, 4096u);
    EXPECT_EQ(model.nodes[0].mem_ptr, reinterpret_cast<std::uintptr_t>(alloc_params.dptr));
    EXPECT_EQ(model.nodes[1].type, NodeType::MemFree);
    EXPECT_EQ(model.nodes[1].mem_ptr, model.nodes[0].mem_ptr);
    EXPECT_EQ(model.edges[0], std::make_pair(0, 1));

    Graph parsed;
    GraphError problem;
    ASSERT_TRUE(parse_graph(dump_graph(model), parsed, problem)) << problem.detail;
    EXPECT_TRUE(parsed == model);
}

TEST_F(CudaSource, ChildGraphIsNestedAndCanonical) {
    cudaGraph_t inner = nullptr;
    GL_CUDA(cudaGraphCreate(&inner, 0));
    cudaGraphNode_t inner_first = nullptr;
    GL_CUDA(add_kernel(inner, {}, dim3(2, 1, 1), dim3(32, 1, 1), inner_first));
    cudaGraphNode_t inner_second = nullptr;
    GL_CUDA(add_kernel(inner, {inner_first}, dim3(4, 1, 1), dim3(32, 1, 1), inner_second));

    cudaGraphNode_t stem = nullptr;
    GL_CUDA(add_kernel(graph_, {}, dim3(8, 1, 1), dim3(128, 1, 1), stem));
    cudaGraphNode_t child_node = nullptr;
    GL_CUDA(cudaGraphAddChildGraphNode(&child_node, graph_, &stem, 1, inner));
    // The node clones the graph: the handle we passed in stays ours to destroy.
    GL_CUDA(cudaGraphDestroy(inner));

    Graph model;
    std::string error;
    ASSERT_TRUE(from_cuda(graph_, SourceKind::Built, model, error)) << error;

    ASSERT_EQ(model.nodes.size(), 2u);
    EXPECT_EQ(model.nodes[0].type, NodeType::Kernel);
    EXPECT_EQ(model.nodes[1].type, NodeType::ChildGraph);
    EXPECT_EQ(model.nodes[1].child, 0);
    EXPECT_EQ(model.nodes[1].grid, k_zero);
    ASSERT_EQ(model.children.size(), 1u);
    const Graph& child = model.children[0];
    EXPECT_EQ(child.source, SourceKind::Built);
    ASSERT_EQ(child.nodes.size(), 2u);
    ASSERT_EQ(child.edges.size(), 1u);
    EXPECT_EQ(child.nodes[0].grid, x_only(2));
    EXPECT_EQ(child.nodes[1].grid, x_only(4));

    Graph parsed;
    GraphError problem;
    ASSERT_TRUE(parse_graph(dump_graph(model), parsed, problem)) << problem.detail;
    EXPECT_TRUE(parsed == model);
}

TEST(CudaDepth, MatchesTheDocumentLimit) {
    // Seven nested children are the deepest document parse_graph accepts: the
    // reader must stop at that bound, not one level later.
    cudaGraph_t at_limit = nullptr;
    GL_CUDA(nested_graph(k_max_graph_depth - 1, at_limit));
    Graph model;
    std::string error;
    const bool read = from_cuda(at_limit, SourceKind::Built, model, error);
    GL_CUDA(cudaGraphDestroy(at_limit));
    ASSERT_TRUE(read) << error;
    const std::string text = dump_graph(model);
    Graph parsed;
    GraphError problem;
    ASSERT_TRUE(parse_graph(text, parsed, problem)) << problem.detail;
    EXPECT_TRUE(parsed == model);

    cudaGraph_t too_deep = nullptr;
    GL_CUDA(nested_graph(k_max_graph_depth, too_deep));
    Graph rejected;
    error.clear();
    const bool deeper = from_cuda(too_deep, SourceKind::Built, rejected, error);
    GL_CUDA(cudaGraphDestroy(too_deep));
    EXPECT_FALSE(deeper);
    EXPECT_NE(error.find("cudaGraphChildGraphNodeGetGraph"), std::string::npos) << error;
    EXPECT_NE(error.find("deeper"), std::string::npos) << error;
}

TEST(CudaDepth, FailedReadLeavesTheHandleMapAlone) {
    // A reader that fails halfway must not leave a vector naming handles for a model the
    // caller never received: the sentinel has to survive.
    cudaGraph_t too_deep = nullptr;
    GL_CUDA(nested_graph(k_max_graph_depth, too_deep));
    Graph rejected;
    std::string error;
    std::vector<cudaGraphNode_t> handles(2, reinterpret_cast<cudaGraphNode_t>(0x1));
    const bool deeper = from_cuda(too_deep, SourceKind::Built, rejected, error, &handles);
    GL_CUDA(cudaGraphDestroy(too_deep));
    EXPECT_FALSE(deeper);
    ASSERT_EQ(handles.size(), 2u);
    EXPECT_EQ(handles[0], reinterpret_cast<cudaGraphNode_t>(0x1));
    EXPECT_EQ(handles[1], reinterpret_cast<cudaGraphNode_t>(0x1));
}

TEST(CudaCapture, MatchesTheCommittedCounts) {
    // M0 shape B, rebuilt independently of demo_graph.cu: 25 launches from four
    // streams and six events. The counts below are the builder's, not the file's.
    cudaStream_t streams[4] = {};
    cudaEvent_t events[6] = {};
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamCreate(&stream));
    }
    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
    }

    GL_CUDA(cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeGlobal));
    noop<<<1, 1, 0, streams[0]>>>();
    GL_CUDA(cudaEventRecord(events[0], streams[0]));
    noop<<<1, 1, 0, streams[0]>>>();
    GL_CUDA(cudaStreamWaitEvent(streams[1], events[0]));
    noop<<<1, 1, 0, streams[1]>>>();
    GL_CUDA(cudaEventRecord(events[1], streams[1]));
    GL_CUDA(cudaStreamWaitEvent(streams[0], events[1]));
    noop<<<1, 1, 0, streams[0]>>>();
    GL_CUDA(cudaEventRecord(events[2], streams[0]));
    for (int branch = 1; branch < 4; ++branch) {
        GL_CUDA(cudaStreamWaitEvent(streams[branch], events[2]));
        for (int i = 0; i < 3; ++i) {
            noop<<<1, 1, 0, streams[branch]>>>();
        }
        GL_CUDA(cudaEventRecord(events[2 + branch], streams[branch]));
    }
    for (int i = 0; i < 3; ++i) {
        noop<<<1, 1, 0, streams[0]>>>();
    }
    for (int branch = 1; branch < 4; ++branch) {
        GL_CUDA(cudaStreamWaitEvent(streams[0], events[2 + branch]));
    }
    noop<<<1, 1, 0, streams[0]>>>();
    for (int i = 0; i < 8; ++i) {
        noop<<<1, 1, 0, streams[0]>>>();
    }
    cudaGraph_t captured = nullptr;
    GL_CUDA(cudaStreamEndCapture(streams[0], &captured));

    Graph model;
    std::string error;
    const bool read = from_cuda(captured, SourceKind::Capture, model, error);
    for (cudaEvent_t& event : events) {
        GL_CUDA(cudaEventDestroy(event));
    }
    for (cudaStream_t& stream : streams) {
        GL_CUDA(cudaStreamDestroy(stream));
    }
    GL_CUDA(cudaGraphDestroy(captured));
    ASSERT_TRUE(read) << error;

    ASSERT_EQ(model.nodes.size(), 25u);
    ASSERT_EQ(model.edges.size(), 29u);
    std::vector<std::size_t> in_degree(model.nodes.size(), 0);
    for (const std::pair<int, int>& edge : model.edges) {
        EXPECT_EQ(model.nodes[static_cast<std::size_t>(edge.first)].type, NodeType::Kernel);
        ++in_degree[static_cast<std::size_t>(edge.second)];
    }
    std::size_t joins = 0;
    for (const Node& node : model.nodes) {
        EXPECT_EQ(node.type, NodeType::Kernel) << "node " << node.id;
    }
    for (const std::size_t degree : in_degree) {
        if (degree == 4) {
            ++joins;
        }
    }
    EXPECT_EQ(joins, 1u);
}
