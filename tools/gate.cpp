// graphlint_gate: compares two report.json documents and exits with the verdict. The document
// is written even for an unreadable input, because "I cannot tell" is a verdict (suspect, 2),
// and the stderr line always carries it.
#include <graphlint/gate.hpp>
#include <graphlint/graph.hpp>
#include <graphlint/report.hpp>
#include <graphlint/rules.hpp>

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using graphlint::GateResult;
using graphlint::GateThresholds;
using graphlint::GraphError;
using graphlint::GraphErrorKind;
using graphlint::ReportDocument;
using graphlint::Verdict;

// A text-mode stdout turns every LF into CRLF on Windows, and the document is a byte contract.
void enable_binary_stdout() {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

int usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --baseline <a.json> --current <b.json> [--json <path>] "
                 "[--max-depth-increase N] [--max-redundant-increase N] "
                 "[--max-severe-increase N] [--max-path-increase-ns N] [--quiet]\n",
                 program);
    return 2;
}

bool parse_int(const char* text, int& value) {
    const std::size_t length = std::strlen(text);
    const std::from_chars_result parsed = std::from_chars(text, text + length, value);
    return parsed.ec == std::errc() && parsed.ptr == text + length;
}

bool read_file(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

// A graph.json where a report belongs fails on its first key; say so, or the reader hunts
// a schema bug that is not there.
std::string report_hint(const GraphError& error) {
    const bool graph_key =
        error.detail.find("\"children\"") != std::string::npos ||
        error.detail.find("\"edges\"") != std::string::npos ||
        error.detail.find("\"nodes\"") != std::string::npos;
    const bool graph_shape = error.kind == GraphErrorKind::UnknownKey && graph_key;
    const bool no_report = error.kind == GraphErrorKind::MissingKey &&
                           error.detail.find("\"analysis\"") != std::string::npos;
    if (graph_shape || no_report) {
        return "; graphlint_gate reads report.json, not graph.json";
    }
    return {};
}

std::string failure_reason(const std::string& side, const std::string& path,
                           const GraphError& error) {
    return side + ": " + path + ":" + std::to_string(error.line) + ":" +
           std::to_string(error.column) + ": " + error.detail + report_hint(error);
}

GateResult failure(const std::string& reason) {
    GateResult result;
    result.verdict = Verdict::suspect;
    result.reason = reason;
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace graphlint;

    enable_binary_stdout();

    std::string baseline_path;
    std::string current_path;
    std::string json_path;
    GateThresholds thresholds;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--baseline" && i + 1 < argc) {
            baseline_path = argv[++i];
        } else if (argument == "--current" && i + 1 < argc) {
            current_path = argv[++i];
        } else if (argument == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else if (argument == "--quiet") {
            quiet = true;
        } else if (argument == "--max-depth-increase" && i + 1 < argc) {
            if (!parse_int(argv[++i], thresholds.max_depth_increase)) {
                return usage(argv[0]);
            }
            thresholds.depth_given = true;
        } else if (argument == "--max-redundant-increase" && i + 1 < argc) {
            if (!parse_int(argv[++i], thresholds.max_redundant_increase)) {
                return usage(argv[0]);
            }
        } else if (argument == "--max-severe-increase" && i + 1 < argc) {
            if (!parse_int(argv[++i], thresholds.max_severe_increase)) {
                return usage(argv[0]);
            }
        } else if (argument == "--max-path-increase-ns" && i + 1 < argc) {
            if (!parse_int(argv[++i], thresholds.max_path_increase_ns)) {
                return usage(argv[0]);
            }
        } else {
            return usage(argv[0]);
        }
    }
    if (baseline_path.empty() || current_path.empty()) {
        return usage(argv[0]);
    }
    // 0 > -1 would turn a no-op comparison into a regression, so a negative threshold is
    // a usage error, not a verdict.
    if (thresholds.max_depth_increase < 0 || thresholds.max_redundant_increase < 0 ||
        thresholds.max_severe_increase < 0) {
        std::fprintf(stderr,
                     "graphlint_gate: thresholds are absolute increases and cannot be negative\n");
        return 2;
    }
    // -1 is the contract's "not asked"; below it there is no meaning to give the number.
    if (thresholds.max_path_increase_ns < -1) {
        std::fprintf(stderr,
                     "graphlint_gate: --max-path-increase-ns is an absolute increase: -1 is "
                     "\"not asked\", below it is a usage error\n");
        return 2;
    }

    // One place writes the document, so the unreadable path leaves a --json file behind too.
    const auto emit = [&](const GateResult& result) -> int {
        const std::string document = dump_gate(result, thresholds);
        if (!quiet) {
            std::fwrite(document.data(), 1, document.size(), stdout);
            std::fflush(stdout);
        }
        bool written = true;
        if (!json_path.empty()) {
            std::ofstream file(json_path, std::ios::binary | std::ios::trunc);
            if (!file) {
                std::fprintf(stderr, "graphlint_gate: cannot write %s\n", json_path.c_str());
                written = false;
            } else {
                file.write(document.data(), static_cast<std::streamsize>(document.size()));
                file.close();
                if (!file) {
                    std::fprintf(stderr, "graphlint_gate: short write on %s\n",
                                 json_path.c_str());
                    written = false;
                }
            }
        }
        // The verdict stays on stderr with --quiet and after a write failure: a log that names
        // nothing is a log nobody reads, and the sentence is the contract's fixed one.
        std::fprintf(stderr, "graphlint_gate: %s \xe2\x80\x94 %s\n", to_string(result.verdict),
                     result.reason.c_str());
        return written ? exit_code(result.verdict) : 2;
    };

    std::string baseline_text;
    if (!read_file(baseline_path, baseline_text)) {
        return emit(failure("cannot read " + baseline_path));
    }
    ReportDocument baseline_report;
    GraphError error;
    if (!parse_report(baseline_text, baseline_report, error)) {
        return emit(failure(failure_reason("baseline", baseline_path, error)));
    }

    std::string current_text;
    if (!read_file(current_path, current_text)) {
        return emit(failure("cannot read " + current_path));
    }
    ReportDocument current_report;
    if (!parse_report(current_text, current_report, error)) {
        return emit(failure(failure_reason("current", current_path, error)));
    }

    return emit(compare(baseline_report, current_report, thresholds));
}
