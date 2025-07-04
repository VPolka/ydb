#include "sql.h"
#include "node.h"

#include "context.h"

#include <yql/essentials/utils/yql_panic.h>

#include <library/cpp/charset/ci_string.h>

using namespace NYql;

namespace NSQLTranslationV0 {

class TSubqueryNode: public INode {
public:
    TSubqueryNode(TSourcePtr&& source, const TString& alias, bool inSubquery, int ensureTupleSize)
        : INode(source->GetPos())
        , Source_(std::move(source))
        , Alias_(alias)
        , InSubquery_(inSubquery)
        , EnsureTupleSize_(ensureTupleSize)
    {
        YQL_ENSURE(!Alias_.empty());
    }

    ISource* GetSource() override {
        return Source_.Get();
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        YQL_ENSURE(!src, "Source not expected for subquery node");
        Source_->UseAsInner();
        if (!Source_->Init(ctx, nullptr)) {
            return false;
        }

        TTableList tableList;
        Source_->GetInputTables(tableList);

        auto tables = BuildInputTables(Pos_, tableList, InSubquery_);
        if (!tables->Init(ctx, Source_.Get())) {
            return false;
        }

        auto source = Source_->Build(ctx);
        if (!source) {
            return false;
        }
        if (EnsureTupleSize_ != -1) {
            source = Y("EnsureTupleSize", source, Q(ToString(EnsureTupleSize_)));
        }

        Node_ = Y("let", Alias_, Y("block", Q(L(tables, Y("return", Q(Y("world", source)))))));
        IsUsed_ = true;
        return true;
    }

    void DoUpdateState() const override {
        State_.Set(ENodeState::Const, true);
    }

    bool UsedSubquery() const override {
        return IsUsed_;
    }

    TAstNode* Translate(TContext& ctx) const override {
        Y_DEBUG_ABORT_UNLESS(Node_);
        return Node_->Translate(ctx);
    }

    const TString* SubqueryAlias() const override {
        return &Alias_;
    }

    TPtr DoClone() const final {
        return {};
    }

protected:
    TSourcePtr Source_;
    TNodePtr Node_;
    const TString Alias_;
    const bool InSubquery_;
    const int EnsureTupleSize_;
    bool IsUsed_ = false;
};

TNodePtr BuildSubquery(TSourcePtr source, const TString& alias, bool inSubquery, int ensureTupleSize) {
    return new TSubqueryNode(std::move(source), alias, inSubquery, ensureTupleSize);
}

class TSourceNode: public INode {
public:
    TSourceNode(TPosition pos, TSourcePtr&& source, bool checkExist)
        : INode(pos)
        , Source_(std::move(source))
        , CheckExist_(checkExist)
    {}

    ISource* GetSource() override {
        return Source_.Get();
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        if (AsInner_) {
            Source_->UseAsInner();
        }
        if (!Source_->Init(ctx, src)) {
            return false;
        }
        Node_ = Source_->Build(ctx);
        if (!Node_) {
            return false;
        }
        if (src) {
            if (IsSubquery()) {
                /// should be not used?
                auto columnsPtr = Source_->GetColumns();
                if (!columnsPtr || columnsPtr->All || columnsPtr->QualifiedAll || columnsPtr->List.size() != 1) {
                    ctx.Error(Pos_) << "Source used in expression should contain one concrete column";
                    return false;
                }
                Node_ = Y("Member", Y("SqlAccess", Q("dict"), Y("Take", Node_, Y("Uint64", Q("1"))), Y("Uint64", Q("0"))), Q(columnsPtr->List.front()));
            }
            src->AddDependentSource(Source_.Get());
        }
        return true;
    }

    bool IsSubquery() const {
        return !AsInner_ && Source_->IsSelect() && !CheckExist_;
    }

    void DoUpdateState() const override {
        State_.Set(ENodeState::Const, IsSubquery());
    }

    TAstNode* Translate(TContext& ctx) const override {
        Y_DEBUG_ABORT_UNLESS(Node_);
        return Node_->Translate(ctx);
    }

    TPtr DoClone() const final {
        return new TSourceNode(Pos_, Source_->CloneSource(), CheckExist_);
    }
protected:
    TSourcePtr Source_;
    TNodePtr Node_;
    bool CheckExist_;
};

TNodePtr BuildSourceNode(TPosition pos, TSourcePtr source, bool checkExist) {
    return new TSourceNode(pos, std::move(source), checkExist);
}

class TFakeSource: public ISource {
public:
    TFakeSource(TPosition pos)
        : ISource(pos)
    {}

    bool IsFake() const override {
        return true;
    }

    bool AddFilter(TContext& ctx, TNodePtr filter) override  {
        Y_UNUSED(filter);
        ctx.Error(Pos_) << "Source does not allow filtering";
        return false;
    }

    TNodePtr Build(TContext& ctx) override  {
        Y_UNUSED(ctx);
        return Y("AsList", Y("Uint32", Q("0")));
    }

    bool AddGroupKey(TContext& ctx, const TString& column) override {
        Y_UNUSED(column);
        ctx.Error(Pos_) << "Source does not allow grouping";
        return false;
    }

    bool AddAggregation(TContext& ctx, TAggregationPtr aggr) override {
        Y_UNUSED(aggr);
        ctx.Error(Pos_) << "Source does not allow aggregation";
        return false;
    }

    bool IsGroupByColumn(const TString& column) const override {
        Y_UNUSED(column);
        return false;
    }

    TNodePtr BuildFilter(TContext& ctx, const TString& label, const TNodePtr& groundNode) override {
        Y_UNUSED(ctx);
        Y_UNUSED(label);
        Y_UNUSED(groundNode);
        return nullptr;
    }

    TNodePtr BuildAggregation(const TString& label) override {
        Y_UNUSED(label);
        return nullptr;
    }

    TPtr DoClone() const final {
        return new TFakeSource(Pos_);
    }
};

TSourcePtr BuildFakeSource(TPosition pos) {
    return new TFakeSource(pos);
}

class TNodeSource: public ISource {
public:
    TNodeSource(TPosition pos, const TNodePtr& node)
        : ISource(pos)
        , Node_(node)
    {
        YQL_ENSURE(Node_);
        FakeSource_ = BuildFakeSource(pos);
    }

    void AllColumns() final {
        UseAllColumns_ = true;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) final {
        Y_UNUSED(ctx);
        if (UseAllColumns_) {
            return true;
        }

        if (column.IsAsterisk()) {
            AllColumns();
        } else {
            Columns_.push_back(*column.GetColumnName());
        }

        return true;
    }

    TNodePtr Build(TContext& ctx) final  {
        ctx.PushBlockShortcuts();
        if (!Node_->Init(ctx, FakeSource_.Get())) {
            return {};
        }

        Node_ = ctx.GroundBlockShortcutsForExpr(Node_);
        auto nodeAst = AstNode(Node_);

        if (UseAllColumns_) {
            return nodeAst;
        } else {
            auto members = Y();
            for (auto& column : Columns_) {
                members = L(members, BuildQuotedAtom(Pos_, column));
            }

            return Y(ctx.UseUnordered(*this) ? "OrderedMap" : "Map", nodeAst, BuildLambda(Pos_, Y("row"), Y("SelectMembers", "row", Q(members))));
        }
    }

    TPtr DoClone() const final {
        return new TNodeSource(Pos_, Node_);
    }

private:
    TNodePtr Node_;
    TSourcePtr FakeSource_;
    TVector<TString> Columns_;
    bool UseAllColumns_ = false;
};

TSourcePtr BuildNodeSource(TPosition pos, const TNodePtr& node) {
    return new TNodeSource(pos, node);
}

class IProxySource: public ISource {
protected:
    IProxySource(TPosition pos, ISource* src)
        : ISource(pos)
        , Source_(src)
    {}

    void AllColumns() override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        return Source_->AllColumns();
    }

    const TColumns* GetColumns() const override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        return Source_->GetColumns();
    }

    void GetInputTables(TTableList& tableList) const override {
        Source_->GetInputTables(tableList);
        ISource::GetInputTables(tableList);
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        const TString label(Source_->GetLabel());
        Source_->SetLabel(Label_);
        const auto ret = Source_->AddColumn(ctx, column);
        Source_->SetLabel(label);
        return ret;
    }

    bool ShouldUseSourceAsColumn(const TString& source) override {
        return Source_->ShouldUseSourceAsColumn(source);
    }

    bool IsStream() const override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        return Source_->IsStream();
    }

    bool IsOrdered() const override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        return Source_->IsOrdered();
    }

    TWriteSettings GetWriteSettings() const override {
        Y_DEBUG_ABORT_UNLESS(Source_);
        return Source_->GetWriteSettings();
    }

protected:
    void SetSource(ISource* source) {
        Source_ = source;
    }

    ISource* Source_;
};

class IRealSource: public ISource {
protected:
    IRealSource(TPosition pos)
        : ISource(pos)
    {
    }

    void AllColumns() override {
        Columns_.SetAll();
    }

    const TColumns* GetColumns() const override {
        return &Columns_;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        auto& label = *column.GetSourceName();
        if (!label.empty() && label != GetLabel()) {
            if (column.IsReliable()) {
                ctx.Error(column.GetPos()) << "Unknown correlation name: " << label;
            }
            return {};
        }
        if (column.IsAsterisk()) {
            return true;
        }
        const auto* name = column.GetColumnName();
        if (name && !Columns_.IsColumnPossible(ctx, *name) && !IsAlias(EExprSeat::GroupBy, *name)) {
            if (column.IsReliable()) {
                TStringBuilder sb;
                sb << "Column " << *name << " is not in source column set";
                if (const auto mistype = FindColumnMistype(*name)) {
                    sb << ". Did you mean " << mistype.GetRef() << "?";
                }
                ctx.Error(column.GetPos()) << sb;
            }
            return {};
        }
        return true;
    }

