// graph-lint M0 spike: measures real cudaGraph_t shapes and evaluates two rules.
// Every printed number comes from a real CUDA graph, none is computed by hand.
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CUDA_CHECK(expr)                                                     \
  do {                                                                       \
    cudaError_t e_ = (expr);                                                 \
    if (e_ != cudaSuccess) {                                                 \
      std::printf("CUDA ERROR %s:%d %s -> %s\n", __FILE__, __LINE__, #expr,  \
                  cudaGetErrorString(e_));                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

// type: 0 kernel, 1 empty, 2 event, 3 mem
struct GNode { int id = 0; int type = 0; };
struct G { std::vector<GNode> nodes; std::vector<std::pair<int, int>> edges; };
struct Topo { std::vector<int> level, width; int depth = 0; };

static Topo topology(const G& g) {
  const int n = (int)g.nodes.size();
  std::vector<std::vector<int>> out(n);
  std::vector<int> indeg(n, 0);
  for (const auto& e : g.edges) { out[e.first].push_back(e.second); ++indeg[e.second]; }
  Topo t; t.level.assign(n, 0);
  std::queue<int> q;
  for (int i = 0; i < n; ++i) if (!indeg[i]) q.push(i);
  int seen = 0;
  while (!q.empty()) {
    const int u = q.front(); q.pop(); ++seen;
    for (int v : out[u]) { t.level[v] = std::max(t.level[v], t.level[u] + 1); if (--indeg[v] == 0) q.push(v); }
  }
  // A cycle here means the reader is broken, not that the graph is odd.
  if (seen != n) { std::printf("ERROR: cycle or unreachable nodes\n"); std::exit(2); }
  for (int i = 0; i < n; ++i) t.depth = std::max(t.depth, t.level[i] + 1);
  t.width.assign(t.depth, 0);
  for (int i = 0; i < n; ++i) ++t.width[t.level[i]];
  return t;
}

// Indices of the edges with an alternative path: this is the transitive reduction.
static std::vector<int> redundant_edges(const G& g) {
  const int n = (int)g.nodes.size();
  std::vector<std::vector<int>> out(n);
  for (const auto& e : g.edges) out[e.first].push_back(e.second);
  std::vector<char> vis(n, 0);
  std::vector<int> red;
  for (int ei = 0; ei < (int)g.edges.size(); ++ei) {
    const int u = g.edges[ei].first, v = g.edges[ei].second;
    std::fill(vis.begin(), vis.end(), 0); std::vector<int> st{u}; vis[u] = 1; bool hit = false;
    while (!st.empty() && !hit) {
      const int x = st.back(); st.pop_back();
      for (int y : out[x]) {
        if (x == u && y == v) continue;  // this edge is excluded, not the node
        if (y == v) { hit = true; break; }
        if (!vis[y]) { vis[y] = 1; st.push_back(y); }
      }
    }
    if (hit) red.push_back(ei);
  }
  return red;
}

struct Fork { int u = -1, j = -1; std::vector<int> sizes; };

// pointless-fork: u with >=2 successors all converging in j, branches of <2 nodes.
static std::vector<Fork> pointless_forks(const G& g) {
  const int n = (int)g.nodes.size();
  std::vector<std::vector<int>> out(n), in(n);
  for (const auto& e : g.edges) { out[e.first].push_back(e.second); in[e.second].push_back(e.first); }
  std::vector<Fork> found;
  std::vector<char> toward(n, 0), vis(n, 0); std::vector<std::vector<char>> reach;
  for (int u = 0; u < n; ++u) {
    const int k = (int)out[u].size();
    if (k < 2) continue;
    reach.assign(k, std::vector<char>(n, 0));
    for (int b = 0; b < k; ++b) {
      std::vector<int> st{out[u][b]}; reach[b][out[u][b]] = 1;
      while (!st.empty()) {
        const int x = st.back(); st.pop_back();
        for (int y : out[x]) if (!reach[b][y]) { reach[b][y] = 1; st.push_back(y); }
      }
      reach[b][u] = 0;
    }
    Fork best; int best_sum = 1 << 30;
    for (int j = 0; j < n; ++j) {
      bool common = j != u;
      for (int b = 0; b < k && common; ++b) common = reach[b][j] != 0;
      if (!common) continue;
      std::fill(toward.begin(), toward.end(), 0);  // u..j corridor: bounds the branch
      toward[j] = 1;
      for (std::vector<int> back{j}; !back.empty();) {
        const int x = back.back(); back.pop_back();
        for (int y : in[x]) if (!toward[y]) { toward[y] = 1; back.push_back(y); }
      }
      std::vector<int> sizes(k, 0); bool ok = true; int sum = 0;
      for (int b = 0; b < k && ok; ++b) {
        std::fill(vis.begin(), vis.end(), 0); std::vector<int> st{out[u][b]}; vis[out[u][b]] = 1; int cnt = 0;
        while (!st.empty()) {
          const int x = st.back(); st.pop_back();
          if (x == j || x == u || !toward[x]) continue;
          ++cnt;
          for (int y : out[x]) if (!vis[y]) { vis[y] = 1; st.push_back(y); }
        }
        sizes[b] = cnt; sum += cnt;
        if (cnt >= 2) ok = false;
      }
      if (ok && sum < best_sum) { best_sum = sum; best = Fork{u, j, sizes}; }
    }
    if (best.u >= 0) found.push_back(best);
  }
  return found;
}

// A node id is its index in the list returned by cudaGraphGetNodes.
static G from_cuda(cudaGraph_t graph) {
  size_t nn = 0, ne = 0;
  CUDA_CHECK(cudaGraphGetNodes(graph, nullptr, &nn));
  std::vector<cudaGraphNode_t> handles(nn);
  if (nn > 0) CUDA_CHECK(cudaGraphGetNodes(graph, handles.data(), &nn));
  G g; g.nodes.resize(nn);
  std::unordered_map<cudaGraphNode_t, int> index;  // handle -> index in the vector
  index.reserve(nn * 2 + 1);
  for (size_t i = 0; i < nn; ++i) {
    cudaGraphNodeType type;
    CUDA_CHECK(cudaGraphNodeGetType(handles[i], &type));
    int k = 3;
    if (type == cudaGraphNodeTypeEmpty) k = 1;
    else if (type == cudaGraphNodeTypeEventRecord || type == cudaGraphNodeTypeWaitEvent) k = 2;
    else if (type == cudaGraphNodeTypeKernel) k = 0;
    g.nodes[i] = {int(i), k};
    index[handles[i]] = int(i);
  }
  CUDA_CHECK(cudaGraphGetEdges(graph, nullptr, nullptr, &ne));
  std::vector<cudaGraphNode_t> from(ne), to(ne);
  if (ne > 0) CUDA_CHECK(cudaGraphGetEdges(graph, from.data(), to.data(), &ne));
  for (size_t i = 0; i < ne; ++i) {
    const auto a = index.find(from[i]), b = index.find(to[i]);
    if (a == index.end() || b == index.end()) { std::printf("ERROR: edge on unknown handle\n"); std::exit(3); }
    g.edges.emplace_back(a->second, b->second);
  }
  return g;
}

struct Row { std::string name; bool launch_shaped = false; int forks = 0; bool measured = true; };

static Row report(const std::string& title, const G& g, const std::string& extra = std::string()) {
  Row row;
  row.name = title.substr(0, title.find(':'));
  int nk = 0, nempty = 0, nev = 0, nmem = 0;
  for (const auto& n : g.nodes) {
    if (n.type == 0) ++nk; else if (n.type == 1) ++nempty; else if (n.type == 2) ++nev; else ++nmem;
  }
  const int nn = (int)g.nodes.size(), ne = (int)g.edges.size();
  const Topo t = topology(g);
  const std::vector<int> red = redundant_edges(g);
  const double ratio = nn > 0 ? (double)t.depth / (double)nn : 0.0;
  std::printf("=== %s ===\n", title.c_str());
  if (!extra.empty()) std::printf("%s\n", extra.c_str());
  std::printf("nodes %d  edges %d  kernel %d  empty %d  event %d", nn, ne, nk, nempty, nev);
  if (nmem > 0) std::printf("  mem %d", nmem);
  std::printf("\n");
  if (std::getenv("GL_DUMP_EDGES")) for (const auto& e : g.edges) std::printf("  edge %d -> %d\n", e.first, e.second);
  std::printf("depth %d  levels width:", t.depth);
  for (int i = 0; i < std::min<int>(12, t.depth); ++i) std::printf(" %d", t.width[i]);
  if (t.depth > 12) std::printf(" ...");  // ASCII: the Windows console uses cp850
  std::printf("\nparallelism %.2f\n", (double)nn / (double)t.depth);
  std::printf("minimal DAG: %d -> %d (%d redundant edges)\n", ne, ne - (int)red.size(), (int)red.size());
  if (!red.empty()) {
    std::printf("  redundant:");
    for (int i = 0; i < std::min<int>(8, (int)red.size()); ++i) std::printf(" %d->%d", g.edges[red[i]].first, g.edges[red[i]].second);
    if ((int)red.size() > 8) std::printf(" ...");
    std::printf("\n");
  }
  row.launch_shaped = nn >= 16 && ratio >= 0.9;
  std::printf("RULE launch-shaped : %s (depth %d / nodes %d = %.2f)\n", row.launch_shaped ? "FIRED " : "silent", t.depth, nn, ratio);
  const std::vector<Fork> forks = pointless_forks(g);
  row.forks = (int)forks.size();
  if (forks.empty()) { std::printf("RULE pointless-fork: silent\n\n"); return row; }
  std::printf("RULE pointless-fork: FIRED  (%d)\n", (int)forks.size());
  for (size_t i = 0; i < forks.size() && i < 6; ++i) {
    std::printf("  u=%d j=%d succ:", forks[i].u, forks[i].j);
    for (const auto& e : g.edges) if (e.first == forks[i].u) std::printf(" %d", e.second);
    std::printf(" branch sizes:"); for (int s : forks[i].sizes) std::printf(" %d", s);
    std::printf("\n");
  } std::printf("\n");
  return row;
}

static float* g_dev = nullptr;
static void* g_kernel_args[1] = {reinterpret_cast<void*>(&g_dev)};  // the params are not copied

__global__ void noop(float* p) { if (p) p[0] = 1.0f; }

static cudaGraphNode_t add_kernel(cudaGraph_t graph, const std::vector<cudaGraphNode_t>& deps) {
  cudaKernelNodeParams kp{};
  kp.func = reinterpret_cast<void*>(&noop);
  kp.gridDim = dim3(1, 1, 1);
  kp.blockDim = dim3(1, 1, 1);
  kp.kernelParams = g_kernel_args;
  cudaGraphNode_t node = nullptr;
  CUDA_CHECK(cudaGraphAddKernelNode(&node, graph, deps.empty() ? nullptr : deps.data(), deps.size(), &kp));
  return node;
}

static void add_dep(cudaGraph_t graph, cudaGraphNode_t from, cudaGraphNode_t to) {
  CUDA_CHECK(cudaGraphAddDependencies(graph, &from, &to, 1));
}

// A) hand-built transformer block: stem, 2x1 fork, 4x3 fork, linear tail of 8.
static cudaGraph_t build_hand() {
  cudaGraph_t graph = nullptr; CUDA_CHECK(cudaGraphCreate(&graph, 0));
  std::vector<cudaGraphNode_t> n;
  for (int i = 0; i < 27; ++i) n.push_back(add_kernel(graph, {}));
  std::vector<std::pair<int, int>> ed{{0, 1}, {1, 2}, {1, 3}, {2, 4}, {3, 4}, {4, 5}};
  for (int b = 0; b < 4; ++b) {
    const int a = 6 + 3 * b;
    ed.insert(ed.end(), {{5, a}, {a, a + 1}, {a + 1, a + 2}, {a + 2, 18}});
  }
  for (int i = 18; i < 26; ++i) ed.push_back({i, i + 1});
  for (const auto& e : ed) add_dep(graph, n[e.first], n[e.second]);
  return graph;
}

// B) the same shape captured from 4 streams: s0 coordinates and carries a branch too.
static cudaGraph_t build_capture(int* kernels, int* empties) {
  cudaStream_t s[4]; for (auto& x : s) CUDA_CHECK(cudaStreamCreate(&x));
  cudaEvent_t ev[6]; for (auto& e : ev) CUDA_CHECK(cudaEventCreateWithFlags(&e, cudaEventDisableTiming));
  CUDA_CHECK(cudaStreamBeginCapture(s[0], cudaStreamCaptureModeGlobal));
  noop<<<1, 1, 0, s[0]>>>(g_dev);  // stem
  CUDA_CHECK(cudaEventRecord(ev[0], s[0]));
  noop<<<1, 1, 0, s[0]>>>(g_dev);  // branch 0 of the 2x1 fork
  CUDA_CHECK(cudaStreamWaitEvent(s[1], ev[0]));
  noop<<<1, 1, 0, s[1]>>>(g_dev);  // branch 1 of the 2x1 fork
  CUDA_CHECK(cudaEventRecord(ev[1], s[1]));
  CUDA_CHECK(cudaStreamWaitEvent(s[0], ev[1]));
  noop<<<1, 1, 0, s[0]>>>(g_dev);  // immediate join
  CUDA_CHECK(cudaEventRecord(ev[2], s[0]));
  for (int b = 1; b < 4; ++b) {
    CUDA_CHECK(cudaStreamWaitEvent(s[b], ev[2]));
    for (int i = 0; i < 3; ++i) noop<<<1, 1, 0, s[b]>>>(g_dev);
    CUDA_CHECK(cudaEventRecord(ev[2 + b], s[b]));
  }
  for (int i = 0; i < 3; ++i) noop<<<1, 1, 0, s[0]>>>(g_dev);  // fourth branch, on s0
  for (int b = 1; b < 4; ++b) CUDA_CHECK(cudaStreamWaitEvent(s[0], ev[2 + b]));
  noop<<<1, 1, 0, s[0]>>>(g_dev);                             // join of the 4 branches
  for (int i = 0; i < 8; ++i) noop<<<1, 1, 0, s[0]>>>(g_dev);  // tail
  cudaGraph_t graph = nullptr;
  CUDA_CHECK(cudaStreamEndCapture(s[0], &graph));
  const G model = from_cuda(graph);
  *kernels = 0;
  *empties = 0;
  for (const auto& n : model.nodes) { if (n.type == 0) ++*kernels; if (n.type == 1) ++*empties; }
  for (auto& e : ev) CUDA_CHECK(cudaEventDestroy(e));
  for (auto& x : s) CUDA_CHECK(cudaStreamDestroy(x));
  return graph;
}

// C) linear chain: the extreme case of depth.
static cudaGraph_t build_chain(int count) {
  cudaGraph_t graph = nullptr; CUDA_CHECK(cudaGraphCreate(&graph, 0));
  cudaGraphNode_t prev = add_kernel(graph, {});
  for (int i = 1; i < count; ++i) prev = add_kernel(graph, {prev});
  return graph;
}

// D) fake overlap: two streams passing the event back and forth.
// If the machine refuses the capture, return nullptr and let the exact error be printed.
static cudaGraph_t build_pingpong(std::string* err) {
  cudaStream_t s[2] = {nullptr, nullptr}; for (auto& x : s) CUDA_CHECK(cudaStreamCreate(&x));
  cudaEvent_t ev[12]; for (auto& e : ev) CUDA_CHECK(cudaEventCreateWithFlags(&e, cudaEventDisableTiming));
  auto fail = [&](const char* what, cudaError_t e) {
    *err = std::string(what) + " -> " + cudaGetErrorString(e);
    for (auto& x : ev) cudaEventDestroy(x);
    for (auto& x : s) cudaStreamDestroy(x);
    cudaGetLastError();  // the capture error stays pending: it has to be consumed
    return (cudaGraph_t) nullptr;
  };
  CUDA_CHECK(cudaStreamBeginCapture(s[0], cudaStreamCaptureModeGlobal));
  for (int i = 0; i < 12; ++i) {
    cudaStream_t cur = s[i % 2];
    if (i > 0) {
      const cudaError_t we = cudaStreamWaitEvent(cur, ev[i - 1]);
      if (we != cudaSuccess) return fail("cudaStreamWaitEvent", we);
    }
    noop<<<1, 1, 0, cur>>>(g_dev);
    const cudaError_t re = cudaEventRecord(ev[i], cur);
    if (re != cudaSuccess) return fail("cudaEventRecord", re);
  }
  CUDA_CHECK(cudaStreamWaitEvent(s[0], ev[11]));  // without this join the capture does not close
  cudaGraph_t graph = nullptr;
  const cudaError_t end = cudaStreamEndCapture(s[0], &graph);
  if (end != cudaSuccess) return fail("cudaStreamEndCapture", end);
  for (auto& e : ev) CUDA_CHECK(cudaEventDestroy(e));
  for (auto& x : s) CUDA_CHECK(cudaStreamDestroy(x));
  return graph;
}

int main() {
  cudaDeviceProp prop{}; int driver_api = 0;
  CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
  CUDA_CHECK(cudaDriverGetVersion(&driver_api));
  CUDA_CHECK(cudaMalloc(&g_dev, sizeof(float)));
  std::printf("graph-lint M0 spike | %s sm_%d%d | driver CUDA API %d.%d | runtime %d.%d\n\n", prop.name,
              prop.major, prop.minor, driver_api / 1000, (driver_api % 1000) / 10, CUDART_VERSION / 1000,
              (CUDART_VERSION % 1000) / 10);
  std::vector<Row> rows;
  { cudaGraph_t g = build_hand(); rows.push_back(report("A: hand-built transformer block", from_cuda(g))); CUDA_CHECK(cudaGraphDestroy(g)); }
  {
    int kernels = 0, empties = 0;
    cudaGraph_t graph = build_capture(&kernels, &empties);
    char extra[160];
    std::snprintf(extra, sizeof(extra), "capture: %d real kernels -> %d nodes (%d Empty added, +%.0f%%)",
                  kernels, kernels + empties, empties, 100.0 * empties / (kernels + empties));
    rows.push_back(report("B: same shape captured from 4 streams", from_cuda(graph), extra));
    CUDA_CHECK(cudaGraphDestroy(graph));
  }
  { cudaGraph_t g = build_chain(1000); rows.push_back(report("C: hand-built chain of 1000 kernels", from_cuda(g))); CUDA_CHECK(cudaGraphDestroy(g)); }
  {
    std::string err;
    cudaGraph_t graph = build_pingpong(&err);
    if (graph == nullptr) {
      std::printf("=== D: fake overlap (captured, 2 streams) ===\nNOT MEASURED: %s\n\n", err.c_str());
      Row r; r.name = "D"; r.measured = false; rows.push_back(r);
    } else {
      rows.push_back(report("D: fake overlap (captured, 2 streams)", from_cuda(graph)));
      CUDA_CHECK(cudaGraphDestroy(graph));
    }
  }
  std::printf("=== VERDICT ===\n");
  int ls = 0, pf = 0; for (const auto& r : rows) { ls += (r.measured && r.launch_shaped) ? 1 : 0; pf += (r.measured && r.forks > 0) ? 1 : 0; }
  std::printf("launch-shaped  : fires on %d/4 shapes  (", ls);
  for (const auto& r : rows) if (r.measured && r.launch_shaped) std::printf("%s ", r.name.c_str());
  std::printf(")\npointless-fork : fires on %d/4 shapes  (", pf);
  for (const auto& r : rows) if (r.measured && r.forks > 0) std::printf("%s ", r.name.c_str());
  std::printf(")\n");
  CUDA_CHECK(cudaFree(g_dev));
  return 0;
}
