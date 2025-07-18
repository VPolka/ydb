#include "action.h"
#include "error.h"
#include "executor.h"
#include "log.h"

#include <ydb/core/ymq/actor/cloud_events/cloud_events.h>

#include <ydb/core/ymq/base/constants.h>
#include <ydb/core/ymq/base/helpers.h>
#include <ydb/core/ymq/base/limits.h>
#include <ydb/core/ymq/base/dlq_helpers.h>
#include <ydb/core/ymq/queues/common/key_hashes.h>
#include <ydb/public/lib/value/value.h>

#include <util/string/cast.h>

using NKikimr::NClient::TValue;

namespace NKikimr::NSQS {

class TUntagQueueActor
    : public TActionActor<TUntagQueueActor>
{
public:
    static constexpr bool NeedQueueAttributes() {
        return true;
    }
    static constexpr bool NeedQueueTags() {
        return true;
    }

    TUntagQueueActor(const NKikimrClient::TSqsRequest& sourceSqsRequest, THolder<IReplyCallback> cb)
        : TActionActor(sourceSqsRequest, EAction::UntagQueue, std::move(cb))
    {
        for (const auto& key : Request().tagkeys()) {
            TagKeys_.push_back(key);
        }

        SourceAddress_ = Request().GetSourceAddress();
        IsCloudEventsEnabled = Cfg().HasCloudEventsConfig() && Cfg().GetCloudEventsConfig().GetEnableCloudEvents();
    }

private:
    bool DoValidate() override {
        if (!GetQueueName()) {
            MakeError(Response_.MutableUntagQueue(), NErrors::MISSING_PARAMETER, "No QueueName parameter.");
            return false;
        }

        return true;
    }

    TError* MutableErrorDesc() override {
        return Response_.MutableUntagQueue()->MutableError();
    }

    void DoAction() override {
        Become(&TThis::StateFunc);

        auto oldTags = TagsToJson(*QueueTags_);

        NJson::TJsonMap tags;
        if (QueueTags_.Defined()) {
            for (const auto& k : TagKeys_) {
                QueueTags_->GetMapSafe().erase(k);
            }
            for (const auto& [k, v] : QueueTags_->GetMap()) {
                tags[k] = v;
            }
        }

        TExecutorBuilder builder(SelfId(), RequestId_);
        if (IsCloudEventsEnabled) {
            const auto& cloudEvCfg = Cfg().GetCloudEventsConfig();
            TString database = (cloudEvCfg.HasTenantMode() && cloudEvCfg.GetTenantMode()? Cfg().GetRoot() : "");

            auto evId = NCloudEvents::TEventIdGenerator::Generate();
            auto createdAt = TInstant::Now().MilliSeconds();

            builder
                .User(UserName_)
                .Queue(GetQueueName())
                .TablesFormat(TablesFormat())
                .QueueLeader(QueueLeader_)
                .QueryId(TAG_QUEUE_ID)
                .Counters(QueueCounters_)
                .RetryOnTimeout()
                .Params()
                    .Utf8("NAME", GetQueueName())
                    .Utf8("USER_NAME", UserName_)
                    .Utf8("TAGS", TagsToJson(tags))
                    .Utf8("OLD_TAGS", oldTags)
                    .Uint64("CLOUD_EVENT_ID", evId)
                    .Uint64("CLOUD_EVENT_NOW", createdAt)
                    .Utf8("CLOUD_EVENT_TYPE", "UpdateMessageQueue")
                    .Utf8("CLOUD_EVENT_CLOUD_ID", UserName_)
                    .Utf8("CLOUD_EVENT_FOLDER_ID", FolderId_)
                    .Utf8("CLOUD_EVENT_USER_SID", UserSID_)
                    .Utf8("CLOUD_EVENT_USER_MASKED_TOKEN", MaskedToken_)
                    .Utf8("CLOUD_EVENT_AUTHTYPE", AuthType_)
                    .Utf8("CLOUD_EVENT_PEERNAME", SourceAddress_)
                    .Utf8("CLOUD_EVENT_REQUEST_ID", RequestId_);
        } else {
            builder
                .User(UserName_)
                .Queue(GetQueueName())
                .TablesFormat(TablesFormat())
                .QueueLeader(QueueLeader_)
                .QueryId(TAG_QUEUE_ID)
                .Counters(QueueCounters_)
                .RetryOnTimeout()
                .Params()
                    .Utf8("NAME", GetQueueName())
                    .Utf8("USER_NAME", UserName_)
                    .Utf8("TAGS", TagsToJson(tags))
                    .Utf8("OLD_TAGS", oldTags);
        }

        builder.Start();
    }

    TString DoGetQueueName() const override {
        return Request().GetQueueName();
    }

    STATEFN(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvWakeup, HandleWakeup);
            hFunc(TSqsEvents::TEvExecuted, HandleExecuted);
        }
    }

    TString GetFullCloudEventsTablePath() const {
        if (!Cfg().GetRoot().empty()) {
            return TStringBuilder() << Cfg().GetRoot() << "/" << NCloudEvents::TProcessor::EventTableName;
        } else {
            return TString(NCloudEvents::TProcessor::EventTableName);
        }
    }

    void HandleExecuted(TSqsEvents::TEvExecuted::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui32 status = record.GetStatus();
        auto* result = Response_.MutableUntagQueue();

        if (status == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecComplete) {
            const TValue val(TValue::Create(record.GetExecutionEngineEvaluatedResponse()));
            bool updated = val["updated"];
            if (updated) {
                RLOG_SQS_DEBUG("Sending clear attributes cache event for queue [" << UserName_ << "/" << GetQueueName() << "]");
                Send(QueueLeader_, MakeHolder<TSqsEvents::TEvClearQueueAttributesCache>());
            } else {
                auto message = "Untag queue query failed, conflicting query in parallel";
                RLOG_SQS_ERROR(message << ": " << record);
                MakeError(result, NErrors::INTERNAL_FAILURE, message);
            }
        } else {
            RLOG_SQS_ERROR("Tag queue query failed, answer: " << record);
            MakeError(result, NErrors::INTERNAL_FAILURE);
        }
        SendReplyAndDie();
    }

    const TUntagQueueRequest& Request() const {
        return SourceSqsRequest_.GetUntagQueue();
    }

private:
    TVector<TString> TagKeys_;
    bool IsCloudEventsEnabled;
    TString CustomQueueName_ = "";
    TString SourceAddress_ = "";
};

IActor* CreateUntagQueueActor(const NKikimrClient::TSqsRequest& sourceSqsRequest, THolder<IReplyCallback> cb) {
    return new TUntagQueueActor(sourceSqsRequest, std::move(cb));
}

} // namespace NKikimr::NSQS
