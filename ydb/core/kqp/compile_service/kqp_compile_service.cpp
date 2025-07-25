#include "kqp_compile_service.h"

#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/core/base/appdata.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/core/cms/console/console.h>
#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/common/kqp_lwtrace_probes.h>
#include <ydb/core/kqp/common/simple/query_ast.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/core/kqp/host/kqp_translate.h>
#include <ydb/library/aclib/aclib.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/actors/core/hfunc.h>
#include <library/cpp/cache/cache.h>

#include <util/string/escape.h>

LWTRACE_USING(KQP_PROVIDER);

namespace NKikimr {
namespace NKqp {

using namespace NKikimrConfig;
using namespace NYql;


struct TKqpCompileSettings {
    TKqpCompileSettings(bool keepInCache, bool isQueryActionPrepare, bool perStatementResult,
        const TInstant& deadline, ECompileActorAction action = ECompileActorAction::COMPILE)
        : KeepInCache(keepInCache)
        , IsQueryActionPrepare(isQueryActionPrepare)
        , PerStatementResult(perStatementResult)
        , Deadline(deadline)
        , Action(action)
    {}

    bool KeepInCache;
    bool IsQueryActionPrepare;
    bool PerStatementResult;
    TInstant Deadline;
    ECompileActorAction Action;
};

struct TKqpCompileRequest {
    TKqpCompileRequest(const TActorId& sender, const TString& uid, TKqpQueryId query, const TKqpCompileSettings& compileSettings,
        const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, const TString& clientAddress, TKqpDbCountersPtr dbCounters, const TGUCSettings::TPtr& gUCSettings,
        const TMaybe<TString>& applicationName, ui64 cookie, std::shared_ptr<std::atomic<bool>> intrestedInResult,
        const TIntrusivePtr<TUserRequestContext>& userRequestContext, NLWTrace::TOrbit orbit = {}, NWilson::TSpan span = {},
        TKqpTempTablesState::TConstPtr tempTablesState = {},
        TMaybe<TQueryAst> queryAst = {},
        std::shared_ptr<NYql::TExprContext> splitCtx = nullptr,
        NYql::TExprNode::TPtr splitExpr = nullptr)
        : Sender(sender)
        , Query(std::move(query))
        , Uid(uid)
        , CompileSettings(compileSettings)
        , UserToken(userToken)
        , ClientAddress(clientAddress)
        , DbCounters(dbCounters)
        , GUCSettings(gUCSettings)
        , ApplicationName(applicationName)
        , UserRequestContext(userRequestContext)
        , Orbit(std::move(orbit))
        , CompileServiceSpan(std::move(span))
        , Cookie(cookie)
        , TempTablesState(std::move(tempTablesState))
        , IntrestedInResult(std::move(intrestedInResult))
        , QueryAst(std::move(queryAst))
        , SplitCtx(std::move(splitCtx))
        , SplitExpr(std::move(splitExpr))
    {}

    TActorId Sender;
    TKqpQueryId Query;
    TString Uid;
    TKqpCompileSettings CompileSettings;
    TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    TString ClientAddress;
    TKqpDbCountersPtr DbCounters;
    TGUCSettings::TPtr GUCSettings;
    TMaybe<TString> ApplicationName;
    TActorId CompileActor;

    TIntrusivePtr<TUserRequestContext> UserRequestContext;
    NLWTrace::TOrbit Orbit;
    NWilson::TSpan CompileServiceSpan;
    ui64 Cookie;
    TKqpTempTablesState::TConstPtr TempTablesState;
    std::shared_ptr<std::atomic<bool>> IntrestedInResult;
    TMaybe<TQueryAst> QueryAst;

    std::shared_ptr<NYql::TExprContext> SplitCtx;
    NYql::TExprNode::TPtr SplitExpr;

    bool FindInCache = true;

    TInstant CompileQueueEnqueuedAt = TInstant::Now();

    bool IsIntrestedInResult() const {
        return IntrestedInResult->load();
    }
};

class TKqpRequestsQueue {
    using TRequestsList = TList<TKqpCompileRequest>;
    using TRequestsIterator = TRequestsList::iterator;

    struct TRequestsIteratorHash {
        inline size_t operator()(const TRequestsIterator& iterator) const {
            return THash<TKqpCompileRequest*>()(&*iterator);
        }
    };

    using TRequestsIteratorSet = THashSet<TRequestsIterator, TRequestsIteratorHash>;

public:
    TKqpRequestsQueue(TIntrusivePtr<TKqpCounters> counters, size_t maxSize)
        : Counters(counters)
        , MaxSize(maxSize) {}

    std::optional<TKqpCompileRequest> Enqueue(TKqpCompileRequest&& request) {
        if (Size() >= MaxSize) {
            return request;
        }

        Queue.push_back(std::move(request));
        auto it = std::prev(Queue.end());
        QueryIndex[it->Query].insert(it);
        return std::nullopt;
    }

    TMaybe<TKqpCompileRequest> Dequeue(const TInstant& now) {
        auto it = Queue.begin();

        while (it != Queue.end()) {
            auto& request = *it;
            auto curIt = it++;

            if (!request.IsIntrestedInResult()) {
                auto result = std::move(request);
                LOG_DEBUG(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE,
                    "Drop compilation request because session is not longer wait for response");
                QueryIndex[result.Query].erase(curIt);
                Queue.erase(curIt);
                continue;
            }

            if (!ActiveRequests.contains(request.Query)) {
                auto result = std::move(request);

                Counters->ReportCompileQueueWaitTime(now - result.CompileQueueEnqueuedAt);
                QueryIndex[result.Query].erase(curIt);
                Queue.erase(curIt);

                return result;
            }
        }

        return {};
    }

    TVector<TKqpCompileRequest> ExtractByQuery(const TKqpQueryId& query) {
        auto queryIt = QueryIndex.find(query);
        if (queryIt == QueryIndex.end()) {
            return {};
        }

        TVector<TKqpCompileRequest> result;
        for (auto& requestIt : queryIt->second) {
            Y_ENSURE(requestIt != Queue.end());
            auto request = std::move(*requestIt);

            Queue.erase(requestIt);

            result.push_back(std::move(request));
        }

        QueryIndex.erase(queryIt);
        return result;
    }

    size_t Size() const {
        return Queue.size();
    }

    TKqpCompileRequest FinishActiveRequest(const TKqpQueryId& query) {
        auto it = ActiveRequests.find(query);
        Y_ENSURE(it != ActiveRequests.end());

        auto request = std::move(it->second);
        ActiveRequests.erase(it);

        return request;
    }

    size_t ActiveRequestsCount() const {
        return ActiveRequests.size();
    }

    void AddActiveRequest(TKqpCompileRequest&& request) {
        auto result = ActiveRequests.emplace(request.Query, std::move(request));
        Y_ENSURE(result.second);
    }

private:
    TIntrusivePtr<TKqpCounters> Counters;
    size_t MaxSize = 0;
    TRequestsList Queue;
    THashMap<TKqpQueryId, TRequestsIteratorSet> QueryIndex;
    THashMap<TKqpQueryId, TKqpCompileRequest> ActiveRequests;
};

class TKqpCompileService : public TActorBootstrapped<TKqpCompileService> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_COMPILE_SERVICE;
    }

