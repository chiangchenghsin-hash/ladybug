#include "common/max_flow.h"

#include "common/exception/binder.h"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

using namespace lbug::common;

namespace lbug {
namespace algo_extension {

namespace {

struct FlowEdge {
    uint64_t to;
    uint64_t rev; // index of the reverse edge in the other endpoint's list
    double cap;
};

struct Dinic {
    std::vector<std::vector<FlowEdge>> graph;
    std::vector<int64_t> level;
    std::vector<size_t> it;

    explicit Dinic(uint64_t n) : graph(n), level(n), it(n) {}

    void addEdge(uint64_t from, uint64_t to, double cap) {
        if (from == to || cap <= 0.0) {
            return;
        }
        FlowEdge forward{to, static_cast<uint64_t>(graph[to].size()), cap};
        FlowEdge back{from, static_cast<uint64_t>(graph[from].size()), 0.0};
        graph[from].push_back(forward);
        graph[to].push_back(back);
    }

    bool bfs(uint64_t s, uint64_t t) {
        std::fill(level.begin(), level.end(), -1);
        std::queue<uint64_t> q;
        level[s] = 0;
        q.push(s);
        while (!q.empty()) {
            const auto u = q.front();
            q.pop();
            for (const auto& e : graph[u]) {
                if (e.cap > 1e-15 && level[e.to] == -1) {
                    level[e.to] = level[u] + 1;
                    q.push(e.to);
                }
            }
        }
        return level[t] != -1;
    }

    double dfs(uint64_t u, uint64_t t, double pushed) {
        if (u == t) {
            return pushed;
        }
        for (auto& i = it[u]; i < graph[u].size(); ++i) {
            auto& e = graph[u][i];
            if (e.cap > 1e-15 && level[e.to] == level[u] + 1) {
                const double tr = dfs(e.to, t, std::min(pushed, e.cap));
                if (tr > 1e-15) {
                    e.cap -= tr;
                    graph[e.to][e.rev].cap += tr;
                    return tr;
                }
            }
        }
        return 0.0;
    }

    double maxFlow(uint64_t s, uint64_t t) {
        double flow = 0.0;
        while (bfs(s, t)) {
            std::fill(it.begin(), it.end(), 0);
            double f = 0.0;
            while ((f = dfs(s, t, 1e300)) > 1e-15) {
                flow += f;
            }
        }
        return flow;
    }

    // Nodes reachable from `s` in the residual graph (source side of a min cut).
    void reachableFromSource(uint64_t s, std::vector<uint8_t>& side) {
        std::fill(level.begin(), level.end(), -1);
        std::queue<uint64_t> q;
        level[s] = 0;
        q.push(s);
        while (!q.empty()) {
            const auto u = q.front();
            q.pop();
            for (const auto& e : graph[u]) {
                if (e.cap > 1e-15 && level[e.to] == -1) {
                    level[e.to] = 0;
                    q.push(e.to);
                }
            }
        }
        side.assign(graph.size(), 0);
        for (size_t u = 0; u < graph.size(); ++u) {
            side[u] = (level[u] != -1) ? 1 : 0;
        }
    }
};

} // namespace

double computeMaxFlow(uint64_t numNodes,
    const std::vector<std::pair<std::pair<uint64_t, uint64_t>, double>>& arcs, uint64_t source,
    uint64_t sink, std::vector<uint8_t>* sourceSide) {
    if (source >= numNodes || sink >= numNodes) {
        throw BinderException{"Max-flow source/sink node offset out of range."};
    }
    if (source == sink) {
        throw BinderException{"Max-flow source and sink must be distinct nodes."};
    }
    Dinic dinic{numNodes};
    for (const auto& [endpoints, cap] : arcs) {
        if (cap < 0) {
            throw BinderException{"Max-flow does not support negative edge capacities."};
        }
        dinic.addEdge(endpoints.first, endpoints.second, cap);
    }
    const double flow = dinic.maxFlow(source, sink);
    if (sourceSide != nullptr) {
        dinic.reachableFromSource(source, *sourceSide);
    }
    return flow;
}

} // namespace algo_extension
} // namespace lbug
