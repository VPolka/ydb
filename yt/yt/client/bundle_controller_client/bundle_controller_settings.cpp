#include "bundle_controller_settings.h"

#include <yt/yt_proto/yt/client/bundle_controller/proto/bundle_controller_service.pb.h>

namespace NYT::NBundleControllerClient {

////////////////////////////////////////////////////////////////////////////////

void TCpuLimits::Register(TRegistrar registrar)
{
    registrar.Parameter("write_thread_pool_size", &TThis::WriteThreadPoolSize)
        .Optional();
    registrar.Parameter("lookup_thread_pool_size", &TThis::LookupThreadPoolSize)
        .Optional();
    registrar.Parameter("query_thread_pool_size", &TThis::QueryThreadPoolSize)
        .Optional();
}

void TMemoryLimits::Register(TRegistrar registrar)
{
    registrar.Parameter("tablet_static", &TThis::TabletStatic)
        .Optional();
    registrar.Parameter("tablet_dynamic", &TThis::TabletDynamic)
        .Optional();
    registrar.Parameter("compressed_block_cache", &TThis::CompressedBlockCache)
        .Optional();
    registrar.Parameter("uncompressed_block_cache", &TThis::UncompressedBlockCache)
        .Optional();
    registrar.Parameter("key_filter_block_cache", &TThis::KeyFilterBlockCache)
        .Optional();
    registrar.Parameter("versioned_chunk_meta", &TThis::VersionedChunkMeta)
        .Optional();
    registrar.Parameter("lookup_row_cache", &TThis::LookupRowCache)
        .Optional();
    registrar.Parameter("reserved", &TThis::Reserved)
        .Optional();
    registrar.Parameter("query", &TThis::Query)
        .Optional();
}

void TInstanceResources::Register(TRegistrar registrar)
{
    registrar.Parameter("vcpu", &TThis::Vcpu)
        .GreaterThanOrEqual(0)
        .Default(18000);
    registrar.Parameter("memory", &TThis::Memory)
        .GreaterThanOrEqual(0)
        .Default(120_GB);
    registrar.Parameter("net", &TThis::NetBits)
        .Optional();
    registrar.Parameter("net_bytes", &TThis::NetBytes)
        .Optional();
    registrar.Parameter("type", &TThis::Type)
        .Default();

    registrar.Postprocessor([] (TThis* config) {
        if (config->NetBits.has_value() && config->NetBytes.has_value() && *config->NetBits != *config->NetBytes * 8) {
            THROW_ERROR_EXCEPTION("Net parameters are not equal")
                << TErrorAttribute("net_bytes", config->NetBytes)
                << TErrorAttribute("net_bits", config->NetBits);
        }

        config->CanonizeNet();
    });
}

void TInstanceResources::Clear()
{
    Vcpu = 0;
    Memory = 0;
    ResetNet();
}

void TInstanceResources::CanonizeNet()
{
    // COMPAT(grachevkirill)
    if (NetBytes.has_value()) {
        NetBits = *NetBytes * 8;
    } else if (NetBits.has_value()) {
        NetBytes = *NetBits / 8;
    }
}

void TInstanceResources::ResetNet()
{
    NetBits.reset();
    NetBytes.reset();
}

bool TInstanceResources::operator==(const TInstanceResources& other) const
{
    return std::tie(Vcpu, Memory, NetBytes) == std::tie(other.Vcpu, other.Memory, other.NetBytes);
}

void TDefaultInstanceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cpu_limits", &TThis::CpuLimits)
        .DefaultNew();
    registrar.Parameter("memory_limits", &TThis::MemoryLimits)
        .DefaultNew();
}

void TInstanceSize::Register(TRegistrar registrar)
{
    registrar.Parameter("resource_guarantee", &TThis::ResourceGuarantee)
        .DefaultNew();
    registrar.Parameter("default_config", &TThis::DefaultConfig)
        .DefaultNew();
    registrar.Parameter("host_tag_filter", &TThis::HostTagFilter)
        .Optional();
}

void TBundleTargetConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cpu_limits", &TThis::CpuLimits)
        .DefaultNew();
    registrar.Parameter("memory_limits", &TThis::MemoryLimits)
        .DefaultNew();
    registrar.Parameter("rpc_proxy_count", &TThis::RpcProxyCount)
        .Optional();
    registrar.Parameter("rpc_proxy_resource_guarantee", &TThis::RpcProxyResourceGuarantee)
        .Default();
    registrar.Parameter("tablet_node_count", &TThis::TabletNodeCount)
        .Optional();
    registrar.Parameter("tablet_node_resource_guarantee", &TThis::TabletNodeResourceGuarantee)
        .Default();
}

void TBundleConfigConstraints::Register(TRegistrar registrar)
{
    registrar.Parameter("rpc_proxy_sizes", &TThis::RpcProxySizes)
        .Default();
    registrar.Parameter("tablet_node_sizes", &TThis::TabletNodeSizes)
        .Default();
}


void TBundleResourceQuota::Register(TRegistrar registrar)
{
    registrar.Parameter("vcpu", &TThis::Vcpu)
        .GreaterThanOrEqual(0)
        .Default(0);

    registrar.Parameter("memory", &TThis::Memory)
        .GreaterThanOrEqual(0)
        .Default(0);

    registrar.Parameter("network", &TThis::NetworkBits)
        .GreaterThanOrEqual(0)
        .Default(0);
    registrar.Parameter("network_bytes", &TThis::NetworkBytes)
        .GreaterThanOrEqual(0)
        .Default(0);

    registrar.Postprocessor([] (TThis* config) {
        if (config->NetworkBits != 0 && config->NetworkBytes != 0 && config->NetworkBits != config->NetworkBytes * 8) {
            THROW_ERROR_EXCEPTION("Network parameters are not equal")
                << TErrorAttribute("network_bytes", config->NetworkBytes)
                << TErrorAttribute("network_bits", config->NetworkBits);
        }

        // COMPAT(grachevkirill)
        if (config->NetworkBytes != 0) {
            config->NetworkBits = config->NetworkBytes * 8;
        } else if (config->NetworkBits != 0) {
            config->NetworkBytes = config->NetworkBits / 8;
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

////////////////////////////////////////////////////////////////////////////////

#define YT_FROMPROTO_OPTIONAL_PTR(messagePtr, messageField, structPtr, structField) (((messagePtr)->has_##messageField()) ? (structPtr)->structField = (messagePtr)->messageField() : (structPtr)->structField)
#define YT_TOPROTO_OPTIONAL_PTR(messagePtr, messageField, structPtr, structField) (((structPtr)->structField.has_value()) ? (messagePtr)->set_##messageField((structPtr)->structField.value()) : void())

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TCpuLimits* protoCpuLimits, const NBundleControllerClient::TCpuLimitsPtr cpuLimits)
{
    YT_TOPROTO_OPTIONAL_PTR(protoCpuLimits, lookup_thread_pool_size, cpuLimits, LookupThreadPoolSize);
    YT_TOPROTO_OPTIONAL_PTR(protoCpuLimits, query_thread_pool_size, cpuLimits, QueryThreadPoolSize);
    YT_TOPROTO_OPTIONAL_PTR(protoCpuLimits, write_thread_pool_size, cpuLimits, WriteThreadPoolSize);
}

void FromProto(NBundleControllerClient::TCpuLimitsPtr cpuLimits, const NBundleController::NProto::TCpuLimits* protoCpuLimits)
{
    YT_FROMPROTO_OPTIONAL_PTR(protoCpuLimits, lookup_thread_pool_size, cpuLimits, LookupThreadPoolSize);
    YT_FROMPROTO_OPTIONAL_PTR(protoCpuLimits, query_thread_pool_size, cpuLimits, QueryThreadPoolSize);
    YT_FROMPROTO_OPTIONAL_PTR(protoCpuLimits, write_thread_pool_size, cpuLimits, WriteThreadPoolSize);
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TMemoryLimits* protoMemoryLimits, const NBundleControllerClient::TMemoryLimitsPtr memoryLimits)
{
    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, compressed_block_cache, memoryLimits, CompressedBlockCache);
    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, key_filter_block_cache, memoryLimits, KeyFilterBlockCache);
    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, lookup_row_cache, memoryLimits, LookupRowCache);

    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, tablet_dynamic, memoryLimits, TabletDynamic);
    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, tablet_static, memoryLimits, TabletStatic);

    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, uncompressed_block_cache, memoryLimits, UncompressedBlockCache);

    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, versioned_chunk_meta, memoryLimits, VersionedChunkMeta);

    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, reserved, memoryLimits, Reserved);

    YT_TOPROTO_OPTIONAL_PTR(protoMemoryLimits, query, memoryLimits, Query);
}