    TKqpCompileService(
        TKqpQueryCachePtr queryCache,
        const TTableServiceConfig& tableServiceConfig, const TQueryServiceConfig& queryServiceConfig,
        const TKqpSettings::TConstPtr& kqpSettings,
        TIntrusivePtr<TModuleResolverState> moduleResolverState, TIntrusivePtr<TKqpCounters> counters,
        std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory,
        std::optional<TKqpFederatedQuerySetup> federatedQuerySetup
        )
        : QueryCache(std::move(queryCache))
        , TableServiceConfig(tableServiceConfig)
        , QueryServiceConfig(queryServiceConfig)
        , KqpSettings(kqpSettings)
        , ModuleResolverState(moduleResolverState)
        , Counters(counters)
        , RequestsQueue(Counters, TableServiceConfig.GetCompileRequestQueueSize())
        , QueryReplayFactory(std::move(queryReplayFactory))
        , FederatedQuerySetup(federatedQuerySetup)
    {}

    void Bootstrap(const TActorContext& ctx) {
        Y_UNUSED(ctx);

        QueryReplayBackend.Reset(CreateQueryReplayBackend(TableServiceConfig, Counters, QueryReplayFactory));
        // Subscribe for TableService config changes
        ui32 tableServiceConfigKind = (ui32) NKikimrConsole::TConfigItem::TableServiceConfigItem;
        Send(NConsole::MakeConfigsDispatcherID(SelfId().NodeId()),
             new NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest({tableServiceConfigKind}),
             IEventHandle::FlagTrackDelivery);

        Become(&TKqpCompileService::MainState);
        if (TableServiceConfig.GetCompileQueryCacheTTLSec()) {
            StartCheckQueriesTtlTimer();
        }
    }

private:
    STFUNC(MainState) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvKqp::TEvCompileRequest, Handle);
            HFunc(TEvKqp::TEvCompileResponse, Handle);
            HFunc(TEvKqp::TEvCompileInvalidateRequest, Handle);
            HFunc(TEvKqp::TEvRecompileRequest, Handle);
            HFunc(TEvKqp::TEvParseResponse, Handle);
            HFunc(TEvKqp::TEvSplitResponse, Handle);

            hFunc(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse, HandleConfig);
            hFunc(NConsole::TEvConsole::TEvConfigNotificationRequest, HandleConfig);
            hFunc(TEvents::TEvUndelivered, HandleUndelivery);

            CFunc(TEvents::TSystem::Wakeup, HandleTtlTimer);
            cFunc(TEvents::TEvPoison::EventType, PassAway);
        default:
            Y_ABORT("TKqpCompileService: unexpected event 0x%08" PRIx32, ev->GetTypeRewrite());
        }
    }

