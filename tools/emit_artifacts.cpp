// Writes the DOT and Mermaid next to the four committed graphs/*.json: the artifacts
// the README figure and the emitter tests compare against stay regenerable on a host
// that has no GPU, because nothing here needs CUDA.
#include <graphlint/analysis.hpp>
#include <graphlint/emit_dot.hpp>
#include <graphlint/emit_mermaid.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/json.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using graphlint::Graph;
using graphlint::GraphError;
using graphlint::parse_graph;
using graphlint::to_dot;
using graphlint::to_mermaid;
using graphlint::Topology;
using graphlint::topology;

namespace {

constexpr const char* k_names[] = {"built", "captured", "pingpong", "chain"};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
    // Binary: a text-mode stream would turn every LF into CRLF on a committed artifact.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "graphlint emit: cannot open %s\n", path.string().c_str());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr, "graphlint emit: short write on %s\n", path.string().c_str());
        return false;
    }
    std::printf("%-13s %6zu bytes\n", path.filename().string().c_str(), text.size());
    return true;
}

int emit_graph(const std::filesystem::path& directory, const char* name) {
    const std::string stem = std::string(name);
    const std::filesystem::path source = directory / (stem + ".json");
    const std::string text = read_file(source);
    if (text.empty()) {
        std::fprintf(stderr, "graphlint emit: cannot read %s\n", source.string().c_str());
        return 1;
    }

    Graph graph;
    GraphError error;
    if (!parse_graph(text, graph, error)) {
        std::fprintf(stderr, "graphlint emit: %s:%d:%d %s\n", source.string().c_str(),
                     error.line, error.column, error.detail.c_str());
        return 1;
    }
    Topology topo;
    if (!topology(graph, topo, error)) {
        std::fprintf(stderr, "graphlint emit: %s: %s\n", source.string().c_str(),
                     error.detail.c_str());
        return 1;
    }

    int failures = 0;
    failures += write_file(directory / (stem + ".dot"), to_dot(graph, topo)) ? 0 : 1;
    failures += write_file(directory / (stem + ".mmd"), to_mermaid(graph, topo)) ? 0 : 1;
    return failures;
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

    std::error_code failed;
    std::filesystem::create_directories(directory, failed);
    if (failed) {
        std::fprintf(stderr, "graphlint emit: cannot create %s: %s\n",
                     directory.string().c_str(), failed.message().c_str());
        return 1;
    }

    int failures = 0;
    for (const char* name : k_names) {
        failures += emit_graph(directory, name);
    }
    return failures == 0 ? 0 : 1;
}