void FromProto(NBundleControllerClient::TMemoryLimitsPtr memoryLimits, const NBundleController::NProto::TMemoryLimits* protoMemoryLimits)
{
    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, compressed_block_cache, memoryLimits, CompressedBlockCache);
    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, key_filter_block_cache, memoryLimits, KeyFilterBlockCache);
    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, lookup_row_cache, memoryLimits, LookupRowCache);

    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, tablet_dynamic, memoryLimits, TabletDynamic);
    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, tablet_static, memoryLimits, TabletStatic);

    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, uncompressed_block_cache, memoryLimits, UncompressedBlockCache);

    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, versioned_chunk_meta, memoryLimits, VersionedChunkMeta);

    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, reserved, memoryLimits, Reserved);

    YT_FROMPROTO_OPTIONAL_PTR(protoMemoryLimits, query, memoryLimits, Query);
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TInstanceResources* protoInstanceResources, const NBundleControllerClient::TInstanceResourcesPtr instanceResources)
{
    if (instanceResources == nullptr) return;
    protoInstanceResources->set_memory(instanceResources->Memory);
    // COMPAT(grachevkirill)
    YT_VERIFY(instanceResources->NetBytes.has_value() || !instanceResources->NetBits.has_value());
    YT_TOPROTO_OPTIONAL_PTR(protoInstanceResources, net_bytes, instanceResources, NetBytes);
    protoInstanceResources->set_type(instanceResources->Type);
    protoInstanceResources->set_vcpu(instanceResources->Vcpu);
}

void FromProto(NBundleControllerClient::TInstanceResourcesPtr instanceResources, const NBundleController::NProto::TInstanceResources* protoInstanceResources)
{
    YT_FROMPROTO_OPTIONAL_PTR(protoInstanceResources, memory, instanceResources, Memory);
    YT_FROMPROTO_OPTIONAL_PTR(protoInstanceResources, net_bytes, instanceResources, NetBytes);
    YT_FROMPROTO_OPTIONAL_PTR(protoInstanceResources, type, instanceResources, Type);
    YT_FROMPROTO_OPTIONAL_PTR(protoInstanceResources, vcpu, instanceResources, Vcpu);
    // COMPAT(grachevkirill): Remove later.
    instanceResources->CanonizeNet();
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TDefaultInstanceConfig* protoDefaultInstanceConfig, const TDefaultInstanceConfigPtr defaultInstanceConfig)
{
    ToProto(protoDefaultInstanceConfig->mutable_cpu_limits(), defaultInstanceConfig->CpuLimits);
    ToProto(protoDefaultInstanceConfig->mutable_memory_limits(), defaultInstanceConfig->MemoryLimits);
}

void FromProto(TDefaultInstanceConfigPtr defaultInstanceConfig, const NBundleController::NProto::TDefaultInstanceConfig* protoDefaultInstanceConfig)
{
    if (protoDefaultInstanceConfig->has_cpu_limits())
        FromProto(defaultInstanceConfig->CpuLimits, &protoDefaultInstanceConfig->cpu_limits());
    if (protoDefaultInstanceConfig->has_memory_limits())
        FromProto(defaultInstanceConfig->MemoryLimits, &protoDefaultInstanceConfig->memory_limits());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TInstanceSize* protoInstanceSize, const TInstanceSizePtr instanceSize)
{
    ToProto(protoInstanceSize->mutable_resource_guarantee(), instanceSize->ResourceGuarantee);
    ToProto(protoInstanceSize->mutable_default_config(), instanceSize->DefaultConfig);
}

