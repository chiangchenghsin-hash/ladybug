// embedding_similarity — 8 DOUBLE args, static SIM function
#include "binder/binder.h"
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

struct CSBD final : TableFuncBindData {
    double fa[4], fb[4];
    CSBD(const double* a,const double* b,expression_vector c,row_idx_t n)
        :TableFuncBindData{std::move(c),n} {for(int i=0;i<4;i++){fa[i]=a[i];fb[i]=b[i];}}
    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<CSBD>(fa,fb,columns,numRows);}
};

static double featureSim(double a, double b) {
    double d=std::max(std::max(std::abs(a),std::abs(b)),0.001);
    return 1.0-std::abs(a-b)/d;
}

static offset_t tableFunc(const TableFuncMorsel&,const TableFuncInput& in,DataChunk& out){
    auto bd = in.bindData->constPtrCast<CSBD>();
    double sBa =featureSim(bd->fa[0],bd->fb[0]), sBei=featureSim(bd->fa[1],bd->fb[1]);
    double sSe =featureSim(bd->fa[2],bd->fb[2]), sDom=featureSim(bd->fa[3],bd->fb[3]);
    double all=(sBa+sBei+sSe+sDom)/4.0;
    auto pos=out.state->getSelVector()[0];
    out.getValueVectorMutable(0).setValue(pos,all);
    out.getValueVectorMutable(1).setValue(pos,sBa); out.getValueVectorMutable(2).setValue(pos,sBei);
    out.getValueVectorMutable(3).setValue(pos,sSe); out.getValueVectorMutable(4).setValue(pos,sDom);
    out.getValueVectorMutable(5).setValue(pos,std::string("ba_ratio,bei_ratio,sentiment,dominance"));
    return 1;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* in){
    auto g=[&](idx_t i){return in->getValue(i).getValue<double>();};
    double a[4]={g(0),g(1),g(2),g(3)}, b[4]={g(4),g(5),g(6),g(7)};
    std::vector<std::string> ns={"similarity_score","ba_ratio_similarity",
        "bei_ratio_similarity","sentiment_similarity","dominance_similarity","compared_features"};
    std::vector<LogicalType> ts;ts.reserve(6);
    for(int i=0;i<5;i++)ts.push_back(LogicalType::DOUBLE());ts.push_back(LogicalType::STRING());
    ns=TableFunction::extractYieldVariables(ns,in->yieldVariables);
    return std::make_unique<CSBD>(a,b,in->binder->createVariables(ns,ts),1);
}

function_set CharacterSimilarityFunction::getFunctionSet(){
    function_set fs;
    auto f=std::make_unique<TableFunction>(name,std::vector{LogicalTypeID::DOUBLE,
        LogicalTypeID::DOUBLE,LogicalTypeID::DOUBLE,LogicalTypeID::DOUBLE,
        LogicalTypeID::DOUBLE,LogicalTypeID::DOUBLE,LogicalTypeID::DOUBLE,LogicalTypeID::DOUBLE});
    f->tableFunc=SimpleTableFunc::getTableFunc(tableFunc); f->bindFunc=bindFunc;
    f->initSharedStateFunc=SimpleTableFunc::initSharedState;
    f->initLocalStateFunc=TableFunction::initEmptyLocalState;
    fs.push_back(std::move(f)); return fs;
}

}} // namespaces
