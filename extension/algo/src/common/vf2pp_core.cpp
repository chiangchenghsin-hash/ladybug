#include "common/vf2pp_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace lbug {
namespace algo_extension {

namespace {

// Recursive backtracking state. Depth is bounded by the number of pattern nodes (small), so
// recursion is safe and keeps the code close to the reference algorithm.
class VF2PPSearch {
public:
    VF2PPSearch(const DirectedCSR& pattern, const DirectedCSR& target, uint64_t maxMatches,
        const std::vector<uint64_t>& nodeOrder, bool useDepthFilter)
        : pattern{pattern}, target{target}, maxMatches{maxMatches}, nodeOrder{nodeOrder},
          useDepthFilter{useDepthFilter}, mapping(pattern.numNodes, kUnmapped),
          used(target.numNodes, false) {}

    void run(std::vector<std::vector<uint64_t>>& outMatches) {
        backtrack(0, outMatches);
    }

private:
    static constexpr int64_t kUnmapped = -1;

    const DirectedCSR& pattern;
    const DirectedCSR& target;
    const uint64_t maxMatches;
    const std::vector<uint64_t>& nodeOrder;
    const bool useDepthFilter;

    std::vector<int64_t> mapping;   // patternNode -> targetNode (or kUnmapped)
    std::vector<bool> used;         // targetNode -> already mapped

    // True iff target has an edge w -> v (or v -> w for the fwd direction).
    bool hasEdge(uint64_t from, uint64_t to, const std::vector<uint64_t>& offsets,
        const std::vector<uint64_t>& neighbors) const {
        const auto begin = neighbors.begin() + static_cast<std::ptrdiff_t>(offsets[from]);
        const auto end = neighbors.begin() + static_cast<std::ptrdiff_t>(offsets[from + 1]);
        return std::binary_search(begin, end, to);
    }

    // Candidate target nodes for pattern node `u` (mirrors vf2pp `_find_candidates_Di`).
    void candidatesFor(uint64_t u, std::vector<uint64_t>& out) const {
        out.clear();
        const auto succBegin = pattern.fwdNeighbors.begin() +
                               static_cast<std::ptrdiff_t>(pattern.fwdOffsets[u]);
        const auto succEnd = pattern.fwdNeighbors.begin() +
                             static_cast<std::ptrdiff_t>(pattern.fwdOffsets[u + 1]);
        const auto predBegin = pattern.bwdNeighbors.begin() +
                               static_cast<std::ptrdiff_t>(pattern.bwdOffsets[u]);
        const auto predEnd = pattern.bwdNeighbors.begin() +
                             static_cast<std::ptrdiff_t>(pattern.bwdOffsets[u + 1]);

        const bool hasCoveredSucc = std::any_of(succBegin, succEnd,
            [&](auto s) { return mapping[s] != kUnmapped; });
        const bool hasCoveredPred = std::any_of(predBegin, predEnd,
            [&](auto s) { return mapping[s] != kUnmapped; });

        if (hasCoveredSucc) {
            // u has a mapped successor x: v must be a predecessor of mapping[x] in the target.
            bool first = true;
            for (auto it = succBegin; it != succEnd; ++it) {
                const auto w = mapping[*it];
                if (w == kUnmapped) {
                    continue;
                }
                if (first) {
                    out.assign(target.bwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.bwdOffsets[w]),
                        target.bwdNeighbors.begin() +
                            static_cast<std::ptrdiff_t>(target.bwdOffsets[w + 1]));
                    first = false;
                } else {
                    std::vector<uint64_t> intersection;
                    const auto b = target.bwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.bwdOffsets[w]);
                    const auto e = target.bwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.bwdOffsets[w + 1]);
                    std::set_intersection(out.begin(), out.end(), b, e,
                        std::back_inserter(intersection));
                    out.swap(intersection);
                }
            }
        } else if (hasCoveredPred) {
            // u has a mapped predecessor x: v must be a successor of mapping[x] in the target.
            bool first = true;
            for (auto it = predBegin; it != predEnd; ++it) {
                const auto w = mapping[*it];
                if (w == kUnmapped) {
                    continue;
                }
                if (first) {
                    out.assign(target.fwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.fwdOffsets[w]),
                        target.fwdNeighbors.begin() +
                            static_cast<std::ptrdiff_t>(target.fwdOffsets[w + 1]));
                    first = false;
                } else {
                    std::vector<uint64_t> intersection;
                    const auto b = target.fwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.fwdOffsets[w]);
                    const auto e = target.fwdNeighbors.begin() +
                                   static_cast<std::ptrdiff_t>(target.fwdOffsets[w + 1]);
                    std::set_intersection(out.begin(), out.end(), b, e,
                        std::back_inserter(intersection));
                    out.swap(intersection);
                }
            }
        } else {
            // No mapped neighbors yet: all unused target nodes are candidates.
            for (uint64_t v = 0; v < target.numNodes; ++v) {
                if (!used[v]) {
                    out.push_back(v);
                }
            }
        }

        // Prune: target node already mapped, or degree too small (non-induced subgraph iso).
        // In/out degree of v must be at least that of u.
        const auto uInDeg = pattern.bwdOffsets[u + 1] - pattern.bwdOffsets[u];
        const auto uOutDeg = pattern.fwdOffsets[u + 1] - pattern.fwdOffsets[u];
        auto write = out.begin();
        for (auto v : out) {
            if (used[v]) {
                continue;
            }
            const auto tvInDeg = target.bwdOffsets[v + 1] - target.bwdOffsets[v];
            const auto tvOutDeg = target.fwdOffsets[v + 1] - target.fwdOffsets[v];
            if (tvInDeg < uInDeg || tvOutDeg < uOutDeg) {
                continue;
            }
            if (useDepthFilter &&
                target.depth[v] < static_cast<int64_t>(pattern.depth[u])) {
                continue;
            }
            *write++ = v;
        }
        out.erase(write, out.end());
    }

    // Mirrors vf2pp `_consistent_PT`: every mapped neighbor of u must keep its edge in target.
    bool feasible(uint64_t u, uint64_t v) const {
        for (auto it = pattern.bwdNeighbors.begin() +
                       static_cast<std::ptrdiff_t>(pattern.bwdOffsets[u]);
             it != pattern.bwdNeighbors.begin() +
                       static_cast<std::ptrdiff_t>(pattern.bwdOffsets[u + 1]);
             ++it) {
            const auto x = mapping[*it];
            if (x != kUnmapped && !hasEdge(x, v, target.fwdOffsets, target.fwdNeighbors)) {
                return false; // pattern edge x -> u must map to target edge x' -> v
            }
        }
        for (auto it = pattern.fwdNeighbors.begin() +
                       static_cast<std::ptrdiff_t>(pattern.fwdOffsets[u]);
             it != pattern.fwdNeighbors.begin() +
                       static_cast<std::ptrdiff_t>(pattern.fwdOffsets[u + 1]);
             ++it) {
            const auto x = mapping[*it];
            if (x != kUnmapped && !hasEdge(v, x, target.fwdOffsets, target.fwdNeighbors)) {
                return false; // pattern edge u -> x must map to target edge v -> x'
            }
        }
        return true;
    }

    void backtrack(size_t depth, std::vector<std::vector<uint64_t>>& outMatches) {
        if (maxMatches > 0 && outMatches.size() >= maxMatches) {
            return;
        }
        if (depth == nodeOrder.size()) {
            outMatches.emplace_back(mapping.begin(), mapping.end());
            return;
        }
        const auto u = nodeOrder[depth];
        std::vector<uint64_t> candidates;
        candidatesFor(u, candidates);
        for (auto v : candidates) {
            if (!feasible(u, v)) {
                continue;
            }
            mapping[u] = static_cast<int64_t>(v);
            used[v] = true;
            backtrack(depth + 1, outMatches);
            used[v] = false;
            mapping[u] = kUnmapped;
            if (maxMatches > 0 && outMatches.size() >= maxMatches) {
                return;
            }
        }
    }
};

} // namespace