    TMaybe<TString> FindColumnMistype(const TString& name) const override {
        auto result = FindMistypeIn(Columns_.Real, name);
        if (!result) {
            auto result = FindMistypeIn(Columns_.Artificial, name);
        }
        return result ? result : ISource::FindColumnMistype(name);
    }

protected:
    TColumns Columns_;
};

class TMuxSource: public ISource {
public:
    TMuxSource(TPosition pos, TVector<TSourcePtr>&& sources)
        : ISource(pos)
        , Sources_(std::move(sources))
    {
        YQL_ENSURE(Sources_.size() > 1);
    }

    void AllColumns() final {
        for (auto& source: Sources_) {
            source->AllColumns();
        }
    }

    const TColumns* GetColumns() const final {
        // Columns are equal in all sources. Return from the first one
        return Sources_.front()->GetColumns();
    }

    void GetInputTables(TTableList& tableList) const final {
        for (auto& source: Sources_) {
            source->GetInputTables(tableList);
        }
        ISource::GetInputTables(tableList);
    }

    bool IsStream() const final {
        return AnyOf(Sources_, [] (const TSourcePtr& s) { return s->IsStream(); });
    }

    bool DoInit(TContext& ctx, ISource* src) final {
        for (auto& source: Sources_) {
            if (AsInner_) {
                source->UseAsInner();
            }

            ctx.PushBlockShortcuts();
            if (src) {
                src->AddDependentSource(source.Get());
            }
            if (!source->Init(ctx, src)) {
                return false;
            }
            if (!source->InitFilters(ctx)) {
                return false;
            }
            FiltersGrounds_.push_back(ctx.GroundBlockShortcuts(Pos_));
        }
        return true;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) final {
        for (auto& source: Sources_) {
            if (!source->AddColumn(ctx, column)) {
                return {};
            }
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) final {
        TNodePtr block;
        auto muxArgs = Y();
        for (size_t i = 0; i < Sources_.size(); ++i) {
            auto& source = Sources_[i];
            auto input = source->Build(ctx);
            auto ref = ctx.MakeName("src");
            muxArgs->Add(ref);
            if (block) {
                block = L(block, Y("let", ref, input));
            } else {
                block = Y(Y("let", ref, input));
            }
            auto filter = source->BuildFilter(ctx, ref, FiltersGrounds_[i]);
            if (filter) {
                block = L(block, Y("let", ref, filter));
            }
        }
        return GroundWithExpr(block, Y("Mux", Q(muxArgs)));
    }

    bool AddFilter(TContext& ctx, TNodePtr filter) final {
        Y_UNUSED(filter);
        ctx.Error() << "Filter is not allowed for multiple sources";
        return false;
    }

    TPtr DoClone() const final {
        // Don't clone FiltersGrounds container because it will be initialized in DoInit of cloned object
        return new TMuxSource(Pos_, CloneContainer(Sources_));
    }

protected:
    TVector<TSourcePtr> Sources_;
    TVector<TNodePtr> FiltersGrounds_;
};

TSourcePtr BuildMuxSource(TPosition pos, TVector<TSourcePtr>&& sources) {
    return new TMuxSource(pos, std::move(sources));
}

class TSubqueryRefNode: public IRealSource {
public:
    TSubqueryRefNode(const TNodePtr& subquery, const TString& alias, int tupleIndex)
        : IRealSource(subquery->GetPos())
        , Subquery_(subquery)
        , Alias_(alias)
        , TupleIndex_(tupleIndex)
    {
        YQL_ENSURE(subquery->GetSource());
    }

    ISource* GetSource() override {
        return this;
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        // independent subquery should not connect source
        Subquery_->UseAsInner();
        if (!Subquery_->Init(ctx, nullptr)) {
            return false;
        }
        Columns_ = *Subquery_->GetSource()->GetColumns();
        Node_ = BuildAtom(Pos_, Alias_, TNodeFlags::Default);
        if (TupleIndex_ != -1) {
            Node_ = Y("Nth", Node_, Q(ToString(TupleIndex_)));
        }
        if (!Node_->Init(ctx, src)) {
            return false;
        }
        if (src && Subquery_->GetSource()->IsSelect()) {
            auto columnsPtr = &Columns_;
            if (!columnsPtr || columnsPtr->All || columnsPtr->QualifiedAll || columnsPtr->List.size() != 1) {
                ctx.Error(Pos_) << "Source used in expression should contain one concrete column";
                return false;
            }
            Node_ = Y("Member", Y("SqlAccess", Q("dict"), Y("Take", Node_, Y("Uint64", Q("1"))), Y("Uint64", Q("0"))), Q(columnsPtr->List.front()));
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) override  {
        Y_UNUSED(ctx);
        return Node_;
    }

    bool IsStream() const override {
        return Subquery_->GetSource()->IsStream();
    }

    void DoUpdateState() const override {
        State_.Set(ENodeState::Const, true);
    }

    TAstNode* Translate(TContext& ctx) const override {
        Y_DEBUG_ABORT_UNLESS(Node_);
        return Node_->Translate(ctx);
    }

    TPtr DoClone() const final {
        return new TSubqueryRefNode(Subquery_, Alias_, TupleIndex_);
    }

protected:
    TNodePtr Subquery_;
    const TString Alias_;
    const int TupleIndex_;
    TNodePtr Node_;
};

TNodePtr BuildSubqueryRef(TNodePtr subquery, const TString& alias, int tupleIndex) {
    return new TSubqueryRefNode(std::move(subquery), alias, tupleIndex);
}

class TTableSource: public IRealSource {
public:
    TTableSource(TPosition pos, const TTableRef& table, bool stream, const TString& label)
        : IRealSource(pos)
        , Table_(table)
        , Stream_(stream)
    {
        SetLabel(label.empty() ? Table_.ShortName() : label);
    }

    void GetInputTables(TTableList& tableList) const override {
        tableList.push_back(Table_);
        ISource::GetInputTables(tableList);
    }

    bool ShouldUseSourceAsColumn(const TString& source) override {
        return source && source != GetLabel();
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        Columns_.Add(column.GetColumnName(), column.GetCountHint(), column.IsArtificial(), column.IsReliable());
        if (!IRealSource::AddColumn(ctx, column)) {
            return {};
        }
        return false;
    }

    bool SetSamplingOptions(
            TContext& ctx,
            TPosition pos,
            ESampleMode mode,
            TNodePtr samplingRate,
            TNodePtr samplingSeed) override {
        Y_UNUSED(pos);
        TString modeName;
        if (!samplingSeed) {
            samplingSeed = Y("Int32", Q("0"));
        }

        switch (mode) {
        case ESampleMode::Auto:
            modeName = "bernoulli";
            samplingRate = Y("*", samplingRate, Y("Double", Q("100")));
            break;
        case ESampleMode::Bernoulli:
            modeName = "bernoulli";
            break;
        case ESampleMode::System:
            modeName = "system";
            break;
        }

        samplingRate = Y("Ensure", samplingRate, Y(">", samplingRate, Y("Double", Q("0"))), Y("String", Q("Expected sampling rate to be positive")));
        samplingRate = Y("Ensure", samplingRate, Y("<=", samplingRate, Y("Double", Q("100"))), Y("String", Q("Sampling rate is over 100%")));
        auto sampleSettings = Q(Y(Q(modeName), Y("EvaluateAtom", Y("ToString", samplingRate)), Y("EvaluateAtom", Y("ToString", samplingSeed))));
        auto sampleOption = Q(Y(Q("sample"), sampleSettings));
        if (Table_.Options) {
            if (!Table_.Options->Init(ctx, this)) {
                return false;
            }
            Table_.Options = L(Table_.Options, sampleOption);
        } else {
            Table_.Options = Y(sampleOption);
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        if (!Table_.Keys->Init(ctx, nullptr)) {
            return nullptr;
        }
        return AstNode(Table_.RefName);
    }

    bool IsStream() const override {
        return Stream_;
    }

    TPtr DoClone() const final {
        return new TTableSource(Pos_, Table_, Stream_, GetLabel());
    }

    bool IsTableSource() const override {
        return true;
    }
protected:
    TTableRef Table_;
    const bool Stream_;
};

TSourcePtr BuildTableSource(TPosition pos, const TTableRef& table, bool stream, const TString& label) {
    return new TTableSource(pos, table, stream, label);
}

class TInnerSource: public IProxySource {
public:
    TInnerSource(TPosition pos, TNodePtr node, const TString& label)
        : IProxySource(pos, nullptr)
        , Node_(node)
    {
        SetLabel(label);
    }

    bool ShouldUseSourceAsColumn(const TString& source) override {
        return source && source != GetLabel();
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        if (const TString* columnName = column.GetColumnName()) {
            if (columnName && IsExprAlias(*columnName)) {
                return true;
            }
        }
        return IProxySource::AddColumn(ctx, column);
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        auto source = Node_->GetSource();
        if (!source) {
            NewSource_ = TryMakeSourceFromExpression(ctx, Node_);
            source = NewSource_.Get();
        }

        if (!source) {
            ctx.Error(Pos_) << "Invalid inner source node";
            return false;
        }
        source->SetLabel(Label_);
        if (!NewSource_) {
            Node_->UseAsInner();
            if (!Node_->Init(ctx, src)) {
                return false;
            }
        }

        SetSource(source);
        if (NewSource_ && !NewSource_->Init(ctx, src)) {
            return false;
        }

        return ISource::DoInit(ctx, source);
    }

