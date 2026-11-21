#pragma once

#include <graphlint/graph.hpp>

#include <cuda_runtime_api.h>

#include <string>
#include <vector>

namespace graphlint {

// The only public header that names a CUDA type: <cuda_runtime_api.h> and not
// <cuda_runtime.h>, which drags in device-side declarations a host TU cannot use.
// `handles_by_id` receives one handle per canonical id: root only, untouched on failure.
[[nodiscard]] bool from_cuda(const cudaGraph_t& graph, SourceKind source, Graph& out,
                             std::string& error,
                             std::vector<cudaGraphNode_t>* handles_by_id = nullptr);

}  // namespace graphlint
