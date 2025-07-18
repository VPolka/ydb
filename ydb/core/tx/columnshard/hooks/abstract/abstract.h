#pragma once

#include <ydb/core/tablet_flat/tablet_flat_executor.h>
#include <ydb/core/tx/columnshard/common/limits.h>
#include <ydb/core/tx/columnshard/common/snapshot.h>
#include <ydb/core/tx/columnshard/common/path_id.h>
#include <ydb/core/tx/columnshard/engines/writer/write_controller.h>
#include <ydb/core/tx/columnshard/splitter/settings.h>
#include <ydb/core/tx/tiering/tier/identifier.h>
#include <ydb/core/tx/tiering/tier/object.h>

#include <ydb/library/accessor/accessor.h>
#include <ydb/services/metadata/abstract/fetcher.h>

#include <util/datetime/base.h>
#include <util/generic/refcount.h>
#include <util/generic/singleton.h>

#include <memory>

namespace NKikimr::NColumnShard {
class TTiersManager;
class TColumnShard;
}   // namespace NKikimr::NColumnShard

namespace NKikimr::NOlap {
class TColumnEngineChanges;
class IBlobsGCAction;
class TPortionInfo;
class TDataAccessorsResult;
class IBlobsStorageOperator;
namespace NIndexes {
class TIndexMetaContainer;
}
namespace NDataLocks {
class ILock;
}
}   // namespace NKikimr::NOlap
namespace arrow {
class RecordBatch;
}

namespace NKikimr::NWrappers::NExternalStorage {
class IExternalStorageOperator;
}

namespace NKikimr::NYDBTest {

enum class EOptimizerCompactionWeightControl {
    Disable,
    Default,
    Force
};

class ILocalDBModifier {
public:
    using TPtr = std::shared_ptr<ILocalDBModifier>;

    virtual ~ILocalDBModifier() {
    }

    virtual void Apply(NTabletFlatExecutor::TTransactionContext& txc) const = 0;
};

class ICSController {
public:
    enum class EBackground {
        Compaction,
        TTL,
        Cleanup,
        GC,
        CleanupSchemas
    };
    YDB_ACCESSOR(bool, InterruptionOnLockedTransactions, false);

protected:
    virtual std::optional<TDuration> DoGetStalenessLivetimePing() const {
        return {};
    }
    virtual void DoOnTabletInitCompleted(const ::NKikimr::NColumnShard::TColumnShard& /*shard*/) {
        return;
    }
    virtual void DoOnTabletStopped(const ::NKikimr::NColumnShard::TColumnShard& /*shard*/) {
        return;
    }
    virtual bool DoOnAfterFilterAssembling(const std::shared_ptr<arrow::RecordBatch>& /*batch*/) {
        return true;
    }
    virtual bool DoOnWriteIndexComplete(const NOlap::TColumnEngineChanges& /*changes*/, const ::NKikimr::NColumnShard::TColumnShard& /*shard*/) {
        return true;
    }
    virtual bool DoOnWriteIndexStart(const ui64 /*tabletId*/, NOlap::TColumnEngineChanges& /*change*/) {
        return true;
    }
    virtual void DoOnAfterSharingSessionsManagerStart(const NColumnShard::TColumnShard& /*shard*/) {
    }
    virtual void DoOnAfterGCAction(const NColumnShard::TColumnShard& /*shard*/, const NOlap::IBlobsGCAction& /*action*/) {
    }
    virtual void DoOnDataSharingFinished(const ui64 /*tabletId*/, const TString& /*sessionId*/) {
    }
    virtual void DoOnDataSharingStarted(const ui64 /*tabletId*/, const TString& /*sessionId*/) {
    }
    virtual void DoOnCollectGarbageResult(TEvBlobStorage::TEvCollectGarbageResult::TPtr& /*result*/) {
    }

