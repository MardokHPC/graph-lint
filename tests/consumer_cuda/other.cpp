// A second translation unit includes the reader on purpose: a definition that is not
// inline would be emitted twice and fail at link time, which is the point of a
// header-only package.
#include <graphlint/cuda_source.hpp>

#include <string>

std::string consumer_cuda_document_from_other_tu() {
    return graphlint::dump_graph(graphlint::Graph{});
}