    TNodePtr Build(TContext& ctx) override {
        Y_UNUSED(ctx);
        return NewSource_ ? NewSource_->Build(ctx) : Node_;
    }

    TPtr DoClone() const final {
        return new TInnerSource(Pos_, SafeClone(Node_), GetLabel());
    }
protected:
    TNodePtr Node_;
    TSourcePtr NewSource_;
};

TSourcePtr BuildInnerSource(TPosition pos, TNodePtr node, const TString& label) {
    return new TInnerSource(pos, node, label);
}

/// \todo move to reduce.cpp? or mapreduce.cpp?
class TReduceSource: public IRealSource {
public:
    TReduceSource(TPosition pos,
        ReduceMode mode,
        TSourcePtr source,
        TVector<TSortSpecificationPtr>&& orderBy,
        TVector<TNodePtr>&& keys,
        TVector<TNodePtr>&& args,
        TNodePtr udf,
        TNodePtr having,
        const TWriteSettings& settings)
        : IRealSource(pos)
        , Mode_(mode)
        , Source_(std::move(source))
        , OrderBy_(std::move(orderBy))
        , Keys_(std::move(keys))
        , Args_(std::move(args))
        , Udf_(udf)
        , Having_(having)
        , Settings_(settings)
    {
        YQL_ENSURE(!Keys_.empty());
        YQL_ENSURE(Source_);
    }

    void GetInputTables(TTableList& tableList) const override {
        Source_->GetInputTables(tableList);
        ISource::GetInputTables(tableList);
    }

    bool DoInit(TContext& ctx, ISource* src) final {
        if (AsInner_) {
            Source_->UseAsInner();
        }

        ctx.PushBlockShortcuts();
        YQL_ENSURE(!src);
        if (!Source_->Init(ctx, src)) {
            return false;
        }
        if (!Source_->InitFilters(ctx)) {
            return false;
        }
        FiltersGround_ = ctx.GroundBlockShortcuts(Pos_);
        src = Source_.Get();
        for (auto& key: Keys_) {
            if (!key->Init(ctx, src)) {
                return false;
            }
            auto keyNamePtr = key->GetColumnName();
            YQL_ENSURE(keyNamePtr);
            if (!src->AddGroupKey(ctx, *keyNamePtr)) {
                return false;
            }
        }
        ctx.PushBlockShortcuts();
        if (Having_ && !Having_->Init(ctx, nullptr)) {
            return false;
        }
        HavingGround_ = ctx.GroundBlockShortcuts(Pos_);

        /// SIN: verify reduce one argument
        if (Args_.size() != 1) {
            ctx.Error(Pos_) << "REDUCE requires exactly one UDF argument";
            return false;
        }
        ctx.PushBlockShortcuts();
        if (!Args_[0]->Init(ctx, src)) {
            return false;
        }
        ExprGround_ = ctx.GroundBlockShortcuts(Pos_);

        ctx.PushBlockShortcuts();
        for (auto orderSpec: OrderBy_) {
            if (!orderSpec->OrderExpr->Init(ctx, src)) {
                return false;
            }
        }
        OrderByGround_ = ctx.GroundBlockShortcuts(Pos_);

        if (!Udf_->Init(ctx, src)) {
            return false;
        }
        if (Udf_->GetLabel().empty()) {
            Columns_.SetAll();
        } else {
            Columns_.Add(&Udf_->GetLabel(), false);
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) final {
        auto input = Source_->Build(ctx);
        if (!input) {
            return nullptr;
        }

        auto keysTuple = Y();
        if (Keys_.size() == 1) {
            keysTuple = Y("Member", "row", BuildQuotedAtom(Pos_, *Keys_.back()->GetColumnName()));
        }
        else {
            for (const auto& key: Keys_) {
                keysTuple = L(keysTuple, Y("Member", "row", BuildQuotedAtom(Pos_, *key->GetColumnName())));
            }
            keysTuple = Q(keysTuple);
        }
        auto extractKey = Y("SqlExtractKey", "row", BuildLambda(Pos_, Y("row"), keysTuple));
        auto extractKeyLambda = BuildLambda(Pos_, Y("row"), extractKey);

        TNodePtr processPartitions;
        switch (Mode_) {
            case ReduceMode::ByAll: {
                auto columnPtr = Args_[0]->GetColumnName();
                TNodePtr expr = BuildAtom(Pos_, "partitionStream");
                if (!columnPtr || *columnPtr != "*") {
                    expr = Y("Map", "partitionStream", BuildLambda(Pos_, Y("keyPair"), Q(L(Y(),\
                        Y("Nth", "keyPair", Q(ToString("0"))),\
                        Y("Map", Y("Nth", "keyPair", Q(ToString("1"))), BuildLambda(Pos_, Y("row"),
                                GroundWithExpr(ExprGround_, Args_[0])))))));
                }
                processPartitions = Y("ToSequence", Y("Apply", Udf_, expr));
                break;
            }
            case ReduceMode::ByPartition: {
                processPartitions = Y("SqlReduce", "partitionStream", extractKeyLambda, Udf_,
                    BuildLambda(Pos_, Y("row"), GroundWithExpr(ExprGround_, Args_[0])));
                break;
            }
            default:
                YQL_ENSURE(false, "Unexpected REDUCE mode");
        }

        TNodePtr sortDirection;
        auto sortKeySelector = OrderByGround_;
        FillSortParts(OrderBy_, sortDirection, sortKeySelector);
        if (!OrderBy_.empty()) {
            sortKeySelector = BuildLambda(Pos_, Y("row"), Y("SqlExtractKey", "row", sortKeySelector));
        }

        auto partitionByKey = Y(Mode_ == ReduceMode::ByAll ? "PartitionByKey" : "PartitionsByKeys", "core", extractKeyLambda,
            sortDirection, sortKeySelector, BuildLambda(Pos_, Y("partitionStream"), processPartitions));

        auto block(Y(Y("let", "core", input)));
        auto filter = Source_->BuildFilter(ctx, "core", FiltersGround_);
        if (filter) {
            block = L(block, Y("let", "core", filter));
        }
        block = L(block, Y("let", "core", Y("AutoDemux", partitionByKey)));
        if (Having_) {
            block = L(block, Y("let", "core",
                Y("Filter", "core", BuildLambda(Pos_, Y("row"), GroundWithExpr(HavingGround_, Y("Coalesce", Having_, Y("Bool", Q("false"))))))
            ));
        }
        return Y("block", Q(L(block, Y("return", "core"))));
    }

    TWriteSettings GetWriteSettings() const final {
        return Settings_;
    }

    TPtr DoClone() const final {
        return new TReduceSource(Pos_, Mode_, Source_->CloneSource(), CloneContainer(OrderBy_),
                CloneContainer(Keys_), CloneContainer(Args_), SafeClone(Udf_), SafeClone(Having_), Settings_);
    }
private:
    ReduceMode Mode_;
    TSourcePtr Source_;
    TVector<TSortSpecificationPtr> OrderBy_;
    TVector<TNodePtr> Keys_;
    TVector<TNodePtr> Args_;
    TNodePtr Udf_;
    TNodePtr Having_;
    const TWriteSettings Settings_;
    TNodePtr ExprGround_;
    TNodePtr FiltersGround_;
    TNodePtr OrderByGround_;
    TNodePtr HavingGround_;
};

TSourcePtr BuildReduce(TPosition pos,
    ReduceMode mode,
    TSourcePtr source,
    TVector<TSortSpecificationPtr>&& orderBy,
    TVector<TNodePtr>&& keys,
    TVector<TNodePtr>&& args,
    TNodePtr udf,
    TNodePtr having,
    const TWriteSettings& settings) {
    return new TReduceSource(pos, mode, std::move(source), std::move(orderBy), std::move(keys), std::move(args), udf, having, settings);
}

class TCompositeSelect: public IRealSource {
public:
    TCompositeSelect(TPosition pos, TSourcePtr source, const TWriteSettings& settings)
        : IRealSource(pos)
        , Source_(std::move(source))
        , Settings_(settings)
    {
        YQL_ENSURE(Source_);
    }

    void SetSubselects(TVector<TSourcePtr>&& subselects, TSet<TString>&& groupingCols) {
        Subselects_ = std::move(subselects);
        GroupingCols_ = std::move(groupingCols);
        Y_DEBUG_ABORT_UNLESS(Subselects_.size() > 1);
    }