void FromProto(TInstanceSizePtr instanceSize, const NBundleController::NProto::TInstanceSize* protoInstanceSize)
{
    if (protoInstanceSize->has_resource_guarantee())
        FromProto(instanceSize->ResourceGuarantee, &protoInstanceSize->resource_guarantee());
    if (protoInstanceSize->has_default_config())
        FromProto(instanceSize->DefaultConfig, &protoInstanceSize->default_config());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TBundleConfig* protoBundleConfig, const NBundleControllerClient::TBundleTargetConfigPtr bundleConfig)
{
    YT_TOPROTO_OPTIONAL_PTR(protoBundleConfig, rpc_proxy_count, bundleConfig, RpcProxyCount);
    YT_TOPROTO_OPTIONAL_PTR(protoBundleConfig, tablet_node_count, bundleConfig, TabletNodeCount);
    ToProto(protoBundleConfig->mutable_cpu_limits(), bundleConfig->CpuLimits);
    ToProto(protoBundleConfig->mutable_memory_limits(), bundleConfig->MemoryLimits);
    ToProto(protoBundleConfig->mutable_rpc_proxy_resource_guarantee(), bundleConfig->RpcProxyResourceGuarantee);
    ToProto(protoBundleConfig->mutable_tablet_node_resource_guarantee(), bundleConfig->TabletNodeResourceGuarantee);
}

void FromProto(NBundleControllerClient::TBundleTargetConfigPtr bundleConfig, const NBundleController::NProto::TBundleConfig* protoBundleConfig)
{
    YT_FROMPROTO_OPTIONAL_PTR(protoBundleConfig, rpc_proxy_count, bundleConfig, RpcProxyCount);
    YT_FROMPROTO_OPTIONAL_PTR(protoBundleConfig, tablet_node_count, bundleConfig, TabletNodeCount);
    if (protoBundleConfig->has_cpu_limits())
        FromProto(bundleConfig->CpuLimits, &protoBundleConfig->cpu_limits());
    if (protoBundleConfig->has_memory_limits())
        FromProto(bundleConfig->MemoryLimits, &protoBundleConfig->memory_limits());
    if (protoBundleConfig->has_rpc_proxy_resource_guarantee())
        FromProto(bundleConfig->RpcProxyResourceGuarantee, &protoBundleConfig->rpc_proxy_resource_guarantee());
    if (protoBundleConfig->has_tablet_node_resource_guarantee())
        FromProto(bundleConfig->TabletNodeResourceGuarantee, &protoBundleConfig->tablet_node_resource_guarantee());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TBundleConfigConstraints* protoBundleConfigConstraints, const TBundleConfigConstraintsPtr bundleConfigConstraints)
{
    for (auto instance : bundleConfigConstraints->RpcProxySizes) {
        ToProto(protoBundleConfigConstraints->add_rpc_proxy_sizes(), instance);
    }
    for (auto instance : bundleConfigConstraints->TabletNodeSizes) {
        ToProto(protoBundleConfigConstraints->add_tablet_node_sizes(), instance);
    }
}

void FromProto(TBundleConfigConstraintsPtr bundleConfigConstraints, const NBundleController::NProto::TBundleConfigConstraints* protoBundleConfigConstraints)
{
    auto rpcProxySizes = protoBundleConfigConstraints->rpc_proxy_sizes();

    for (auto instance : rpcProxySizes) {
        auto newInstance = New<TInstanceSize>();
        FromProto(newInstance, &instance);
        bundleConfigConstraints->RpcProxySizes.push_back(newInstance);
    }

    auto tabletNodeSizes = protoBundleConfigConstraints->tablet_node_sizes();

    for (auto instance : tabletNodeSizes) {
        auto newInstance = New<TInstanceSize>();
        FromProto(newInstance, &instance);
        bundleConfigConstraints->TabletNodeSizes.push_back(newInstance);
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NBundleController::NProto::TResourceQuota* protoResourceQuota, const TBundleResourceQuotaPtr resourceQuota)
{
    protoResourceQuota->set_vcpu(resourceQuota->Vcpu);
    protoResourceQuota->set_memory(resourceQuota->Memory);
}

void FromProto(TBundleResourceQuotaPtr resourceQuota, const NBundleController::NProto::TResourceQuota* protoResourceQuota)
{
    YT_FROMPROTO_OPTIONAL_PTR(protoResourceQuota, memory, resourceQuota, Memory);
    YT_FROMPROTO_OPTIONAL_PTR(protoResourceQuota, vcpu, resourceQuota, Vcpu);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto

} // namespace NYT::NBundleControllerClient