private:
    void HandleConfig(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse::TPtr&) {
        LOG_INFO(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE, "Subscribed for config changes");
    }

    void HandleConfig(NConsole::TEvConsole::TEvConfigNotificationRequest::TPtr& ev) {
        auto &event = ev->Get()->Record;

        bool allowMultiBroadcasts = TableServiceConfig.GetAllowMultiBroadcasts();
        bool enableKqpDataQueryStreamIdxLookupJoin = TableServiceConfig.GetEnableKqpDataQueryStreamIdxLookupJoin();
        bool enableKqpScanQueryStreamIdxLookupJoin = TableServiceConfig.GetEnableKqpScanQueryStreamIdxLookupJoin();

        bool enableKqpScanQuerySourceRead = TableServiceConfig.GetEnableKqpScanQuerySourceRead();

        bool defaultSyntaxVersion = TableServiceConfig.GetSqlVersion();

        ui64 rangesLimit = TableServiceConfig.GetExtractPredicateRangesLimit();
        ui64 idxLookupPointsLimit = TableServiceConfig.GetIdxLookupJoinPointsLimit();

        bool allowOlapDataQuery = TableServiceConfig.GetAllowOlapDataQuery();
        bool enableOlapSink = TableServiceConfig.GetEnableOlapSink();
        bool enableOltpSink = TableServiceConfig.GetEnableOltpSink();
        bool enableHtapTx = TableServiceConfig.GetEnableHtapTx();
        bool enableStreamWrite = TableServiceConfig.GetEnableStreamWrite();
        bool enableCreateTableAs = TableServiceConfig.GetEnableCreateTableAs();
        auto blockChannelsMode = TableServiceConfig.GetBlockChannelsMode();

        bool enableAstCache = TableServiceConfig.GetEnableAstCache();
        bool enableImplicitQueryParameterTypes = TableServiceConfig.GetEnableImplicitQueryParameterTypes();
        bool enablePgConstsToParams = TableServiceConfig.GetEnablePgConstsToParams();
        bool enablePerStatementQueryExecution = TableServiceConfig.GetEnablePerStatementQueryExecution();

        auto mkqlHeavyLimit = TableServiceConfig.GetResourceManager().GetMkqlHeavyProgramMemoryLimit();

        ui64 defaultCostBasedOptimizationLevel = TableServiceConfig.GetDefaultCostBasedOptimizationLevel();
        bool enableConstantFolding = TableServiceConfig.GetEnableConstantFolding();
        bool enableFoldUdfs = TableServiceConfig.GetEnableFoldUdfs();

        bool defaultEnableShuffleElimination = TableServiceConfig.GetDefaultEnableShuffleElimination();

        TString enableSpillingNodes = TableServiceConfig.GetEnableSpillingNodes();
        bool enableSpilling = TableServiceConfig.GetEnableQueryServiceSpilling();

        bool enableSnapshotIsolationRW = TableServiceConfig.GetEnableSnapshotIsolationRW();

        bool enableNewRBO = TableServiceConfig.GetEnableNewRBO();
        bool enableSpillingInHashJoinShuffleConnections = TableServiceConfig.GetEnableSpillingInHashJoinShuffleConnections();
        bool enableOlapScalarApply = TableServiceConfig.GetEnableOlapScalarApply();
        bool enableOlapSubstringPushdown = TableServiceConfig.GetEnableOlapSubstringPushdown();

        bool enableIndexStreamWrite = TableServiceConfig.GetEnableIndexStreamWrite();

        bool enableOlapPushdownProjections = TableServiceConfig.GetEnableOlapPushdownProjections();

        TableServiceConfig.Swap(event.MutableConfig()->MutableTableServiceConfig());
        LOG_INFO(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE, "Updated config");

        auto responseEv = MakeHolder<NConsole::TEvConsole::TEvConfigNotificationResponse>(event);
        Send(ev->Sender, responseEv.Release(), IEventHandle::FlagTrackDelivery, ev->Cookie);

        if (TableServiceConfig.GetSqlVersion() != defaultSyntaxVersion ||
            TableServiceConfig.GetEnableKqpScanQueryStreamIdxLookupJoin() != enableKqpScanQueryStreamIdxLookupJoin ||
            TableServiceConfig.GetEnableKqpDataQueryStreamIdxLookupJoin() != enableKqpDataQueryStreamIdxLookupJoin ||
            TableServiceConfig.GetEnableKqpScanQuerySourceRead() != enableKqpScanQuerySourceRead ||
            TableServiceConfig.GetAllowOlapDataQuery() != allowOlapDataQuery ||
            TableServiceConfig.GetEnableStreamWrite() != enableStreamWrite ||
            TableServiceConfig.GetEnableOlapSink() != enableOlapSink ||
            TableServiceConfig.GetEnableOltpSink() != enableOltpSink ||
            TableServiceConfig.GetEnableHtapTx() != enableHtapTx ||
            TableServiceConfig.GetEnableCreateTableAs() != enableCreateTableAs ||
            TableServiceConfig.GetBlockChannelsMode() != blockChannelsMode ||
            TableServiceConfig.GetExtractPredicateRangesLimit() != rangesLimit ||
            TableServiceConfig.GetResourceManager().GetMkqlHeavyProgramMemoryLimit() != mkqlHeavyLimit ||
            TableServiceConfig.GetIdxLookupJoinPointsLimit() != idxLookupPointsLimit ||
            TableServiceConfig.GetEnableSpillingNodes() != enableSpillingNodes ||
            TableServiceConfig.GetDefaultCostBasedOptimizationLevel() != defaultCostBasedOptimizationLevel ||
            TableServiceConfig.GetEnableConstantFolding() != enableConstantFolding ||
            TableServiceConfig.GetEnableFoldUdfs() != enableFoldUdfs ||
            TableServiceConfig.GetEnableAstCache() != enableAstCache ||
            TableServiceConfig.GetEnableImplicitQueryParameterTypes() != enableImplicitQueryParameterTypes ||
            TableServiceConfig.GetEnablePgConstsToParams() != enablePgConstsToParams ||
            TableServiceConfig.GetEnablePerStatementQueryExecution() != enablePerStatementQueryExecution ||
            TableServiceConfig.GetEnableSnapshotIsolationRW() != enableSnapshotIsolationRW ||
            TableServiceConfig.GetAllowMultiBroadcasts() != allowMultiBroadcasts ||
            TableServiceConfig.GetDefaultEnableShuffleElimination() != defaultEnableShuffleElimination ||
            TableServiceConfig.GetEnableQueryServiceSpilling() != enableSpilling ||
            TableServiceConfig.GetEnableNewRBO() != enableNewRBO ||
            TableServiceConfig.GetEnableSpillingInHashJoinShuffleConnections() != enableSpillingInHashJoinShuffleConnections ||
            TableServiceConfig.GetEnableOlapScalarApply() != enableOlapScalarApply ||
            TableServiceConfig.GetEnableOlapSubstringPushdown() != enableOlapSubstringPushdown ||
            TableServiceConfig.GetEnableIndexStreamWrite() != enableIndexStreamWrite ||
            TableServiceConfig.GetEnableOlapPushdownProjections() != enableOlapPushdownProjections)
        {

            QueryCache->Clear();

            LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE,
                "Query cache was invalidated due to config change");
        }
    }

    void HandleUndelivery(TEvents::TEvUndelivered::TPtr& ev) {
        switch (ev->Get()->SourceType) {
            case NConsole::TEvConfigsDispatcher::EvSetConfigSubscriptionRequest:
                LOG_CRIT(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE,
                    "Failed to deliver subscription request to config dispatcher");
                break;
            case NConsole::TEvConsole::EvConfigNotificationResponse:
                LOG_ERROR(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE,
                    "Failed to deliver config notification response");
                break;
            default:
                LOG_ERROR(*TlsActivationContext, NKikimrServices::KQP_COMPILE_SERVICE,
                    "Undelivered event with unexpected source type: %d", ev->Get()->SourceType);
                break;
        }
    }

    void Handle(TEvKqp::TEvCompileRequest::TPtr& ev, const TActorContext& ctx) {
        const auto& query = ev->Get()->Query;
        LWTRACK(KqpCompileServiceHandleRequest,
            ev->Get()->Orbit,
            query ? query->UserSid : "");

        try {
            PerformRequest(ev, ctx);
        }
        catch (const std::exception& e) {
            LogException("TEvCompileRequest", ev->Sender, e, ctx);
            ReplyInternalError(ev->Sender, "", e.what(), ctx, ev->Cookie, std::move(ev->Get()->Orbit), {});
        }
    }

    void PerformRequest(TEvKqp::TEvCompileRequest::TPtr& ev, const TActorContext& ctx) {
        auto& request = *ev->Get();

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Perform request, TraceId.SpanIdPtr: " << ev->TraceId.GetSpanIdPtr());

        NWilson::TSpan compileServiceSpan(TWilsonKqp::CompileService, std::move(ev->TraceId), "CompileService");

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Received compile request"
            << ", sender: " << ev->Sender
            << ", queryUid: " << (request.Uid ? *request.Uid : "<empty>")
            << ", queryText: \"" << (request.Query ? EscapeC(request.Query->Text) : "<empty>") << "\""
            << ", keepInCache: " << request.KeepInCache
            << ", split: " << request.Split
            << *request.UserRequestContext);

        auto userSid = request.UserToken->GetUserSID();
        auto dbCounters = request.DbCounters;

        auto compileResult = QueryCache->Find(
            request.Uid,
            request.Query,
            request.TempTablesState,
            request.KeepInCache,
            request.UserToken->GetUserSID(),
            Counters,
            dbCounters,
            ev->Sender,
            ctx);

        if (request.Uid) {
            if (compileResult) {
                Y_ENSURE(compileResult->Query);
                if (compileResult->Query->UserSid == userSid) {
                    LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Served query from cache by uid"
                        << ", sender: " << ev->Sender
                        << ", queryUid: " << *request.Uid);

                    ReplyFromCache(ev->Sender, compileResult, ctx, ev->Cookie, std::move(ev->Get()->Orbit), std::move(compileServiceSpan));
                    return;
                } else {
                    LOG_NOTICE_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Non-matching user sid for query"
                        << ", sender: " << ev->Sender
                        << ", queryUid: " << *request.Uid
                        << ", expected sid: " <<  compileResult->Query->UserSid
                        << ", actual sid: " << userSid);
                }
            } else {
                Counters->ReportQueryCacheHit(dbCounters, false);
                LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Query not found"
                    << ", sender: " << ev->Sender
                    << ", queryUid: " << *request.Uid);

                NYql::TIssue issue(NYql::TPosition(), TStringBuilder() << "Query not found: " << *request.Uid);
                ReplyError(ev->Sender, *request.Uid, Ydb::StatusIds::NOT_FOUND, {issue}, ctx, ev->Cookie, std::move(ev->Get()->Orbit), std::move(compileServiceSpan));
                return;
            }
        } else if (compileResult) {
            Y_ENSURE(request.Query);

            LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Served query from cache from query text"
                << ", sender: " << ev->Sender
                << ", queryUid: " << compileResult->Uid);

            ReplyFromCache(ev->Sender, compileResult, ctx, ev->Cookie, std::move(ev->Get()->Orbit), std::move(compileServiceSpan));
            return;
        }

        Counters->ReportCompileRequestCompile(dbCounters);

        CollectDiagnostics = request.CollectDiagnostics;

        LWTRACK(KqpCompileServiceEnqueued,
            ev->Get()->Orbit,
            ev->Get()->Query ? ev->Get()->Query->UserSid : "");

        TKqpCompileSettings compileSettings(
            request.KeepInCache,
            request.IsQueryActionPrepare,
            request.PerStatementResult,
            request.Deadline,
            ev->Get()->Split
                ? ECompileActorAction::SPLIT
                : (TableServiceConfig.GetEnableAstCache() && !request.QueryAst)
                    ? ECompileActorAction::PARSE
                    : ECompileActorAction::COMPILE);
        TKqpCompileRequest compileRequest(ev->Sender, CreateGuidAsString(), std::move(*request.Query),
            compileSettings, request.UserToken, request.ClientAddress, dbCounters, request.GUCSettings, request.ApplicationName, ev->Cookie, std::move(ev->Get()->IntrestedInResult),
            ev->Get()->UserRequestContext, std::move(ev->Get()->Orbit), std::move(compileServiceSpan),
            std::move(ev->Get()->TempTablesState), Nothing(), request.SplitCtx, request.SplitExpr);

        if (TableServiceConfig.GetEnableAstCache() && request.QueryAst) {
            return CompileByAst(*request.QueryAst, compileRequest, ctx);
        }

        auto overflow = RequestsQueue.Enqueue(std::move(compileRequest));
        if (overflow.has_value()) {
            Counters->ReportCompileRequestRejected(dbCounters);

            LOG_WARN_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Requests queue size limit exceeded"
                << ", sender: " << ev->Sender
                << ", queueSize: " << RequestsQueue.Size());

            NYql::TIssue issue(NYql::TPosition(), TStringBuilder() <<
                "Exceeded maximum number of requests in compile service queue.");
            ReplyError(ev->Sender, "", Ydb::StatusIds::OVERLOADED, {issue},
                ctx, overflow->Cookie, std::move(overflow->Orbit), std::move(overflow->CompileServiceSpan));
            return;
        }

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Added request to queue"
            << ", sender: " << ev->Sender
            << ", queueSize: " << RequestsQueue.Size());

        ProcessQueue(ctx);
    }

    void Handle(TEvKqp::TEvRecompileRequest::TPtr& ev, const TActorContext& ctx) {
        try {
            PerformRequest(ev, ctx);
        }
        catch (const std::exception& e) {
            LogException("TEvRecompileRequest", ev->Sender, e, ctx);
            ReplyInternalError(ev->Sender, "", e.what(), ctx, ev->Cookie, std::move(ev->Get()->Orbit), {});
        }
    }

    void PerformRequest(TEvKqp::TEvRecompileRequest::TPtr& ev, const TActorContext& ctx) {
        auto& request = *ev->Get();

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Received recompile request"
            << ", sender: " << ev->Sender);

        auto dbCounters = request.DbCounters;
        Counters->ReportRecompileRequestGet(dbCounters);

        TKqpCompileResult::TConstPtr compileResult = QueryCache->FindByUid(request.Uid, false);
        if (HasTempTablesNameClashes(compileResult, request.TempTablesState)) {
            compileResult = nullptr;
        }

        if (compileResult || request.Query) {
            Counters->ReportCompileRequestCompile(dbCounters);

            NWilson::TSpan compileServiceSpan(TWilsonKqp::CompileService, ev->Get() ? std::move(ev->TraceId) : NWilson::TTraceId(), "CompileService");

            TKqpCompileSettings compileSettings(
                true,
                request.IsQueryActionPrepare,
                false,
                request.Deadline,
                ev->Get()->Split
                    ? ECompileActorAction::SPLIT
                    : (TableServiceConfig.GetEnableAstCache() && !request.QueryAst)
                        ? ECompileActorAction::PARSE
                        : ECompileActorAction::COMPILE);
            auto query = request.Query ? *request.Query : *compileResult->Query;
            if (compileResult) {
                query.UserSid = compileResult->Query->UserSid;
                if (query != *compileResult->Query) {
                    LOG_WARN_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "queryId in recompile request and queryId in cache are different"
                      << ", queryId in request: " << query.SerializeToString()
                      << ", queryId in cache: " << compileResult->Query->SerializeToString()
                    );
                }
            }
            TKqpCompileRequest compileRequest(ev->Sender, request.Uid, compileResult ? *compileResult->Query : *request.Query,
                compileSettings, request.UserToken, request.ClientAddress, dbCounters, request.GUCSettings, request.ApplicationName,
                ev->Cookie, std::move(ev->Get()->IntrestedInResult),
                ev->Get()->UserRequestContext,
                ev->Get() ? std::move(ev->Get()->Orbit) : NLWTrace::TOrbit(),
                std::move(compileServiceSpan), std::move(ev->Get()->TempTablesState));
                compileRequest.FindInCache = false;

            if (TableServiceConfig.GetEnableAstCache() && request.QueryAst) {
                return CompileByAst(*request.QueryAst, compileRequest, ctx);
            }

            auto overflow = RequestsQueue.Enqueue(std::move(compileRequest));
            if (overflow.has_value()) {
                Counters->ReportCompileRequestRejected(dbCounters);

                LOG_WARN_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Requests queue size limit exceeded"
                    << ", sender: " << ev->Sender
                    << ", queueSize: " << RequestsQueue.Size());

                NYql::TIssue issue(NYql::TPosition(), TStringBuilder() <<
                    "Exceeded maximum number of requests in compile service queue.");
                ReplyError(ev->Sender, "", Ydb::StatusIds::OVERLOADED, {issue}, ctx,
                    overflow->Cookie, std::move(overflow->Orbit), std::move(overflow->CompileServiceSpan));
                return;
            }
        } else {
            LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Query not found"
                << ", sender: " << ev->Sender
                << ", queryUid: " << request.Uid);

            NYql::TIssue issue(NYql::TPosition(), TStringBuilder() << "Query not found: " << request.Uid);

            NWilson::TSpan compileServiceSpan(TWilsonKqp::CompileService, ev->Get() ? std::move(ev->TraceId) : NWilson::TTraceId(), "CompileService");

            ReplyError(ev->Sender, request.Uid, Ydb::StatusIds::NOT_FOUND, {issue}, ctx,
                ev->Cookie, std::move(ev->Get()->Orbit), std::move(compileServiceSpan));
            return;
        }

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Added request to queue"
            << ", sender: " << ev->Sender
            << ", queueSize: " << RequestsQueue.Size());

        ProcessQueue(ctx);
    }

    void Handle(TEvKqp::TEvCompileResponse::TPtr& ev, const TActorContext& ctx) {
        auto compileActorId = ev->Sender;
        auto& compileResult = ev->Get()->CompileResult;
        auto& compileStats = ev->Get()->Stats;

        Y_ABORT_UNLESS(compileResult->Query);

        auto compileRequest = RequestsQueue.FinishActiveRequest(*compileResult->Query);
        Y_ABORT_UNLESS(compileRequest.CompileActor == compileActorId);
        Y_ABORT_UNLESS(compileRequest.Uid == compileResult->Uid);

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Received response"
            << ", sender: " << compileRequest.Sender
            << ", status: " << compileResult->Status
            << ", compileActor: " << ev->Sender);

        if (compileResult->NeedToSplit) {
            Reply(compileRequest.Sender, compileResult, compileStats, ctx,
                compileRequest.Cookie, std::move(compileRequest.Orbit), std::move(compileRequest.CompileServiceSpan));
            ProcessQueue(ctx);
            return;
        }

        bool keepInCache = compileRequest.CompileSettings.KeepInCache && compileResult->AllowCache;
        bool isPerStatementExecution = TableServiceConfig.GetEnableAstCache() && compileRequest.QueryAst;

        bool hasTempTablesNameClashes = HasTempTablesNameClashes(compileResult, compileRequest.TempTablesState, true);

        try {
            if (compileResult->Status == Ydb::StatusIds::SUCCESS) {
                if (!hasTempTablesNameClashes) {
                    UpdateQueryCache(ctx, compileResult, keepInCache, compileRequest.CompileSettings.IsQueryActionPrepare, isPerStatementExecution);
                }

                if (ev->Get()->ReplayMessage && !QueryReplayBackend->IsNull()) {
                    QueryReplayBackend->Collect(*ev->Get()->ReplayMessage);
                    QueryCache->AttachReplayMessage(compileRequest.Uid, *ev->Get()->ReplayMessage);
                }

                auto requests = RequestsQueue.ExtractByQuery(*compileResult->Query);
                for (auto& request : requests) {
                    LWTRACK(KqpCompileServiceGetCompilation, request.Orbit, request.Query.UserSid, compileActorId.ToString());
                    Reply(request.Sender, compileResult, compileStats, ctx,
                        request.Cookie, std::move(request.Orbit), std::move(request.CompileServiceSpan));
                }
            } else {
                if (!hasTempTablesNameClashes) {
                    if (QueryCache->FindByUid(compileResult->Uid, false)) {
                        QueryCache->EraseByUid(compileResult->Uid);
                    }
                }
            }

            LWTRACK(KqpCompileServiceGetCompilation, compileRequest.Orbit, compileRequest.Query.UserSid, compileActorId.ToString());
            Reply(compileRequest.Sender, compileResult, compileStats, ctx,
                compileRequest.Cookie, std::move(compileRequest.Orbit), std::move(compileRequest.CompileServiceSpan));
        }
        catch (const std::exception& e) {
            LogException("TEvCompileResponse", ev->Sender, e, ctx);
            ReplyInternalError(compileRequest.Sender, compileResult->Uid, e.what(), ctx,
                compileRequest.Cookie, std::move(compileRequest.Orbit), std::move(compileRequest.CompileServiceSpan));
        }

        ProcessQueue(ctx);
    }

    void Handle(TEvKqp::TEvCompileInvalidateRequest::TPtr& ev, const TActorContext& ctx) {
        try {
            PerformRequest(ev, ctx);
        }
        catch (const std::exception& e) {
            LogException("TEvCompileInvalidateRequest", ev->Sender, e, ctx);
        }
    }

    void PerformRequest(TEvKqp::TEvCompileInvalidateRequest::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ctx);
        auto& request = *ev->Get();

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Received invalidate request"
            << ", sender: " << ev->Sender
            << ", queryUid: " << request.Uid);

        auto dbCounters = request.DbCounters;
        Counters->ReportCompileRequestInvalidate(dbCounters);

        QueryCache->EraseByUid(request.Uid);
    }

    void HandleTtlTimer(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Received check queries TTL timeout");

        auto evicted = QueryCache->EraseExpiredQueries();
        if (evicted != 0) {
            Counters->CompileQueryCacheEvicted->Add(evicted);
        }

        StartCheckQueriesTtlTimer();
    }

    void UpdateQueryCache(const TActorContext& ctx, TKqpCompileResult::TConstPtr compileResult, bool keepInCache, bool isQueryActionPrepare, bool isPerStatementExecution) {
        if (QueryCache->FindByUid(compileResult->Uid, false)) {
            QueryCache->Replace(compileResult);
        } else if (keepInCache) {
            if (compileResult->Query) {
                LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Insert query into compile cache, queryId: " << compileResult->Query->SerializeToString());
                if (QueryCache->FindByQuery(*compileResult->Query, keepInCache)) {
                    LOG_ERROR_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Trying to insert query into compile cache when it is already there");
                }
            }
            if (QueryCache->Insert(compileResult, TableServiceConfig.GetEnableAstCache(), isPerStatementExecution)) {
                Counters->CompileQueryCacheEvicted->Inc();
            }
            if (compileResult->Query && isQueryActionPrepare) {
                if (InsertPreparingQuery(ctx, compileResult, true, isPerStatementExecution)) {
                    Counters->CompileQueryCacheEvicted->Inc();
                };
            }
        }
    }

    void CompileByAst(const TQueryAst& queryAst, TKqpCompileRequest& compileRequest, const TActorContext& ctx) {
        YQL_ENSURE(queryAst.Ast);
        YQL_ENSURE(queryAst.Ast->IsOk());
        YQL_ENSURE(queryAst.Ast->Root);
        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Try to find query by ast, queryId: " << compileRequest.Query.SerializeToString()
            << ", ast: " << queryAst.Ast->Root->ToString());
        auto compileResult = QueryCache->FindByAst(compileRequest.Query, *queryAst.Ast, compileRequest.CompileSettings.KeepInCache);

        if (!compileRequest.FindInCache || HasTempTablesNameClashes(compileResult, compileRequest.TempTablesState)) {
            compileResult = nullptr;
        }

        if (compileResult) {
            Counters->ReportQueryCacheHit(compileRequest.DbCounters, true);

            LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Served query from cache from ast"
                << ", sender: " << compileRequest.Sender
                << ", queryUid: " << compileResult->Uid);

            compileResult->GetAst()->PgAutoParamValues = std::move(queryAst.Ast->PgAutoParamValues);

            ReplyFromCache(compileRequest.Sender, compileResult, ctx, compileRequest.Cookie, std::move(compileRequest.Orbit), std::move(compileRequest.CompileServiceSpan));
            return;
        }

        Counters->ReportQueryCacheHit(compileRequest.DbCounters, false);

        LWTRACK(KqpCompileServiceEnqueued,
            compileRequest.Orbit,
            compileRequest.Query.UserSid);

        compileRequest.QueryAst = std::move(queryAst);

        auto sender = compileRequest.Sender;
        auto overflow = RequestsQueue.Enqueue(std::move(compileRequest));
        if (overflow.has_value()) {
            Counters->ReportCompileRequestRejected(overflow->DbCounters);

            LOG_WARN_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Requests queue size limit exceeded"
                << ", sender: " << overflow->Sender
                << ", queueSize: " << RequestsQueue.Size());

            NYql::TIssue issue(NYql::TPosition(), TStringBuilder() <<
                "Exceeded maximum number of requests in compile service queue.");
            ReplyError(overflow->Sender, "", Ydb::StatusIds::OVERLOADED, {issue}, ctx, overflow->Cookie, std::move(overflow->Orbit), std::move(overflow->CompileServiceSpan));
            return;
        }

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Added request to queue"
            << ", sender: " << sender
            << ", queueSize: " << RequestsQueue.Size());

        ProcessQueue(ctx);
    }

    void Handle(TEvKqp::TEvParseResponse::TPtr& ev, const TActorContext& ctx) {
        auto& astStatements = ev->Get()->AstStatements;
        YQL_ENSURE(astStatements.size());
        auto& query = ev->Get()->Query;
        auto compileRequest = RequestsQueue.FinishActiveRequest(query);
        if (astStatements.size() > 1) {
            ReplyQueryStatements(compileRequest.Sender, astStatements, query, ctx, compileRequest.Cookie, std::move(compileRequest.Orbit), std::move(compileRequest.CompileServiceSpan));
            return;
        }

        compileRequest.CompileSettings.Action = ECompileActorAction::COMPILE;
        CompileByAst(astStatements.front(), compileRequest, ctx);
    }

    void Handle(TEvKqp::TEvSplitResponse::TPtr& ev, const TActorContext& ctx) {
        auto& query = ev->Get()->Query;
        auto compileRequest = RequestsQueue.FinishActiveRequest(query);
        ctx.Send(compileRequest.Sender, ev->Release(), 0, compileRequest.Cookie);
    }