    virtual TDuration DoGetUsedSnapshotLivetime(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetLimitForPortionsMetadataAsk(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetOverridenGCPeriod(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetCompactionActualizationLag(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetActualizationTasksLag(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetMetadataRequestSoftMemoryLimit(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetReadSequentiallyBufferSize(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetSmallPortionSizeDetector(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetMaxReadStaleness(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetGuaranteeIndexationInterval(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetPeriodicWakeupActivationPeriod(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetStatsReportInterval(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetGuaranteeIndexationStartBytesLimit(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetOptimizerFreshnessCheckDuration(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual TDuration DoGetLagForCompactionBeforeTierings(const TDuration defaultValue) const {
        return defaultValue;
    }
    virtual ui64 DoGetMemoryLimitScanPortion(const ui64 defaultValue) const {
        return defaultValue;
    }
    virtual const NOlap::NSplitter::TSplitSettings& DoGetBlobSplitSettings(const NOlap::NSplitter::TSplitSettings& defaultValue) const {
        return defaultValue;
    }

private:
    inline static const NKikimrConfig::TColumnShardConfig DefaultConfig = {};

public:
    static const NKikimrConfig::TColumnShardConfig& GetConfig() {
        if (HasAppData()) {
            return AppDataVerified().ColumnShardConfig;
        }
        return DefaultConfig;
    }

    virtual void OnCleanupSchemasFinished() {
    }

    const NOlap::NSplitter::TSplitSettings& GetBlobSplitSettings(
        const NOlap::NSplitter::TSplitSettings& defaultValue = Default<NOlap::NSplitter::TSplitSettings>()) {
        return DoGetBlobSplitSettings(defaultValue);
    }
    virtual bool CheckPortionsToMergeOnCompaction(const ui64 memoryAfterAdd, const ui32 currentSubsetsCount);
    virtual void OnRequestTracingChanges(
        const std::set<NOlap::TSnapshot>& /*snapshotsToSave*/, const std::set<NOlap::TSnapshot>& /*snapshotsToRemove*/) {
    }

    virtual NKikimrProto::EReplyStatus OverrideBlobPutResultOnWrite(const NKikimrProto::EReplyStatus originalStatus) const {
        return originalStatus;
    }

    ui64 GetMemoryLimitScanPortion() const {
        return DoGetMemoryLimitScanPortion(GetConfig().GetMemoryLimitScanPortion());
    }
    virtual bool CheckPortionForEvict(const NOlap::TPortionInfo& portion) const;

    TDuration GetStalenessLivetimePing(const TDuration defValue) const {
        const auto val = DoGetStalenessLivetimePing();
        if (!val || defValue < *val) {
            return defValue;
        } else {
            return *val;
        }
    }

    virtual bool IsBackgroundEnabled(const EBackground /*id*/) const {
        return true;
    }

    using TPtr = std::shared_ptr<ICSController>;
    virtual ~ICSController() = default;

    TDuration GetOverridenGCPeriod() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetGCIntervalMs());
        return DoGetOverridenGCPeriod(defaultValue);
    }

    virtual void OnSelectShardingFilter() {
    }

    ui64 GetLimitForPortionsMetadataAsk() const {
        const ui64 defaultValue = GetConfig().GetLimitForPortionsMetadataAsk();
        return DoGetLimitForPortionsMetadataAsk(defaultValue);
    }

    TDuration GetCompactionActualizationLag() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetCompactionActualizationLagMs());
        return DoGetCompactionActualizationLag(defaultValue);
    }

    virtual NColumnShard::TBlobPutResult::TPtr OverrideBlobPutResultOnCompaction(
        const NColumnShard::TBlobPutResult::TPtr original, const NOlap::TWriteActionsCollection& /*actions*/) const {
        return original;
    }

    virtual std::shared_ptr<NWrappers::NExternalStorage::IExternalStorageOperator> GetStorageOperatorOverride(
        const NColumnShard::NTiers::TExternalStorageId& /*storageId*/) const {
        return nullptr;
    }

    TDuration GetActualizationTasksLag() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetActualizationTasksLagMs());
        return DoGetActualizationTasksLag(defaultValue);
    }

    ui64 GetMetadataRequestSoftMemoryLimit() const {
        const ui64 defaultValue = 100 * (1 << 20);
        return DoGetMetadataRequestSoftMemoryLimit(defaultValue);
    }
    virtual bool NeedForceCompactionBacketsConstruction() const {
        return false;
    }
    ui64 GetSmallPortionSizeDetector() const {
        const ui64 defaultValue = GetConfig().GetSmallPortionDetectSizeLimit();
        return DoGetSmallPortionSizeDetector(defaultValue);
    }
    virtual void OnExportFinished() {
    }
    virtual void OnActualizationRefreshScheme() {
    }
    virtual void OnActualizationRefreshTiering() {
    }
    virtual void AddPortionForActualizer(const i32 /*portionsCount*/) {
    }

    void OnDataSharingFinished(const ui64 tabletId, const TString& sessionId) {
        return DoOnDataSharingFinished(tabletId, sessionId);
    }
    void OnDataSharingStarted(const ui64 tabletId, const TString& sessionId) {
        return DoOnDataSharingStarted(tabletId, sessionId);
    }
    virtual void OnStatisticsUsage(const NOlap::NIndexes::TIndexMetaContainer& /*statOperator*/) {
    }
    virtual void OnPortionActualization(const NOlap::TPortionInfo& /*info*/) {
    }
    virtual void OnTieringMetadataActualized() {
    }
    virtual void OnMaxValueUsage() {
    }

    virtual TDuration GetLagForCompactionBeforeTierings() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetLagForCompactionBeforeTieringsMs());
        return DoGetLagForCompactionBeforeTierings(defaultValue);
    }

    void OnTabletInitCompleted(const NColumnShard::TColumnShard& shard) {
        DoOnTabletInitCompleted(shard);
    }

    void OnTabletStopped(const NColumnShard::TColumnShard& shard) {
        DoOnTabletStopped(shard);
    }

    void OnAfterGCAction(const NColumnShard::TColumnShard& shard, const NOlap::IBlobsGCAction& action) {
        DoOnAfterGCAction(shard, action);
    }

    void OnCollectGarbageResult(TEvBlobStorage::TEvCollectGarbageResult::TPtr& result) {
        DoOnCollectGarbageResult(result);
    }

    bool OnAfterFilterAssembling(const std::shared_ptr<arrow::RecordBatch>& batch) {
        return DoOnAfterFilterAssembling(batch);
    }
    bool OnWriteIndexComplete(const NOlap::TColumnEngineChanges& changes, const NColumnShard::TColumnShard& shard) {
        return DoOnWriteIndexComplete(changes, shard);
    }
    void OnAfterSharingSessionsManagerStart(const NColumnShard::TColumnShard& shard) {
        return DoOnAfterSharingSessionsManagerStart(shard);
    }
    bool OnWriteIndexStart(const ui64 tabletId, NOlap::TColumnEngineChanges& change) {
        return DoOnWriteIndexStart(tabletId, change);
    }
    virtual void OnHeaderSelectProcessed(const std::optional<bool> /*result*/) {
    }

    virtual void OnIndexSelectProcessed(const std::optional<bool> /*result*/) {
    }
    TDuration GetMaxReadStaleness() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetMaxReadStaleness_ms());
        return DoGetMaxReadStaleness(defaultValue);
    }
    TDuration GetMaxReadStalenessInMem() const {
        return 0.9 * GetMaxReadStaleness();
    }
    TDuration GetUsedSnapshotLivetime() const {
        const TDuration defaultValue = 0.6 * GetMaxReadStaleness();
        return DoGetUsedSnapshotLivetime(defaultValue);
    }
    virtual EOptimizerCompactionWeightControl GetCompactionControl() const {
        return EOptimizerCompactionWeightControl::Force;
    }
    TDuration GetGuaranteeIndexationInterval() const;
    TDuration GetPeriodicWakeupActivationPeriod() const;
    TDuration GetStatsReportInterval() const;
    ui64 GetGuaranteeIndexationStartBytesLimit() const;
    TDuration GetOptimizerFreshnessCheckDuration() const {
        const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetOptimizerFreshnessCheckDurationMs());
        return DoGetOptimizerFreshnessCheckDuration(defaultValue);
    }

    virtual void OnTieringModified(const std::shared_ptr<NColumnShard::TTiersManager>& /*tiers*/) {
    }

    virtual ILocalDBModifier::TPtr BuildLocalBaseModifier() const {
        return nullptr;
    }

    virtual THashMap<TString, NColumnShard::NTiers::TTierConfig> GetOverrideTierConfigs() const {
        return {};
    }

    virtual void OnSwitchToWork(const ui64 tabletId) {
        Y_UNUSED(tabletId);
    }

    virtual void OnCleanupActors(const ui64 tabletId) {
        Y_UNUSED(tabletId);
    }

    virtual void OnAfterLocalTxCommitted(const NActors::TActorContext& ctx, const NColumnShard::TColumnShard& shard, const TString& txInfo) {
        Y_UNUSED(ctx);
        Y_UNUSED(shard);
        Y_UNUSED(txInfo);
    }

    virtual THashMap<TString, std::shared_ptr<NKikimr::NOlap::NDataLocks::ILock>> GetExternalDataLocks() const {
        return {};
    }

    virtual bool IsForcedGenerateInternalPathId() const {
        return false;
    }

    virtual void OnAddPathId(const ui64 /* tabletId */, const NColumnShard::TUnifiedPathId& /* pathId */) {
    }
    virtual void OnDeletePathId(const ui64 /* tabletId */, const NColumnShard::TUnifiedPathId& /* pathId */) {
    }

};

class IKqpController {
public:
    using TPtr = std::shared_ptr<IKqpController>;

    virtual ~IKqpController() = default;

    virtual void OnInitTabletScan(const ui64 /*tabletId*/) {
    }

    virtual void OnInitTabletResolving(const ui64 /*tabletId*/) {
    }
};

class TTestKqpController: public IKqpController {
private:
    YDB_READONLY_DEF(TAtomicCounter, InitScanCounter);
    YDB_READONLY_DEF(TAtomicCounter, ResolvingCounter);

public:
    virtual void OnInitTabletScan(const ui64 /*tabletId*/) override {
        InitScanCounter.Inc();
    }

    virtual void OnInitTabletResolving(const ui64 /*tabletId*/) override {
        ResolvingCounter.Inc();
    }
};

class TControllers {
private:
    ICSController::TPtr CSController = std::make_shared<ICSController>();
    IKqpController::TPtr KqpController = std::make_shared<IKqpController>();

public:
    template <class TController>
    class TGuard: TMoveOnly {
    private:
        std::shared_ptr<TController> Controller;

    public:
        TGuard(std::shared_ptr<TController> controller)
            : Controller(controller) {
            Y_ABORT_UNLESS(Controller);
        }

        TGuard(TGuard&& other)
            : TGuard(other.Controller) {
            other.Controller = nullptr;
        }
        TGuard& operator=(TGuard&& other) {
            std::swap(Controller, other.Controller);
        }

        TController* operator->() {
            return Controller.get();
        }

        ~TGuard() {
            if (Controller) {
                Singleton<TControllers>()->CSController = std::make_shared<ICSController>();
            }
        }
    };

    template <class T, class... Types>
    static TGuard<T> RegisterCSControllerGuard(Types... args) {
        auto result = std::make_shared<T>(args...);
        Singleton<TControllers>()->CSController = result;
        return result;
    }

    static ICSController::TPtr GetColumnShardController() {
        return Singleton<TControllers>()->CSController;
    }

    template <class T>
    static T* GetControllerAs() {
        auto controller = Singleton<TControllers>()->CSController;
        return dynamic_cast<T*>(controller.get());
    }

    template <class T, class... Types>
    static TGuard<T> RegisterKqpControllerGuard(Types... args) {
        auto result = std::make_shared<T>(args...);
        Singleton<TControllers>()->KqpController = result;
        return result;
    }

    static IKqpController::TPtr GetKqpController() {
        return Singleton<TControllers>()->KqpController;
    }

    template <class T>
    static T* GetKqpControllerAs() {
        auto controller = Singleton<TControllers>()->KqpController;
        return dynamic_cast<T*>(controller.get());
    }
};

}   // namespace NKikimr::NYDBTest