    void GetInputTables(TTableList& tableList) const override {
        for (const auto& select: Subselects_) {
            select->GetInputTables(tableList);
        }
        ISource::GetInputTables(tableList);
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        if (AsInner_) {
            Source_->UseAsInner();
        }

        ctx.PushBlockShortcuts();
        if (src) {
            src->AddDependentSource(Source_.Get());
        }
        if (!Source_->Init(ctx, src)) {
            return false;
        }
        if (!Source_->InitFilters(ctx)) {
            return false;
        }
        FiltersGround_ = ctx.GroundBlockShortcuts(Pos_);
        for (const auto& select: Subselects_) {
            select->SetLabel(Label_);
            if (AsInner_) {
                select->UseAsInner();
            }

            if (!select->Init(ctx, Source_.Get())) {
                return false;
            }
        }
        return true;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        for (const auto& select: Subselects_) {
            if (!select->AddColumn(ctx, column)) {
                return {};
            }
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        auto input = Source_->Build(ctx);
        auto block(Y(Y("let", "composite", input)));
        auto filter = Source_->BuildFilter(ctx, "composite", FiltersGround_);
        if (filter) {
            block = L(block, Y("let", "composite", filter));
        }

        TNodePtr compositeNode = Y("UnionAll");
        for (const auto& select: Subselects_) {
            auto addNode = select->Build(ctx);
            if (!addNode) {
                return nullptr;
            }
            compositeNode->Add(addNode);
        }

        return GroundWithExpr(block, compositeNode);
    }

    bool IsGroupByColumn(const TString& column) const override {
        return GroupingCols_.contains(column);
    }

    const TSet<TString>& GetGroupingCols() const {
        return GroupingCols_;
    }

    TNodePtr BuildSort(TContext& ctx, const TString& label) override {
        return Subselects_.front()->BuildSort(ctx, label);
    }

    bool IsOrdered() const override {
        return Subselects_.front()->IsOrdered();
    }

    const TColumns* GetColumns() const override{
        return Subselects_.front()->GetColumns();
    }

    ISource* RealSource() const {
        return Source_.Get();
    }

    TWriteSettings GetWriteSettings() const override {
        return Settings_;
    }

    TNodePtr DoClone() const final {
        auto newSource = MakeIntrusive<TCompositeSelect>(Pos_, Source_->CloneSource(), Settings_);
        newSource->SetSubselects(CloneContainer(Subselects_), TSet<TString>(GroupingCols_));
        return newSource;
    }
private:
    TSourcePtr Source_;
    const TWriteSettings Settings_;
    TVector<TSourcePtr> Subselects_;
    TSet<TString> GroupingCols_;
    TNodePtr FiltersGround_;
};

/// \todo simplify class
class TSelectCore: public IRealSource {
public:
    TSelectCore(
        TPosition pos,
        TSourcePtr source,
        const TVector<TNodePtr>& groupByExpr,
        const TVector<TNodePtr>& groupBy,
        const TVector<TSortSpecificationPtr>& orderBy,
        TNodePtr having,
        TWinSpecs& winSpecs,
        THoppingWindowSpecPtr hoppingWindowSpec,
        const TVector<TNodePtr>& terms,
        bool distinct,
        const TVector<TNodePtr>& without,
        bool stream,
        const TWriteSettings& settings
    )
        : IRealSource(pos)
        , Source_(std::move(source))
        , GroupByExpr_(groupByExpr)
        , GroupBy_(groupBy)
        , OrderBy_(orderBy)
        , Having_(having)
        , WinSpecs_(winSpecs)
        , Terms_(terms)
        , Without_(without)
        , Distinct_(distinct)
        , HoppingWindowSpec_(hoppingWindowSpec)
        , Stream_(stream)
        , Settings_(settings)
    {
    }

    void GetInputTables(TTableList& tableList) const override {
        Source_->GetInputTables(tableList);
        ISource::GetInputTables(tableList);
    }

    bool IsComparableExpression(TContext& ctx, const TNodePtr& expr, const char* sqlConstruction) {
        if (expr->IsConstant()) {
            ctx.Error(expr->GetPos()) << "Unable to " << sqlConstruction << " constant expression";
            return false;
        }
        if (expr->IsAggregated() && !expr->HasState(ENodeState::AggregationKey)) {
            ctx.Error(expr->GetPos()) << "Unable to " << sqlConstruction << " aggregated values";
            return false;
        }
        if (expr->GetSourceName()) {
            return true;
        }
        if (expr->GetOpName().empty()) {
            ctx.Error(expr->GetPos()) << "You should use in " << sqlConstruction << " column name, qualified field, callable function or expression";
            return false;
        }
        return true;
    }

    bool DoInit(TContext& ctx, ISource* initSrc) override {
        if (AsInner_) {
            Source_->UseAsInner();
        }

        if (!Source_->Init(ctx, initSrc)) {
            return false;
        }
        if (Stream_ && !Source_->IsStream()) {
            ctx.Error(Pos_) << "SELECT STREAM is unsupported for non-streaming sources";
            return false;
        }
        if (!Stream_ && Source_->IsStream() && !ctx.PragmaDirectRead) {
            ctx.Error(Pos_) << "SELECT STREAM must be used for streaming sources";
            return false;
        }

        ctx.PushBlockShortcuts();
        auto src = Source_.Get();
        bool hasError = false;
        for (auto& expr: GroupByExpr_) {
            if (!expr->Init(ctx, src) || !IsComparableExpression(ctx, expr, "GROUP BY")) {
                hasError = true;
                continue;
            }
        }
        if (!src->AddExpressions(ctx, GroupByExpr_, EExprSeat::GroupBy)) {
            hasError = true;
        }
        GroupByExprGround_ = ctx.GroundBlockShortcuts(Pos_);
        /// grouped expressions are available in filters
        ctx.PushBlockShortcuts();
        if (!Source_->InitFilters(ctx)) {
            hasError = true;
        }
        FiltersGround_ = ctx.GroundBlockShortcuts(Pos_);
        const bool isJoin = Source_->GetJoin();
        for (auto& expr: GroupBy_) {
            if (!expr->Init(ctx, src)) {
                hasError = true;
                continue;
            }
            auto keyNamePtr = expr->GetColumnName();
            if (keyNamePtr && expr->GetLabel().empty()) {
                auto usedColumn = *keyNamePtr;
                auto sourceNamePtr = expr->GetSourceName();
                auto columnNode = dynamic_cast<TColumnNode*>(expr.Get());
                if (isJoin && (!columnNode || !columnNode->IsArtificial())) {
                    if (!sourceNamePtr || sourceNamePtr->empty()) {
                        ctx.Error(expr->GetPos()) << "Columns in GROUP BY should have correlation name, error in key: " << usedColumn;
                        hasError = true;
                        continue;
                    }
                    usedColumn = DotJoin(*sourceNamePtr, usedColumn);
                }
                if (!src->AddGroupKey(ctx, usedColumn)) {
                    hasError = true;
                    continue;
                }
            }
        }
        ctx.PushBlockShortcuts();
        if (Having_ && !Having_->Init(ctx, src)) {
            hasError = true;
        }
        HavingGround_ = ctx.GroundBlockShortcuts(Pos_);
        src->AddWindowSpecs(WinSpecs_);

        if (!InitSelect(ctx, src, isJoin, hasError)) {
            return false;
        }

        src->FinishColumns();
        Aggregate_ = src->BuildAggregation("core");
        if (src->IsFlattenByColumns() || src->IsFlattenColumns()) {
            Flatten_ = src->IsFlattenByColumns() ?
                src->BuildFlattenByColumns("row") :
                src->BuildFlattenColumns("row");
            if (!Flatten_ || !Flatten_->Init(ctx, src)) {
                hasError = true;
            }
        }
        if (GroupByExpr_) {
            auto sourcePreaggregate = src->BuildPreaggregatedMap(ctx);
            if (!sourcePreaggregate) {
                hasError = true;
            } else {
                PreaggregatedMap_ = !GroupByExprGround_ ? sourcePreaggregate :
                    Y("block", Q(L(GroupByExprGround_, Y("return", sourcePreaggregate))));
            }
        }
        if (Aggregate_) {
            if (!Aggregate_->Init(ctx, src)) {
                hasError = true;
            }
            if (Having_) {
                Aggregate_ = Y(
                    "Filter",
                    Aggregate_,
                    BuildLambda(Pos_, Y("row"), GroundWithExpr(HavingGround_, Y("Coalesce", Having_, Y("Bool", Q("false")))))
                );
            }
        } else if (Having_) {
            ctx.Error(Having_->GetPos()) << "HAVING with meaning GROUP BY () should be with aggregation function.";
            hasError = true;
        } else if (!Distinct_ && !GroupBy_.empty()) {
            ctx.Error(Pos_) << "No aggregations were specified";
            hasError = true;
        }
        if (hasError) {
            return false;
        }

        if (src->IsCalcOverWindow()) {
            if (src->IsExprSeat(EExprSeat::WindowPartitionBy, EExprType::WithExpression)) {
                PrewindowMap_ = src->BuildPrewindowMap(ctx, WinSpecsPartitionByGround_);
                if (!PrewindowMap_) {
                    hasError = true;
                }
            }
            CalcOverWindow_ = src->BuildCalcOverWindow(ctx, "core", WinSpecsOrderByGround_);
            if (!CalcOverWindow_) {
                hasError = true;
            }
        }
        if (hasError) {
            return false;
        }

        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        auto input = Source_->Build(ctx);
        if (!input) {
            return nullptr;
        }

        TNodePtr terms = BuildColumnsTerms(ctx);

        bool ordered = ctx.UseUnordered(*this);
        auto block(Y(Y("let", "core", input)));
        if (Flatten_) {
            block = L(block, Y("let", "core", Y(ordered ? "OrderedFlatMap" : "FlatMap", "core", BuildLambda(Pos_, Y("row"), Flatten_, "res"))));
        }
        if (PreaggregatedMap_) {
            block = L(block, Y("let", "core", Y("FlatMap", "core", BuildLambda(Pos_, Y("row"), PreaggregatedMap_))));
            if (Source_->IsCompositeSource() && !Columns_.QualifiedAll) {
                block = L(block, Y("let", "preaggregated", "core"));
            }
        } else if (Source_->IsCompositeSource() && !Columns_.QualifiedAll) {
            block = L(block, Y("let", "origcore", "core"));
        }
        auto filter = Source_->BuildFilter(ctx, "core", FiltersGround_);
        if (filter) {
            block = L(block, Y("let", "core", filter));
        }
        if (Aggregate_) {
            block = L(block, Y("let", "core", Aggregate_));
            ordered = false;
        }
        if (PrewindowMap_) {
            block = L(block, Y("let", "core", PrewindowMap_));
        }
        if (CalcOverWindow_) {
            block = L(block, Y("let", "core", CalcOverWindow_));
        }
        block = L(block, Y("let", "core", Y("EnsurePersistable", Y(ordered ? "OrderedFlatMap" : "FlatMap", "core", BuildLambda(Pos_, Y("row"), terms, "res")))));
        return Y("block", Q(L(block, Y("return", "core"))));
    }