private:
    bool InsertPreparingQuery(const TActorContext& ctx, const TKqpCompileResult::TConstPtr& compileResult, bool keepInCache, bool isPerStatementExecution) {
        YQL_ENSURE(compileResult->Query);
        auto query = *compileResult->Query;

        YQL_ENSURE(compileResult->PreparedQuery);
        YQL_ENSURE(!query.QueryParameterTypes);
        if (compileResult->PreparedQuery->GetParameters().empty()) {
            return false;
        }
        auto queryParameterTypes = std::make_shared<std::map<TString, Ydb::Type>>();
        for (const auto& param : compileResult->PreparedQuery->GetParameters()) {
            Ydb::Type paramType;
            ConvertMiniKQLTypeToYdbType(param.GetType(), paramType);
            queryParameterTypes->insert({param.GetName(), paramType});
        }
        query.QueryParameterTypes = queryParameterTypes;
        if (QueryCache->FindByQuery(query, keepInCache)) {
            return false;
        }
        if (compileResult->GetAst() && QueryCache->FindByAst(query, *compileResult->GetAst(), keepInCache)) {
            return false;
        }
        auto newCompileResult = TKqpCompileResult::Make(CreateGuidAsString(), compileResult->Status, compileResult->Issues, compileResult->MaxReadType, std::move(query), compileResult->QueryAst,
            false, {}, compileResult->ReplayMessageUserView);
        newCompileResult->AllowCache = compileResult->AllowCache;
        newCompileResult->PreparedQuery = compileResult->PreparedQuery;
        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Insert preparing query with params, queryId: " << compileResult->Query->SerializeToString());
        return QueryCache->Insert(newCompileResult, TableServiceConfig.GetEnableAstCache(), isPerStatementExecution);
    }

    void ProcessQueue(const TActorContext& ctx) {
        auto maxActiveRequests = TableServiceConfig.GetCompileMaxActiveRequests();
        TInstant processStartedAt = TInstant::Now();
        while (RequestsQueue.ActiveRequestsCount() < maxActiveRequests) {
            auto request = RequestsQueue.Dequeue(processStartedAt);
            if (!request) {
                break;
            }

            if (request->CompileSettings.Deadline && request->CompileSettings.Deadline < TAppData::TimeProvider->Now()) {
                LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Compilation timed out"
                    << ", sender: " << request->Sender
                    << ", deadline: " << request->CompileSettings.Deadline);

                Counters->ReportCompileRequestTimeout(request->DbCounters);

                NYql::TIssue issue(NYql::TPosition(), "Compilation timed out.");
                ReplyError(request->Sender, "", Ydb::StatusIds::TIMEOUT, {issue}, ctx,
                    request->Cookie, std::move(request->Orbit), std::move(request->CompileServiceSpan));
            } else {
                StartCompilation(std::move(*request), ctx);
            }
        }

        *Counters->CompileQueueSize = RequestsQueue.Size();
    }

    void StartCompilation(TKqpCompileRequest&& request, const TActorContext& ctx) {
        auto compileActor = CreateKqpCompileActor(ctx.SelfID, KqpSettings, TableServiceConfig, QueryServiceConfig, ModuleResolverState, Counters,
            request.Uid, request.Query, request.UserToken, request.ClientAddress, FederatedQuerySetup, request.DbCounters, request.GUCSettings, request.ApplicationName, request.UserRequestContext,
            request.CompileServiceSpan.GetTraceId(), request.TempTablesState, request.CompileSettings.Action, std::move(request.QueryAst), CollectDiagnostics,
            request.CompileSettings.PerStatementResult, request.SplitCtx, request.SplitExpr);
        auto compileActorId = ctx.Register(compileActor, TMailboxType::HTSwap,
            AppData(ctx)->UserPoolId);

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Created compile actor"
            << ", sender: " << request.Sender
            << ", compileActor: " << compileActorId);
        request.CompileActor = compileActorId;

        RequestsQueue.AddActiveRequest(std::move(request));
    }

    void StartCheckQueriesTtlTimer() {
        Schedule(TDuration::Seconds(TableServiceConfig.GetCompileQueryCacheTTLSec()), new TEvents::TEvWakeup());
    }

    void Reply(const TActorId& sender, const TKqpCompileResult::TConstPtr& compileResult,
        const TKqpStatsCompile& compileStats, const TActorContext& ctx, ui64 cookie,
        NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        const auto& query = compileResult->Query;
        LWTRACK(KqpCompileServiceReply,
            orbit,
            query ? query->UserSid : "",
            compileResult->Issues.ToString());

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Send response"
            << ", sender: " << sender
            << ", queryUid: " << compileResult->Uid
            << ", status:" << compileResult->Status);

        auto responseEv = MakeHolder<TEvKqp::TEvCompileResponse>(compileResult, std::move(orbit));
        responseEv->Stats = compileStats;

        if (span) {
            span.End();
        }

        ctx.Send(sender, responseEv.Release(), 0, cookie);
    }

    void ReplyFromCache(const TActorId& sender, const TKqpCompileResult::TConstPtr& compileResult,
        const TActorContext& ctx, ui64 cookie, NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        TKqpStatsCompile stats;
        stats.FromCache = true;

        if (auto replayMessage = QueryCache->ReplayMessageByUid(compileResult->Uid, TDuration::Seconds(TableServiceConfig.GetQueryReplayCacheUploadTTLSec()))) {
            QueryReplayBackend->Collect(replayMessage);
        }

        LWTRACK(KqpCompileServiceReplyFromCache, orbit);
        Reply(sender, compileResult, stats, ctx, cookie, std::move(orbit), std::move(span));
    }

    void ReplyError(const TActorId& sender, const TString& uid, Ydb::StatusIds::StatusCode status,
        const TIssues& issues, const TActorContext& ctx, ui64 cookie, NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        LWTRACK(KqpCompileServiceReplyError, orbit);
        Reply(sender, TKqpCompileResult::Make(uid, status, issues, ETableReadType::Other), TKqpStatsCompile{}, ctx, cookie, std::move(orbit), std::move(span));
    }

    void ReplyInternalError(const TActorId& sender, const TString& uid, const TString& message,
        const TActorContext& ctx, ui64 cookie, NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        NYql::TIssue issue(NYql::TPosition(), TStringBuilder() << "Internal error during query compilation.");
        issue.AddSubIssue(MakeIntrusive<TIssue>(NYql::TPosition(), message));

        LWTRACK(KqpCompileServiceReplyInternalError, orbit);
        ReplyError(sender, uid, Ydb::StatusIds::INTERNAL_ERROR, {issue}, ctx, cookie, std::move(orbit), std::move(span));
    }

    static void LogException(const TString& scope, const TActorId& sender, const std::exception& e,
        const TActorContext& ctx)
    {
        LOG_CRIT_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Exception"
            << ", scope: " << scope
            << ", sender: " << sender
            << ", message: " << e.what());
    }

    void Reply(const TActorId& sender, const TVector<TQueryAst>& astStatements, const TKqpQueryId query,
        const TActorContext& ctx, ui64 cookie, NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        LWTRACK(KqpCompileServiceReply,
            orbit,
            query.UserSid,
            {});

        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Send ast statements response");

        auto responseEv = MakeHolder<TEvKqp::TEvParseResponse>(std::move(query), astStatements, std::move(orbit));

        if (span) {
            span.End();
        }

        ctx.Send(sender, responseEv.Release(), 0, cookie);
    }

    void ReplyQueryStatements(const TActorId& sender, const TVector<TQueryAst>& astStatements,
        const TKqpQueryId query, const TActorContext& ctx, ui64 cookie, NLWTrace::TOrbit orbit, NWilson::TSpan span)
    {
        LWTRACK(KqpCompileServiceReplyStatements, orbit);
        Reply(sender, astStatements, query, ctx, cookie, std::move(orbit), std::move(span));
    }

