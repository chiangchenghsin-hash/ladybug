// detect_drift_points — variable-dimension embedding flat list + turning point detection
//
// CALL detect_drift_points(
//     flat_embeddings LIST<DOUBLE>,   -- N×D doubles (row-major: e1_1, e1_2, ..., eN_D)
//     num_embeddings INT64,            -- N
//     sequence_points LIST<INT64>,            -- N sequence_point numbers
//     embedding_dim INT64,             -- D (e.g. 768)
//     threshold:=0.3                   -- [optional] DOUBLE detection threshold
// ) RETURN
//     label INT64, embedding_shift DOUBLE,
//     significance DOUBLE, direction STRING
//
// Algorithm:
//   1. Reconstruct N vectors of D dimensions from flat list
//   2. Compute pairwise cosine distance between consecutive vectors
//   3. Min-max normalize distances → significance [0,1]
//   4. Filter by threshold, sort by significance descending
//
// Pure computation — no table scans. Data prepared via Cypher COLLECT at app layer.

#include "binder/binder.h"
#include "binder/expression/literal_expression.h"
#include "common/types/value/nested.h"
#include "common/types/value/value.h"
#include "function/timeseries_function.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::processor;

namespace lbug { namespace timeseries_extension {

struct DTPBD final : TableFuncBindData {
    std::vector<double> embeds;     // N×D doubles (row-major)
    std::vector<int64_t> chaps;     // N sequence_point numbers
    int64_t dims;                   // D (e.g. 768)
    int64_t numEmb;                 // N
    double thr;                     // detection threshold
    DTPBD(std::vector<double> e, std::vector<int64_t> c, int64_t d, int64_t n, double t,
          expression_vector co, row_idx_t nr)
        : TableFuncBindData{std::move(co),nr}, embeds{std::move(e)}, chaps{std::move(c)},
          dims{d}, numEmb{n}, thr{t} {}
    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DTPBD>(embeds,chaps,dims,numEmb,thr,columns,numRows);}
};

// Cosine distance: 1 − dot(a,b)/(|a|×|b|), range [0,2]
static double cosDist(const double* a, const double* b, int64_t dims) {
    double d=0, nA=0, nB=0;
    for (int64_t i=0; i<dims; i++) { d+=a[i]*b[i]; nA+=a[i]*a[i]; nB+=b[i]*b[i]; }
    if (nA<1e-12 || nB<1e-12) return 0.0;
    double cs = d/std::sqrt(nA*nB);
    if (cs>1.0) cs=1.0; if (cs<-1.0) cs=-1.0;  // clamp rounding error
    return 1.0 - cs;
}

static offset_t tableFunc(const TableFuncMorsel&, const TableFuncInput& in, DataChunk& out) {
    auto bd = in.bindData->constPtrCast<DTPBD>();
    int64_t N = bd->numEmb, D = bd->dims;
    if (N < 2) return 0;

    // Pairwise cosine distances between consecutive embeddings
    std::vector<double> dist(N-1);
    for (int64_t i=1; i<N; i++)
        dist[i-1] = cosDist(&bd->embeds[(i-1)*D], &bd->embeds[i*D], D);

    double mn = *std::min_element(dist.begin(), dist.end());
    double mx = *std::max_element(dist.begin(), dist.end());
    double rng = mx - mn + 0.001;

    // Collect turning points above threshold
    struct TP { int64_t ch; double shift, sig; std::string dir; };
    std::vector<TP> tps;
    for (int64_t i=0; i<N-1; i++) {
        if (dist[i] > bd->thr) {
            double sig = (dist[i] - mn) / rng;
            std::string dir = (i>0 && dist[i] > dist[i-1]) ? "up" : "down";
            tps.push_back({bd->chaps[i+1], dist[i], sig, dir});
        }
    }

    // Sort by significance descending
    std::sort(tps.begin(), tps.end(),
              [](const TP& a, const TP& b) { return a.sig > b.sig; });

    // Output to DataChunk
    for (size_t j=0; j<tps.size(); j++) {
        out.getValueVectorMutable(0).setValue((offset_t)j, tps[j].ch);
        out.getValueVectorMutable(1).setValue((offset_t)j, tps[j].shift);
        out.getValueVectorMutable(2).setValue((offset_t)j, tps[j].sig);
        out.getValueVectorMutable(3).setValue((offset_t)j, tps[j].dir);
    }
    return (offset_t)tps.size();
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* in) {
    // flat_embeddings (param 0) — LIST<DOUBLE>
    auto fv = in->getValue(0);
    uint32_t nf = NestedVal::getChildrenSize(&fv);
    std::vector<double> embeds; embeds.reserve(nf);
    for (uint32_t j=0; j<nf; j++)
        embeds.push_back(NestedVal::getChildVal(&fv, j)->getValue<double>());

    // num_embeddings (param 1) — INT64
    int64_t numEmb = in->getValue(1).getValue<int64_t>();

    // sequence_points (param 2) — LIST<INT64>
    auto cv = in->getValue(2);
    uint32_t nc = NestedVal::getChildrenSize(&cv);
    std::vector<int64_t> chaps; chaps.reserve(nc);
    for (uint32_t j=0; j<nc; j++)
        chaps.push_back(NestedVal::getChildVal(&cv, j)->getValue<int64_t>());

    // embedding_dim (param 3) — INT64
    int64_t dims = in->getValue(3).getValue<int64_t>();

    // threshold (optional param, default 0.3)
    double thr = 0.3;
    for (auto& p : in->optionalParamsLegacy)
        if (p->getAlias() == "threshold")
            if (auto le = p->constPtrCast<LiteralExpression>())
                thr = le->getValue().getValue<double>();

    // Validate dimensions: flat_embeddings must have exactly numEmb×dims elements
    if (numEmb <= 0 || dims <= 0 || (int64_t)nf != numEmb * dims || (int64_t)nc != numEmb)
        numEmb = 0;  // invalid input → return 0 rows

    std::vector<std::string> ns = {"label","embedding_shift","significance","direction"};
    std::vector<LogicalType> ts; ts.reserve(4);
    ts.push_back(LogicalType::INT64()); ts.push_back(LogicalType::DOUBLE());
    ts.push_back(LogicalType::DOUBLE()); ts.push_back(LogicalType::STRING());
    ns = TableFunction::extractYieldVariables(ns, in->yieldVariables);
    row_idx_t maxRows = (row_idx_t)(numEmb > 1 ? numEmb - 1 : 1);
    return std::make_unique<DTPBD>(std::move(embeds), std::move(chaps), dims, numEmb, thr,
                                    in->binder->createVariables(ns, ts), maxRows);
}

function_set DetectTurningPointsFunction::getFunctionSet() {
    function_set fs;
    // 4 positional params: ANY(LIST<DOUBLE>), INT64, ANY(LIST<INT64>), INT64
    // + optional threshold:=DOUBLE
    auto f = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64,
                    LogicalTypeID::ANY, LogicalTypeID::INT64});
    f->inferInputTypes = [](const expression_vector&) -> std::vector<LogicalType> {
        std::vector<LogicalType> result; result.reserve(4);
        result.push_back(LogicalType::LIST(LogicalType::DOUBLE()));
        result.push_back(LogicalType::INT64());
        result.push_back(LogicalType::LIST(LogicalType::INT64()));
        result.push_back(LogicalType::INT64());
        return result;
    };
    f->tableFunc = SimpleTableFunc::getTableFunc(tableFunc);
    f->bindFunc = bindFunc;
    f->initSharedStateFunc = SimpleTableFunc::initSharedState;
    f->initLocalStateFunc = TableFunction::initEmptyLocalState;
    fs.push_back(std::move(f));
    return fs;
}

}} // namespaces