    TNodePtr BuildSort(TContext& ctx, const TString& label) override {
        Y_UNUSED(ctx);
        if (OrderBy_.empty()) {
            return nullptr;
        }

        return Y("let", label, BuildSortSpec(OrderBy_, label, OrderByGround_));
    }

    bool IsSelect() const override {
        return true;
    }

    bool IsStream() const override {
        return Stream_;
    }

    bool IsOrdered() const override {
        return !OrderBy_.empty();
    }

    TWriteSettings GetWriteSettings() const override {
        return Settings_;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        if (OrderByInit_ && Source_->GetJoin()) {
            column.SetAsNotReliable();
            auto maybeExist = IRealSource::AddColumn(ctx, column);
            if (maybeExist && maybeExist.GetRef()) {
                return true;
            }
            return Source_->AddColumn(ctx, column);
        }
        return IRealSource::AddColumn(ctx, column);
    }

    TNodePtr PrepareWithout(const TNodePtr& base) {
        auto terms = base;
        if (Without_) {
            for (auto without: Without_) {
                auto name = *without->GetColumnName();
                if (Source_ && Source_->GetJoin()) {
                    name = DotJoin(*without->GetSourceName(), name);
                }
                terms = L(terms, Y("let", "row", Y("RemoveMember", "row", Q(name))));
            }
        }

        if (Source_) {
            for (auto column : Source_->GetTmpWindowColumns()) {
                terms = L(terms, Y("let", "row", Y("RemoveMember", "row", Q(column))));
            }
        }

        return terms;
    }

    TNodePtr DoClone() const final {
        TWinSpecs newSpecs;
        for (auto cur: WinSpecs_) {
            newSpecs.emplace(cur.first, cur.second->Clone());
        }
        return new TSelectCore(Pos_, Source_->CloneSource(), CloneContainer(GroupByExpr_),
                CloneContainer(GroupBy_), CloneContainer(OrderBy_), SafeClone(Having_), newSpecs, SafeClone(HoppingWindowSpec_),
                CloneContainer(Terms_), Distinct_, Without_, Stream_, Settings_);
    }

private:
    bool InitSelect(TContext& ctx, ISource* src, bool isJoin, bool& hasError) {
        for (auto iter: WinSpecs_) {
            auto winSpec = *iter.second;
            ctx.PushBlockShortcuts();
            for (auto& partitionNode: winSpec.Partitions) {
                auto invalidPartitionNodeFunc = [&]() {
                    ctx.Error(partitionNode->GetPos()) << "Expected either column name, either alias" <<
                        " or expression with alias for PARTITION BY expression in WINDOWS clause";
                    hasError = true;
                };
                if (!partitionNode->GetLabel() && !partitionNode->GetColumnName()) {
                    invalidPartitionNodeFunc();
                    continue;
                }
                if (!partitionNode->Init(ctx, src)) {
                    hasError = true;
                    continue;
                }
                if (!partitionNode->GetLabel() && !partitionNode->GetColumnName()) {
                    invalidPartitionNodeFunc();
                    continue;
                }
            }
            WinSpecsPartitionByGround_ = ctx.GroundBlockShortcuts(Pos_, WinSpecsPartitionByGround_);
            if (!src->AddExpressions(ctx, winSpec.Partitions, EExprSeat::WindowPartitionBy)) {
                hasError = true;
            }

            ctx.PushBlockShortcuts();
            for (auto orderSpec: winSpec.OrderBy) {
                if (!orderSpec->OrderExpr->Init(ctx, src)) {
                    hasError = true;
                }
            }
            WinSpecsOrderByGround_ = ctx.GroundBlockShortcuts(Pos_, WinSpecsOrderByGround_);
        }

        if (HoppingWindowSpec_) {
            ctx.PushBlockShortcuts();
            if (!HoppingWindowSpec_->TimeExtractor->Init(ctx, src)) {
                hasError = true;
            }
            HoppingWindowSpec_->TimeExtractor = ctx.GroundBlockShortcutsForExpr(HoppingWindowSpec_->TimeExtractor);
            src->SetHoppingWindowSpec(HoppingWindowSpec_);
        }

        ctx.PushBlockShortcuts();
        for (auto& term: Terms_) {
            if (!term->Init(ctx, src)) {
                hasError = true;
                continue;
            }
            auto column = term->GetColumnName();
            if (Distinct_) {
                if (!column) {
                    ctx.Error(Pos_) << "SELECT DISTINCT requires a list of column references";
                    hasError = true;
                    continue;
                }
                if (term->IsAsterisk()) {
                    ctx.Error(Pos_) << "SELECT DISTINCT * is not implemented yet";
                    hasError = true;
                    continue;
                }
                auto columnName = *column;
                if (isJoin) {
                    auto sourceNamePtr = term->GetSourceName();
                    if (!sourceNamePtr || sourceNamePtr->empty()) {
                        if (src->IsGroupByColumn(columnName)) {
                            ctx.Error(term->GetPos()) << ErrorDistinctByGroupKey(columnName);
                            hasError = true;
                            continue;
                        } else {
                            ctx.Error(term->GetPos()) << ErrorDistinctWithoutCorrelation(columnName);
                            hasError = true;
                            continue;
                        }
                    }
                    columnName = DotJoin(*sourceNamePtr, columnName);
                }
                if (src->IsGroupByColumn(columnName)) {
                    ctx.Error(term->GetPos()) << ErrorDistinctByGroupKey(columnName);
                    hasError = true;
                    continue;
                }
                if (!src->AddGroupKey(ctx, columnName)) {
                    hasError = true;
                    continue;
                }
                GroupBy_.push_back(BuildColumn(Pos_, columnName));
            }
            TString label(term->GetLabel());
            bool hasName = true;
            if (label.empty()) {
                auto source = term->GetSourceName();
                if (term->IsAsterisk() && !source->empty()) {
                    Columns_.QualifiedAll = true;
                    label = DotJoin(*source, "*");
                } else if (column) {
                    label = isJoin && source && *source ? DotJoin(*source, *column) : *column;
                } else {
                    label = TStringBuilder() << "column" << Columns_.List.size();
                    hasName = false;
                }
            }
            if (!Columns_.Add(&label, false, false, true, hasName)) {
                ctx.Error(Pos_) << "Duplicate column: " << label;
                hasError = true;
                continue;
            }
        }
        TermsGround_ = ctx.GroundBlockShortcuts(Pos_);

        if (Columns_.All || Columns_.QualifiedAll) {
            Source_->AllColumns();
            if (Columns_.All && isJoin && ctx.SimpleColumns) {
                Columns_.All = false;
                Columns_.QualifiedAll = true;
                const auto pos = Terms_.front()->GetPos();
                Terms_.clear();
                for (const auto& source: Source_->GetJoin()->GetJoinLabels()) {
                    auto withDot = DotJoin(source, "*");
                    Columns_.Add(&withDot, false);
                    Terms_.push_back(BuildColumn(pos, "*", source));
                }
            }
        }
        for (const auto& without: Without_) {
            auto namePtr = without->GetColumnName();
            auto sourcePtr = without->GetSourceName();
            YQL_ENSURE(namePtr && *namePtr);
            if (isJoin && !(sourcePtr && *sourcePtr)) {
                ctx.Error(without->GetPos()) << "Expected correlation name for WITHOUT in JOIN";
                hasError = true;
                continue;
            }
        }
        if (Having_ && !Having_->Init(ctx, src)) {
            hasError = true;
        }
        if (!src->IsCompositeSource() && !Distinct_ && !Columns_.All && src->HasAggregations()) {
            /// verify select aggregation compatibility
            TVector<TNodePtr> exprs(Terms_);
            if (Having_) {
                exprs.push_back(Having_);
            }
            for (const auto& iter: WinSpecs_) {
                for (const auto& sortSpec: iter.second->OrderBy) {
                    exprs.push_back(sortSpec->OrderExpr);
                }
            }
            if (!ValidateAllNodesForAggregation(ctx, exprs)) {
                hasError = true;
            }
        }
        const auto label = GetLabel();
        ctx.PushBlockShortcuts();
        for (const auto& sortSpec: OrderBy_) {
            auto& expr = sortSpec->OrderExpr;
            SetLabel(Source_->GetLabel());
            OrderByInit_ = true;
            if (!expr->Init(ctx, this)) {
                hasError = true;
                continue;
            }
            OrderByInit_ = false;
            if (!IsComparableExpression(ctx, expr, "ORDER BY")) {
                hasError = true;
                continue;
            }
        }
        OrderByGround_ = ctx.GroundBlockShortcuts(Pos_);
        SetLabel(label);

        return true;
    }