private:
    TKqpQueryCachePtr QueryCache;

    TTableServiceConfig TableServiceConfig;
    TQueryServiceConfig QueryServiceConfig;
    TKqpSettings::TConstPtr KqpSettings;
    TIntrusivePtr<TModuleResolverState> ModuleResolverState;
    TIntrusivePtr<TKqpCounters> Counters;
    THolder<IQueryReplayBackend> QueryReplayBackend;

    TKqpRequestsQueue RequestsQueue;
    std::shared_ptr<IQueryReplayBackendFactory> QueryReplayFactory;
    std::optional<TKqpFederatedQuerySetup> FederatedQuerySetup;

    bool CollectDiagnostics = false;
};

// QueryCache

bool TKqpQueryCache::Insert(
    const TKqpCompileResult::TConstPtr& compileResult,
    bool isEnableAstCache,
    bool isPerStatementExecution)
{
    TGuard<TAdaptiveLock> guard(Lock);

    if (!isPerStatementExecution) {
        InsertQuery(compileResult);
    }
    if (isEnableAstCache && compileResult->GetAst()) {
        InsertAst(compileResult);
    }

    auto it = Index.emplace(compileResult->Uid, TCacheEntry{compileResult, TAppData::TimeProvider->Now() + Ttl});
    Y_ABORT_UNLESS(it.second);

    TItem* item = &const_cast<TItem&>(*it.first);
    auto removedItem = List.Insert(item);

    IncBytes(item->Value.CompileResult->PreparedQuery->ByteSize());

    if (removedItem) {
        DecBytes(removedItem->Value.CompileResult->PreparedQuery->ByteSize());

        auto queryId = *removedItem->Value.CompileResult->Query;
        QueryIndex.erase(queryId);
        if (removedItem->Value.CompileResult->GetAst()) {
            AstIndex.erase(GetQueryIdWithAst(queryId, *removedItem->Value.CompileResult->GetAst()));
        }
        auto indexIt = Index.find(*removedItem);
        if (indexIt != Index.end()) {
            Index.erase(indexIt);
        }
    }

    Y_ABORT_UNLESS(List.GetSize() == Index.size());

    return removedItem != nullptr;
}