VF2PPContext::VF2PPContext(const DirectedCSR& pattern, const DirectedCSR& target,
    uint64_t maxMatches)
    : pattern{pattern}, target{target}, maxMatches{maxMatches} {
    useDepthFilter = pattern.isDAG && target.isDAG;

    // Node ordering: for a DAG pattern, anchor at sources first (topological depth asc),
    // tie-break by out-degree desc (mirrors VF2++'s "most restrictive first" spirit).
    // For cyclic patterns fall back to total-degree desc.
    nodeOrder.resize(pattern.numNodes);
    for (uint64_t i = 0; i < pattern.numNodes; ++i) {
        nodeOrder[i] = i;
    }
    if (pattern.isDAG) {
        std::stable_sort(nodeOrder.begin(), nodeOrder.end(), [&](auto a, auto b) {
            if (pattern.depth[a] != pattern.depth[b]) {
                return pattern.depth[a] < pattern.depth[b];
            }
            const auto aOut = pattern.fwdOffsets[a + 1] - pattern.fwdOffsets[a];
            const auto bOut = pattern.fwdOffsets[b + 1] - pattern.fwdOffsets[b];
            return aOut > bOut;
        });
    } else {
        std::stable_sort(nodeOrder.begin(), nodeOrder.end(), [&](auto a, auto b) {
            const auto aDeg = (pattern.fwdOffsets[a + 1] - pattern.fwdOffsets[a]) +
                              (pattern.bwdOffsets[a + 1] - pattern.bwdOffsets[a]);
            const auto bDeg = (pattern.fwdOffsets[b + 1] - pattern.fwdOffsets[b]) +
                              (pattern.bwdOffsets[b + 1] - pattern.bwdOffsets[b]);
            return aDeg > bDeg;
        });
    }
}

void vf2ppAllMatches(const DirectedCSR& pattern, const DirectedCSR& target, uint64_t maxMatches,
    std::vector<std::vector<uint64_t>>& outMatches) {
    outMatches.clear();
    if (pattern.numNodes == 0) {
        return; // empty pattern: no match by definition
    }
    if (pattern.numNodes > target.numNodes) {
        return; // cannot fit
    }
    VF2PPContext ctx{pattern, target, maxMatches};
    VF2PPSearch search{pattern, target, maxMatches, ctx.nodeOrder, ctx.useDepthFilter};
    search.run(outMatches);
}

uint64_t vf2ppCountMatches(const DirectedCSR& pattern, const DirectedCSR& target) {
    std::vector<std::vector<uint64_t>> matches;
    vf2ppAllMatches(pattern, target, 0 /*unlimited*/, matches);
    return matches.size();
}

} // namespace algo_extension
} // namespace lbug