    TNodePtr BuildColumnsTerms(TContext& ctx) {
        TNodePtr terms;
        if (Columns_.All) {
            Y_DEBUG_ABORT_UNLESS(Columns_.List.empty());
            terms = PrepareWithout(Y());
            if (ctx.EnableSystemColumns) {
                terms = L(terms, Y("let", "res", Y("AsList", Y("RemoveSystemMembers", "row"))));
            } else {
                terms = L(terms, (Y("let", "res", Y("AsList", "row"))));
            }
        } else if (!Columns_.List.empty()) {
            Y_DEBUG_ABORT_UNLESS(Columns_.List.size() == Terms_.size());
            const bool isJoin = Source_->GetJoin();

            terms = TermsGround_ ? TermsGround_ : Y();
            if (Source_->IsCompositeSource() && !Columns_.QualifiedAll) {
                auto compositeSrcPtr = static_cast<TCompositeSelect*>(Source_->GetCompositeSource());
                if (compositeSrcPtr) {
                    const auto& groupings = compositeSrcPtr->GetGroupingCols();
                    for (const auto& column: groupings) {
                        bool isAggregated = false;
                        for (const auto& group: GroupBy_) {
                            const auto columnName = group->GetColumnName();
                            if (columnName && *columnName == column) {
                                isAggregated = true;
                                break;
                            }
                        }
                        if (isAggregated) {
                            continue;
                        }
                        const TString tableName = PreaggregatedMap_ ? "preaggregated" : "origcore";
                        terms = L(terms, Y("let", "row", Y("AddMember", "row", BuildQuotedAtom(Pos_, column), Y("Nothing", Y("MatchType",
                            Y("StructMemberType", Y("ListItemType", Y("TypeOf", tableName)), Q(column)),
                            Q("Optional"), Y("lambda", Q(Y("item")), "item"), Y("lambda", Q(Y("item")), Y("OptionalType", "item")))))));
                    }
                }
            }

            TNodePtr structObj = nullptr;
            auto column = Columns_.List.begin();
            for (auto& term: Terms_) {
                if (!term->IsAsterisk()) {
                    if (!structObj) {
                        structObj = Y("AsStruct");
                    }
                    structObj = L(structObj, Q(Y(BuildQuotedAtom(Pos_, *column), term)));
                }
                ++column;
            }
            terms = structObj ? L(terms, Y("let", "res", structObj)) : Y(Y("let", "res", Y("AsStruct")));
            terms = PrepareWithout(terms);
            if (Columns_.QualifiedAll) {
                if (ctx.SimpleColumns && !isJoin) {
                    terms = L(terms, Y("let", "res", Y("FlattenMembers", Q(Y(BuildQuotedAtom(Pos_, ""), "res")),
                        Q(Y(BuildQuotedAtom(Pos_, ""), "row")))));
                } else {
                    if (isJoin && ctx.SimpleColumns) {
                        const auto& sameKeyMap = Source_->GetJoin()->GetSameKeysMap();
                        if (sameKeyMap) {
                            terms = L(terms, Y("let", "flatSameKeys", "row"));
                            for (const auto& sameKeysPair: sameKeyMap) {
                                const auto& column = sameKeysPair.first;
                                auto keys = Y("Coalesce");
                                auto sameSourceIter = sameKeysPair.second.begin();
                                for (auto end = sameKeysPair.second.end(); sameSourceIter != end; ++sameSourceIter) {
                                    auto addKeyNode = Q(DotJoin(*sameSourceIter, column));
                                    keys = L(keys, Y("TryMember", "row", addKeyNode, Y("Null")));
                                }

                                terms = L(terms, Y("let", "flatSameKeys", Y("AddMember", "flatSameKeys", Q(column), keys)));
                                sameSourceIter = sameKeysPair.second.begin();
                                for (auto end = sameKeysPair.second.end(); sameSourceIter != end; ++sameSourceIter) {
                                    auto removeKeyNode = Q(DotJoin(*sameSourceIter, column));
                                    terms = L(terms, Y("let", "flatSameKeys", Y("ForceRemoveMember", "flatSameKeys", removeKeyNode)));
                                }
                            }
                            terms = L(terms, Y("let", "row", "flatSameKeys"));
                        }
                    }

                    auto members = isJoin ? Y() : Y("FlattenMembers");
                    for (auto& term: Terms_) {
                        if (term->IsAsterisk()) {
                            auto sourceName = term->GetSourceName();
                            YQL_ENSURE(*sourceName && !sourceName->empty());
                            if (isJoin) {
                                members = L(members, BuildQuotedAtom(Pos_, *sourceName + "."));
                            } else {
                                auto prefix = ctx.SimpleColumns ? "" : *sourceName + ".";
                                members = L(members, Q(Y(Q(prefix), "row")));
                            }
                        }
                    }
                    if (isJoin) {
                        members = Y(ctx.SimpleColumns ? "DivePrefixMembers" : "SelectMembers", "row", Q(members));
                    }
                    terms = L(terms, Y("let", "res", Y("FlattenMembers", Q(Y(BuildQuotedAtom(Pos_, ""), "res")),
                        Q(Y(BuildQuotedAtom(Pos_, ""), members)))));
                    if (isJoin && ctx.SimpleColumns) {
                        for (const auto& sameKeysPair: Source_->GetJoin()->GetSameKeysMap()) {
                            const auto& column = sameKeysPair.first;
                            auto addMemberKeyNode = Y("Member", "row", Q(column));
                            terms = L(terms, Y("let", "res", Y("AddMember", "res", Q(column), addMemberKeyNode)));
                        }
                    }
                }
            }
            terms = L(terms, Y("let", "res", Y("AsList", "res")));
        }
        return terms;
    }

private:
    TSourcePtr Source_;
    TVector<TNodePtr> GroupByExpr_;
    TVector<TNodePtr> GroupBy_;
    TVector<TSortSpecificationPtr> OrderBy_;
    TNodePtr Having_;
    TWinSpecs WinSpecs_;
    TNodePtr Flatten_;
    TNodePtr PreaggregatedMap_;
    TNodePtr PrewindowMap_;
    TNodePtr Aggregate_;
    TNodePtr CalcOverWindow_;
    TNodePtr FiltersGround_;
    TNodePtr TermsGround_;
    TNodePtr GroupByExprGround_;
    TNodePtr HavingGround_;
    TNodePtr OrderByGround_;
    TNodePtr WinSpecsPartitionByGround_;
    TNodePtr WinSpecsOrderByGround_;
    TVector<TNodePtr> Terms_;
    TVector<TNodePtr> Without_;
    const bool Distinct_;
    bool OrderByInit_ = false;
    THoppingWindowSpecPtr HoppingWindowSpec_;
    const bool Stream_;
    const TWriteSettings Settings_;
};

class TProcessSource: public IRealSource {
public:
    TProcessSource(
        TPosition pos,
        TSourcePtr source,
        TNodePtr with,
        TVector<TNodePtr>&& terms,
        bool listCall,
        bool stream,
        const TWriteSettings& settings
    )
        : IRealSource(pos)
        , Source_(std::move(source))
        , With_(with)
        , Terms_(std::move(terms))
        , ListCall_(listCall)
        , Stream_(stream)
        , Settings_(settings)
    {
    }

    void GetInputTables(TTableList& tableList) const override {
        Source_->GetInputTables(tableList);
        ISource::GetInputTables(tableList);
    }

    bool DoInit(TContext& ctx, ISource* initSrc) override {
        if (AsInner_) {
            Source_->UseAsInner();
        }

        if (!Source_->Init(ctx, initSrc)) {
            return false;
        }

        if (Stream_ && !Source_->IsStream()) {
            ctx.Error(Pos_) << "PROCESS STREAM is unsupported for non-streaming sources";
            return false;
        }

        if (!Stream_ && Source_->IsStream() && !ctx.PragmaDirectRead) {
            ctx.Error(Pos_) << "PROCESS STREAM must be used for streaming sources";
            return false;
        }

        auto src = Source_.Get();
        if (!With_) {
            src->AllColumns();
            Columns_.SetAll();
            src->FinishColumns();
            return true;
        }

        /// grouped expressions are available in filters
        ctx.PushBlockShortcuts();
        if (!Source_->InitFilters(ctx)) {
            return false;
        }
        FiltersGround_ = ctx.GroundBlockShortcuts(Pos_);

        // Use fake source in case of list process to restrict column access.
        TSourcePtr fakeSource;
        if (ListCall_) {
            fakeSource = BuildFakeSource(src->GetPos());
            src->AllColumns();
        }

        auto processSource = ListCall_ ? fakeSource.Get() : src;
        Y_DEBUG_ABORT_UNLESS(processSource != nullptr);

        ctx.PushBlockShortcuts();
        if (!With_->Init(ctx, processSource)) {
            return false;
        }
        if (With_->GetLabel().empty()) {
            Columns_.SetAll();
        } else {
            if (ListCall_) {
                ctx.Error(With_->GetPos()) << "Label is not allowed to use with $ROWS";
                return false;
            }

            Columns_.Add(&With_->GetLabel(), false);
        }

        bool hasError = false;
        auto produce = Y(ListCall_ ? "SqlProcess" : "Apply", With_);
        TMaybe<ui32> listPosIndex;
        ui32 termIndex = 0;
        for (auto& term: Terms_) {
            if (ListCall_) {
                if (auto atom = dynamic_cast<TAstAtomNode*>(term.Get())) {
                    if (atom->GetContent() == "inputRowsList") {
                        listPosIndex = termIndex;
                    }
                }
            }
            ++termIndex;

            if (!term->GetLabel().empty()) {
                ctx.Error(term->GetPos()) << "Labels are not allowed for PROCESS terms";
                hasError = true;
                continue;
            }

            if (!term->Init(ctx, processSource)) {
                hasError = true;
                continue;
            }

            produce = L(produce, term);
        }

        if (ListCall_) {
            produce = L(produce, Q(ToString(*listPosIndex)));
        }

        if (!produce->Init(ctx, src)) {
            hasError = true;
        }
        produce = ctx.GroundBlockShortcutsForExpr(produce);

        TVector<TNodePtr>(1, produce).swap(Terms_);

        src->FinishColumns();

        if (hasError) {
            return false;
        }

        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        auto input = Source_->Build(ctx);
        if (!input) {
            return nullptr;
        }

        if (!With_) {
            return input;
        }

        TString inputLabel = ListCall_ ? "inputRowsList" : "core";

        auto block(Y(Y("let", inputLabel, input)));

        auto filter = Source_->BuildFilter(ctx, inputLabel, FiltersGround_);
        if (filter) {
            block = L(block, Y("let", inputLabel, filter));
        }

        if (ListCall_) {
            block = L(block, Y("let", "core", Terms_[0]));
        } else {
            auto terms = BuildColumnsTerms(ctx);
            block = L(block, Y("let", "core", Y(ctx.UseUnordered(*this) ? "OrderedFlatMap" : "FlatMap", "core", BuildLambda(Pos_, Y("row"), terms, "res"))));
        }
        block = L(block, Y("let", "core", Y("AutoDemux", Y("EnsurePersistable", "core"))));
        return Y("block", Q(L(block, Y("return", "core"))));
    }