void TKqpQueryCache::AttachReplayMessage(const TString uid, TString replayMessage) {
    TGuard<TAdaptiveLock> guard(Lock);

    auto it = Index.find(TItem(uid));
    if (it != Index.end()) {
        TItem* item = &const_cast<TItem&>(*it);
        DecBytes(item->Value.ReplayMessage.size());
        item->Value.ReplayMessage = replayMessage;
        item->Value.LastReplayTime = TInstant::Now();
        IncBytes(replayMessage.size());
    }
}

TString TKqpQueryCache::ReplayMessageByUid(const TString uid, TDuration timeout) {
    TGuard<TAdaptiveLock> guard(Lock);

    auto it = Index.find(TItem(uid));
    if (it != Index.end()) {
        TInstant& lastReplayTime = const_cast<TItem&>(*it).Value.LastReplayTime;
        TInstant now = TInstant::Now();
        if (lastReplayTime + timeout < now) {
            lastReplayTime = now;
            return it->Value.ReplayMessage;
        }
    }
    return "";
}

TKqpCompileResult::TConstPtr TKqpQueryCache::FindByUidImpl(const TString& uid, bool promote) {
    auto it = Index.find(TItem(uid));
    if (it != Index.end()) {
        TItem* item = &const_cast<TItem&>(*it);
        if (promote) {
            item->Value.ExpiredAt = TAppData::TimeProvider->Now() + Ttl;
            List.Promote(item);
        }

        return item->Value.CompileResult;
    }

    return nullptr;
}

