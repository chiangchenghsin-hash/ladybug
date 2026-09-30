#pragma once

#include <cstdint>
#include <vector>

#include "common/directed_csr.h"

namespace lbug {
namespace algo_extension {

// VF2++-style subgraph isomorphism over directed graphs (pattern is typically a DAG, the
// target may be any directed graph).
//
// Semantics: non-induced subgraph isomorphism (monomorphism). Every pattern edge must map to
// a target edge; the target may contain additional edges. This matches workflow-pattern use
// cases (e.g. ComfyUI templates) where the target graph has extra parallel edges.
//
// The search mirrors NetworkX's vf2pp `_find_candidates_Di` / `_consistent_PT`:
//   - candidate generation intersects target predecessors of mapped pattern successors
//     (resp. successors of mapped pattern predecessors) — direction-aware
//   - feasibility checks edge existence in the target for every mapped in/out neighbor
//   - pattern-side DAG structure is exploited for free: Kahn detection, then node ordering
//     by (topological depth asc, out-degree desc) so the search anchors at sources first
//   - when the TARGET is also a DAG, a depth filter (depth_t[v] >= depth_p[u]) prunes
//     candidates; for cyclic targets the filter is disabled (depth is undefined)
//
// Match output: matches[m][patternNode] == targetNode, one entry per complete mapping.
// maxMatches == 0 means unlimited.
struct VF2PPContext {
    const DirectedCSR& pattern;
    const DirectedCSR& target;
    uint64_t maxMatches;
    std::vector<uint64_t> nodeOrder; // pattern nodes in search order
    bool useDepthFilter;             // target detected as DAG

    VF2PPContext(const DirectedCSR& pattern, const DirectedCSR& target, uint64_t maxMatches);
};

// Returns all (up to maxMatches) subgraph isomorphisms of `pattern` in `target`.
void vf2ppAllMatches(const DirectedCSR& pattern, const DirectedCSR& target, uint64_t maxMatches,
    std::vector<std::vector<uint64_t>>& outMatches);

// Exposed for unit tests.
uint64_t vf2ppCountMatches(const DirectedCSR& pattern, const DirectedCSR& target);

} // namespace algo_extension
} // namespace lbug