    bool IsSelect() const override {
        return false;
    }

    bool IsStream() const override {
        return Stream_;
    }

    TWriteSettings GetWriteSettings() const override {
        return Settings_;
    }

    TNodePtr DoClone() const final {
        return new TProcessSource(Pos_, Source_->CloneSource(), SafeClone(With_),
            CloneContainer(Terms_), ListCall_, Stream_, Settings_);
    }

private:
    TNodePtr BuildColumnsTerms(TContext& ctx) {
        Y_UNUSED(ctx);
        TNodePtr terms;
        Y_DEBUG_ABORT_UNLESS(Terms_.size() == 1);
        if (Columns_.All) {
            terms = Y(Y("let", "res", Y("ToSequence", Terms_.front())));
        } else {
            Y_DEBUG_ABORT_UNLESS(Columns_.List.size() == Terms_.size());
            terms = TermsGround_ ? TermsGround_ : Y();
            terms = L(terms, Y("let", "res",
                L(Y("AsStruct"), Q(Y(BuildQuotedAtom(Pos_, Columns_.List.front()), Terms_.front())))));
            terms = L(terms, Y("let", "res", Y("Just", "res")));
        }
        return terms;
    }

private:
    TSourcePtr Source_;
    TNodePtr With_;
    TNodePtr FiltersGround_;
    TNodePtr TermsGround_;
    TVector<TNodePtr> Terms_;
    const bool ListCall_;
    const bool Stream_;
    const TWriteSettings Settings_;
};

TSourcePtr BuildProcess(
    TPosition pos,
    TSourcePtr source,
    TNodePtr with,
    TVector<TNodePtr>&& terms,
    bool listCall,
    bool stream,
    const TWriteSettings& settings
) {
    return new TProcessSource(pos, std::move(source), with, std::move(terms), listCall, stream, settings);
}

class TNestedProxySource: public IProxySource {
public:
    TNestedProxySource(TPosition pos, const TVector<TNodePtr>& groupBy, TSourcePtr source)
        : IProxySource(pos, source.Get())
        , CompositeSelect_(nullptr)
        , Holder_(std::move(source))
        , GroupBy_(groupBy)
    {}

    TNestedProxySource(TCompositeSelect* compositeSelect, const TVector<TNodePtr>& groupBy)
        : IProxySource(compositeSelect->GetPos(), compositeSelect->RealSource())
        , CompositeSelect_(compositeSelect)
        , GroupBy_(groupBy)
    {}

    bool DoInit(TContext& ctx, ISource* src) override {
        return Source_->Init(ctx, src);
    }

    TNodePtr Build(TContext& ctx) override  {
        return CompositeSelect_ ? BuildAtom(Pos_, "composite", TNodeFlags::Default) : Source_->Build(ctx);
    }

    bool InitFilters(TContext& ctx) override {
        return CompositeSelect_ ? true : Source_->InitFilters(ctx);
    }

    TNodePtr BuildFilter(TContext& ctx, const TString& label, const TNodePtr& groundNode) override {
        return CompositeSelect_ ? nullptr : Source_->BuildFilter(ctx, label, groundNode);
    }

    bool IsCompositeSource() const override {
        return true;
    }

    ISource* GetCompositeSource() override {
        return CompositeSelect_;
    }

    bool CalculateGroupingHint(TContext& ctx, const TVector<TString>& columns, ui64& hint) const override {
        Y_UNUSED(ctx);
        hint = 0;
        if (GroupByColumns_.empty()) {
            for (const auto& groupByNode: GroupBy_) {
                auto namePtr = groupByNode->GetColumnName();
                YQL_ENSURE(namePtr);
                GroupByColumns_.insert(*namePtr);
            }
        }
        for (const auto& column: columns) {
            hint <<= 1;
            if (!GroupByColumns_.contains(column)) {
                hint += 1;
            }
        }
        return true;
    }

    void FinishColumns() override {
        Source_->FinishColumns();
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        return Source_->AddColumn(ctx, column);
    }

    TPtr DoClone() const final {
        return Holder_.Get() ? new TNestedProxySource(Pos_, CloneContainer(GroupBy_), Holder_->CloneSource()) :
            new TNestedProxySource(CompositeSelect_, CloneContainer(GroupBy_));
    }

private:
    TCompositeSelect* CompositeSelect_;
    TSourcePtr Holder_;
    TVector<TNodePtr> GroupBy_;
    mutable TSet<TString> GroupByColumns_;
};

TSourcePtr BuildSelectCore(
    TContext& ctx,
    TPosition pos,
    TSourcePtr source,
    const TVector<TNodePtr>& groupByExpr,
    const TVector<TNodePtr>& groupBy,
    const TVector<TSortSpecificationPtr>& orderBy,
    TNodePtr having,
    TWinSpecs&& winSpecs,
    THoppingWindowSpecPtr hoppingWindowSpec,
    TVector<TNodePtr>&& terms,
    bool distinct,
    TVector<TNodePtr>&& without,
    bool stream,
    const TWriteSettings& settings
) {
    if (groupBy.empty() || !groupBy.front()->ContentListPtr()) {
        return new TSelectCore(pos, std::move(source), groupByExpr, groupBy, orderBy, having, winSpecs, hoppingWindowSpec, terms, distinct, without, stream, settings);
    }
    if (groupBy.size() == 1) {
        /// actualy no big idea to use grouping function in this case (result allways 0)
        auto contentPtr = groupBy.front()->ContentListPtr();
        TSourcePtr proxySource = new TNestedProxySource(pos, *contentPtr, std::move(source));
        return BuildSelectCore(ctx, pos, std::move(proxySource), groupByExpr, *contentPtr, orderBy, having, std::move(winSpecs),
            hoppingWindowSpec, std::move(terms), distinct, std::move(without), stream, settings);
    }
    /// \todo some smart merge logic, generalize common part of grouping (expr, flatten, etc)?
    TIntrusivePtr<TCompositeSelect> compositeSelect = new TCompositeSelect(pos, std::move(source), settings);
    size_t totalGroups = 0;
    TVector<TSourcePtr> subselects;
    TSet<TString> groupingCols;
    for (auto& grouping: groupBy) {
        auto contentPtr = grouping->ContentListPtr();
        TVector<TNodePtr> cache(1, nullptr);
        if (!contentPtr) {
            cache[0] = grouping;
            contentPtr = &cache;
        }
        for (const auto& elem: *contentPtr) {
            auto namePtr = elem->GetColumnName();
            if (namePtr && !namePtr->empty()) {
                groupingCols.insert(*namePtr);
            }
        }
        TSourcePtr proxySource = new TNestedProxySource(compositeSelect.Get(), *contentPtr);
        if (!subselects.empty()) {
            /// clone terms for others usage
            TVector<TNodePtr> termsCopy;
            for (const auto& term: terms) {
                termsCopy.emplace_back(term->Clone());
            }
            std::swap(terms, termsCopy);
        }
        totalGroups += contentPtr->size();
        TSelectCore* selectCore = new TSelectCore(pos, std::move(proxySource), CloneContainer(groupByExpr),
            *contentPtr, orderBy, SafeClone(having), winSpecs, hoppingWindowSpec, terms, distinct, without, stream, settings);
        subselects.emplace_back(selectCore);
    }
    if (totalGroups > ctx.PragmaGroupByLimit) {
        ctx.Error(pos) << "Unable to GROUP BY more than " << ctx.PragmaGroupByLimit << " groups, you try use " << totalGroups << " groups";
        return nullptr;
    }
    compositeSelect->SetSubselects(std::move(subselects), std::move(groupingCols));
    return compositeSelect;
}

class TUnionAll: public IRealSource {
public:
    TUnionAll(TPosition pos, TVector<TSourcePtr>&& sources)
        : IRealSource(pos)
        , Sources_(std::move(sources))
    {
    }

    const TColumns* GetColumns() const override {
        return IRealSource::GetColumns();
    }

    void GetInputTables(TTableList& tableList) const override {
        for (auto& x : Sources_) {
            x->GetInputTables(tableList);
        }

        ISource::GetInputTables(tableList);
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        for (auto& s: Sources_) {
            s->UseAsInner();
            if (!s->Init(ctx, src)) {
                return false;
            }
            auto c = s->GetColumns();
            Y_DEBUG_ABORT_UNLESS(c);
            Columns_.Merge(*c);
        }
        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        auto res = Y("UnionAll");
        for (auto& s: Sources_) {
            auto input = s->Build(ctx);
            if (!input) {
                return nullptr;
            }
            res->Add(input);
        }
        return res;
    }