TKqpCompileResult::TConstPtr TKqpQueryCache::FindByQueryImpl(const TKqpQueryId& query, bool promote) {
    auto uid = QueryIndex.FindPtr(query);
    if (!uid) {
        return nullptr;
    }

    // we're holding read and assume it's recursive
    return FindByUidImpl(*uid, promote);
}

bool TKqpQueryCache::EraseByUidImpl(const TString& uid) {
    auto it = Index.find(TItem(uid));
    if (it == Index.end()) {
        return false;
    }

    TItem* item = &const_cast<TItem&>(*it);
    List.Erase(item);

    DecBytes(item->Value.CompileResult->PreparedQuery->ByteSize());
    DecBytes(item->Value.ReplayMessage.size());

    Y_ABORT_UNLESS(item->Value.CompileResult);
    Y_ABORT_UNLESS(item->Value.CompileResult->Query);
    auto queryId = *item->Value.CompileResult->Query;
    QueryIndex.erase(queryId);
    if (item->Value.CompileResult->GetAst()) {
        AstIndex.erase(GetQueryIdWithAst(queryId, *item->Value.CompileResult->GetAst()));
    }

    Index.erase(it);

    Y_ABORT_UNLESS(List.GetSize() == Index.size());
    return true;
}

void TKqpQueryCache::Replace(const TKqpCompileResult::TConstPtr& compileResult) {
    TGuard<TAdaptiveLock> guard(Lock);

    auto it = Index.find(TItem(compileResult->Uid));
    if (it != Index.end()) {
        TItem& item = const_cast<TItem&>(*it);
        item.Value.CompileResult = compileResult;
    }
}

// find by either uid or query
TKqpCompileResult::TConstPtr TKqpQueryCache::Find(
    const TMaybe<TString>& uid,
    TMaybe<TKqpQueryId>& query,
    TKqpTempTablesState::TConstPtr tempTablesState,
    bool promote,
    const NACLib::TSID& userSid,
    TIntrusivePtr<TKqpCounters> counters,
    TKqpDbCountersPtr& dbCounters,
    const TActorId& sender,
    const TActorContext& ctx)
{
    TGuard<TAdaptiveLock> guard(Lock);

    *counters->CompileQueryCacheSize = SizeImpl();
    *counters->CompileQueryCacheBytes = BytesImpl();

    if (uid) {
        counters->ReportCompileRequestGet(dbCounters);

        auto compileResult = FindByUidImpl(*uid, promote);
        if (HasTempTablesNameClashes(compileResult, tempTablesState)) {
            compileResult = nullptr;
        }

        if (compileResult) {
            Y_ENSURE(compileResult->Query);
            if (compileResult->Query->UserSid == userSid) {
                counters->ReportQueryCacheHit(dbCounters, true);

                LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Served query from cache by uid"
                    << ", sender: " << sender
                    << ", queryUid: " << *uid);

                return compileResult;
            } else {
                LOG_NOTICE_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Non-matching user sid for query"
                    << ", sender: " << sender
                    << ", queryUid: " << *uid
                    << ", expected sid: " <<  compileResult->Query->UserSid
                    << ", actual sid: " << userSid);
            }
        }

        return nullptr;
    }

    Y_ENSURE(query);

    if (query->UserSid.empty()) {
        query->UserSid = userSid;
    } else {
        Y_ENSURE(query->UserSid == userSid);
    }

    LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Try to find query by queryId, queryId: "
        << query->SerializeToString());
    auto compileResult = FindByQueryImpl(*query, promote);
    if (HasTempTablesNameClashes(compileResult, tempTablesState)) {
        return nullptr;
    }

    if (compileResult) {
        counters->ReportQueryCacheHit(dbCounters, true);
        LOG_DEBUG_S(ctx, NKikimrServices::KQP_COMPILE_SERVICE, "Served query from cache from query text"
            << ", sender: " << sender
            << ", queryUid: " << compileResult->Uid);

        return compileResult;
    }

    // note, we don't report cache miss, because it's up to caller to decide what to do:
    // in particular, session actor will go to the compile service, which will actually report the miss.

    return nullptr;
}

TKqpCompileResult::TConstPtr TKqpQueryCache::FindByAst(
    const TKqpQueryId& query,
    const NYql::TAstParseResult& ast,
    bool promote)
{
    TGuard<TAdaptiveLock> guard(Lock);

    auto uid = AstIndex.FindPtr(GetQueryIdWithAst(query, ast));
    if (!uid) {
        return nullptr;
    }

    return FindByUidImpl(*uid, promote);
}

size_t TKqpQueryCache::EraseExpiredQueries() {
    TGuard<TAdaptiveLock> guard(Lock);

    auto prevSize = SizeImpl();

    auto now = TAppData::TimeProvider->Now();
    while (List.GetSize() && List.GetOldest()->Value.ExpiredAt <= now) {
        EraseByUidImpl(List.GetOldest()->Key);
    }

    Y_ABORT_UNLESS(List.GetSize() == Index.size());
    return prevSize - SizeImpl();
}

void TKqpQueryCache::Clear() {
    TGuard<TAdaptiveLock> guard(Lock);

    List = TList(List.GetMaxSize());
    Index.clear();
    QueryIndex.clear();
    AstIndex.clear();
    ByteSize = 0;
}

void TKqpQueryCache::InsertQuery(const TKqpCompileResult::TConstPtr& compileResult) {
    Y_ENSURE(compileResult->Query);
    auto& query = *compileResult->Query;

    YQL_ENSURE(compileResult->PreparedQuery);

    auto queryIt = QueryIndex.emplace(query, compileResult->Uid);
    if (!queryIt.second) {
        EraseByUidImpl(compileResult->Uid);
        QueryIndex.erase(query);
    }
    Y_ENSURE(queryIt.second);
}

TKqpQueryId TKqpQueryCache::GetQueryIdWithAst(const TKqpQueryId& query, const NYql::TAstParseResult& ast) {
    Y_ABORT_UNLESS(ast.Root);
    std::shared_ptr<std::map<TString, Ydb::Type>> astPgParams;
    if (query.QueryParameterTypes || ast.PgAutoParamValues) {
        astPgParams = std::make_shared<std::map<TString, Ydb::Type>>();
        if (query.QueryParameterTypes) {
            for (const auto& [name, param] : *query.QueryParameterTypes) {
                astPgParams->insert({name, param});
            }
        }
        if (ast.PgAutoParamValues) {
            const auto& params = dynamic_cast<TKqpAutoParamBuilder*>(ast.PgAutoParamValues.Get())->Values;
            for (const auto& [name, param] : params) {
                astPgParams->insert({name, param.Gettype()});
            }
        }
    }
    return TKqpQueryId{query.Cluster, query.Database, query.DatabaseId, ast.Root->ToString(), query.Settings, astPgParams, query.GUCSettings};
}

//

bool HasTempTablesNameClashes(
    TKqpCompileResult::TConstPtr compileResult,
    TKqpTempTablesState::TConstPtr tempTablesState,
    bool withSessionId)
{
    if (!compileResult) {
        return false;
    }
    if (!compileResult->PreparedQuery) {
        return false;
    }

    return compileResult->PreparedQuery->HasTempTables(tempTablesState, withSessionId);
}

IActor* CreateKqpCompileService(
    TKqpQueryCachePtr queryCache,
    const TTableServiceConfig& tableServiceConfig, const TQueryServiceConfig& queryServiceConfig,
    const TKqpSettings::TConstPtr& kqpSettings, TIntrusivePtr<TModuleResolverState> moduleResolverState, TIntrusivePtr<TKqpCounters> counters,
    std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory,
    std::optional<TKqpFederatedQuerySetup> federatedQuerySetup)
{
    return new TKqpCompileService(
        std::move(queryCache),
        tableServiceConfig, queryServiceConfig, kqpSettings, moduleResolverState, counters,
                                  std::move(queryReplayFactory), federatedQuerySetup);
}

} // namespace NKqp
} // namespace NKikimr