    bool IsStream() const override {
        for (auto& s: Sources_) {
            if (!s->IsStream()) {
                return false;
            }
        }
        return true;
    }

    TNodePtr DoClone() const final {
        return MakeIntrusive<TUnionAll>(Pos_, CloneContainer(Sources_));
    }

private:
    TVector<TSourcePtr> Sources_;
};

TSourcePtr BuildUnionAll(TPosition pos, TVector<TSourcePtr>&& sources) {
    return new TUnionAll(pos, std::move(sources));
}

class TOverWindowSource: public IProxySource {
public:
    TOverWindowSource(TPosition pos, const TString& windowName, ISource* origSource)
        : IProxySource(pos, origSource)
        , WindowName_(windowName)
    {
        Source_->SetLabel(origSource->GetLabel());
    }

    TString MakeLocalName(const TString& name) override {
        return Source_->MakeLocalName(name);
    }

    void AddTmpWindowColumn(const TString& column) override {
        return Source_->AddTmpWindowColumn(column);
    }

    bool AddAggregation(TContext& ctx, TAggregationPtr aggr) override {
        if (aggr->IsOverWindow()) {
            return Source_->AddAggregationOverWindow(ctx, WindowName_, aggr);
        }
        return Source_->AddAggregation(ctx, aggr);
    }

    bool AddFuncOverWindow(TContext& ctx, TNodePtr expr) override {
        return Source_->AddFuncOverWindow(ctx, WindowName_, expr);
    }

    bool IsOverWindowSource() const override {
        return true;
    }

    TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column) override {
        return Source_->AddColumn(ctx, column);
    }

    TNodePtr Build(TContext& ctx) override {
        Y_UNUSED(ctx);
        Y_ABORT("Unexpected call");
    }

    const TString* GetWindowName() const override {
        return &WindowName_;
    }

    TWindowSpecificationPtr FindWindowSpecification(TContext& ctx, const TString& windowName) const override {
        return Source_->FindWindowSpecification(ctx, windowName);
    }

    TNodePtr DoClone() const final {
        return {};
    }

private:
    const TString WindowName_;
};

TSourcePtr BuildOverWindowSource(TPosition pos, const TString& windowName, ISource* origSource) {
    return new TOverWindowSource(pos, windowName, origSource);
}

class TSkipTakeNode final: public TAstListNode {
public:
    TSkipTakeNode(TPosition pos, const TNodePtr& skip, const TNodePtr& take)
        : TAstListNode(pos)
    {
        TNodePtr select(AstNode("select"));
        if (skip) {
            select = Y("Skip", select, skip);
        }
        Add("let", "select", Y("Take", select, take));
    }

    TPtr DoClone() const final {
        return {};
    }
};

TNodePtr BuildSkipTake(TPosition pos, const TNodePtr& skip, const TNodePtr& take) {
    return new TSkipTakeNode(pos, skip, take);
}

class TSelect: public IProxySource {
public:
    TSelect(TPosition pos, TSourcePtr source, TNodePtr skipTake)
        : IProxySource(pos, source.Get())
        , Source_(std::move(source))
        , SkipTake_(skipTake)
    {}

    bool DoInit(TContext& ctx, ISource* src) override {
        Source_->SetLabel(Label_);
        if (AsInner_) {
            Source_->UseAsInner();
        }

        if (!Source_->Init(ctx, src)) {
            return false;
        }
        src = Source_.Get();
        if (SkipTake_) {
            ctx.PushBlockShortcuts();
            FakeSource_.Reset(new TFakeSource(SkipTake_->GetPos()));
            if (!SkipTake_->Init(ctx, FakeSource_.Get())) {
                return false;
            }

            SkipTakeGround_ = ctx.GroundBlockShortcuts(ctx.Pos());
        }

        return true;
    }

    TNodePtr Build(TContext& ctx) override {
        auto input = Source_->Build(ctx);
        if (!input) {
            return nullptr;
        }
        const auto label = "select";
        auto block(Y(Y("let", label, input)));

        auto sortNode = Source_->BuildSort(ctx, label);
        if (sortNode) {
            if (AsInner_ && !SkipTake_) {
                ctx.Warning(sortNode->GetPos(), TIssuesIds::YQL_ORDER_BY_WITHOUT_LIMIT_IN_SUBQUERY) << "ORDER BY without LIMIT in subquery will be ignored";
            } else {
                block = L(block, sortNode);
            }
        }

        if (SkipTake_) {
            if (SkipTakeGround_) {
                block = L(block, SkipTake_->Y("let", "select", SkipTake_->Y("block", SkipTake_->Q(
                    SkipTake_->L(SkipTake_->L(SkipTakeGround_, SkipTake_), Y("return", "select"))))));
            } else {
                block = L(block, SkipTake_);
            }
        }
        block = L(block, Y("return", label));
        return Y("block", Q(block));
    }

    bool IsSelect() const override {
        return Source_->IsSelect();
    }

    TPtr DoClone() const final {
        return MakeIntrusive<TSelect>(Pos_, Source_->CloneSource(), SafeClone(SkipTake_));
    }
protected:
    TSourcePtr Source_;
    TNodePtr SkipTake_;
    TNodePtr SkipTakeGround_;
    THolder<TFakeSource> FakeSource_;
};

TSourcePtr BuildSelect(TPosition pos, TSourcePtr source, TNodePtr skipTake) {
    return new TSelect(pos, std::move(source), skipTake);
}

class TSelectResultNode final: public TAstListNode {
public:
    TSelectResultNode(TPosition pos, TSourcePtr source, bool writeResult, bool inSubquery)
        : TAstListNode(pos)
        , Source_(std::move(source))
        , WriteResult_(writeResult)
        , InSubquery_(inSubquery)
    {
        YQL_ENSURE(Source_, "Invalid source node");
        FakeSource_ = BuildFakeSource(pos);
    }

    bool IsSelect() const override {
        return true;
    }

    bool DoInit(TContext& ctx, ISource* src) override {
        if (!Source_->Init(ctx, src)) {
            return false;
        }

        src = Source_.Get();
        TTableList tableList;
        Source_->GetInputTables(tableList);

        TNodePtr node(BuildInputTables(Pos_, tableList, InSubquery_));
        if (!node->Init(ctx, src)) {
            return false;
        }

        TSet<TString> clusters;
        for (auto& it: tableList) {
            clusters.insert(it.Cluster);
        }

        auto writeSettings = src->GetWriteSettings();
        bool asRef = ctx.PragmaRefSelect;
        bool asAutoRef = true;
        if (ctx.PragmaSampleSelect) {
            asRef = false;
            asAutoRef = false;
        }

        auto settings = Y(Q(Y(Q("type"))));
        if (writeSettings.Discard) {
            settings = L(settings, Q(Y(Q("discard"))));
        }

        if (!writeSettings.Label.Empty()) {
            auto labelNode = writeSettings.Label.Build();
            if (!writeSettings.Label.GetLiteral()) {
                labelNode = Y("EvaluateAtom", labelNode);
            }

            ctx.PushBlockShortcuts();
            if (!labelNode->Init(ctx, FakeSource_.Get())) {
                return false;
            }

            labelNode = ctx.GroundBlockShortcutsForExpr(labelNode);
            settings = L(settings, Q(Y(Q("label"), labelNode)));
        }

        if (asRef) {
            settings = L(settings, Q(Y(Q("ref"))));
        } else if (asAutoRef) {
            settings = L(settings, Q(Y(Q("autoref"))));
        }

        auto columns = Source_->GetColumns();
        if (columns && !columns->All && !(columns->QualifiedAll && ctx.SimpleColumns)) {
            auto list = Y();
            for (auto& c: columns->List) {
                if (c.EndsWith('*')) {
                    list = L(list, Q(Y(Q("prefix"), BuildQuotedAtom(Pos_, c.substr(0, c.size() - 1)))));
                } else {
                    list = L(list, BuildQuotedAtom(Pos_, c));
                }
            }
            settings = L(settings, Q(Y(Q("columns"), Q(list))));
        }

        if (ctx.ResultRowsLimit > 0) {
            settings = L(settings, Q(Y(Q("take"), Q(ToString(ctx.ResultRowsLimit)))));
        }

        auto output = Source_->Build(ctx);
        if (!output) {
            return false;
        }
        node = L(node, Y("let", "output", output));
        if (WriteResult_) {
            if (!Source_->IsOrdered() && ctx.UseUnordered(*Source_)) {
                node = L(node, Y("let", "output", Y("Unordered", "output")));
            }
            auto writeResult(BuildWriteResult(Pos_, "output", settings, clusters));
            if (!writeResult->Init(ctx, src)) {
                return false;
            }
            node = L(node, Y("let", "world", writeResult));
            node = L(node, Y("return", "world"));
        } else {
            node = L(node, Y("return", "output"));
        }

        Add("block", Q(node));
        return true;
    }

    TPtr DoClone() const final {
        return {};
    }
protected:
    TSourcePtr Source_;

    const bool WriteResult_;
    const bool InSubquery_;
    TSourcePtr FakeSource_;
};

TNodePtr BuildSelectResult(TPosition pos, TSourcePtr source, bool writeResult, bool inSubquery) {
    return new TSelectResultNode(pos, std::move(source), writeResult, inSubquery);
}

} // namespace NSQLTranslationV0
