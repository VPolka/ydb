#include "clusters_from_connections.h"

#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <ydb/library/yql/providers/generic/provider/yql_generic_cluster_config.h>
#include <yql/essentials/utils/url_builder.h>
#include <ydb/library/actors/http/http.h>
#include <ydb/core/fq/libs/common/iceberg_processor.h>

#include <util/generic/hash.h>
#include <util/string/builder.h>
#include <util/system/env.h>

#include <library/cpp/string_utils/quote/quote.h>

namespace NFq {

using namespace NYql;

namespace {

template <typename TClusterConfig>
void FillClusterAuth(TClusterConfig& clusterCfg,
        const FederatedQuery::IamAuth& auth, const TString& authToken,
        const THashMap<TString, TString>& accountIdSignatures) {
    switch (auth.identity_case()) {
    case FederatedQuery::IamAuth::kNone:
        break;
    case FederatedQuery::IamAuth::kCurrentIam:
        clusterCfg.SetToken(authToken);
        break;
    case FederatedQuery::IamAuth::kToken:
        clusterCfg.SetToken(auth.token().token());
        break;
    case FederatedQuery::IamAuth::kServiceAccount:
        clusterCfg.SetServiceAccountId(auth.service_account().id());
        clusterCfg.SetServiceAccountIdSignature(accountIdSignatures.at(auth.service_account().id()));
        break;
    // Do not replace with default. Adding a new auth item should cause a compilation error
    case FederatedQuery::IamAuth::IDENTITY_NOT_SET:
        break;
    }
}

void FillPqClusterConfig(NYql::TPqClusterConfig& clusterConfig,
        const TString& name, bool useBearerForYdb,
        const TString& authToken, const THashMap<TString, TString>& accountIdSignatures,
        const FederatedQuery::DataStreams& ds,
        const TString& readGroup) {
    clusterConfig.SetName(name);
    if (ds.endpoint()) {
        clusterConfig.SetEndpoint(ds.endpoint());
    }
    clusterConfig.SetDatabase(ds.database());
    clusterConfig.SetDatabaseId(ds.database_id());
    clusterConfig.SetUseSsl(ds.secure());
    clusterConfig.SetAddBearerToToken(useBearerForYdb);
    clusterConfig.SetClusterType(TPqClusterConfig::CT_DATA_STREAMS);
    clusterConfig.SetSharedReading(ds.shared_reading());
    clusterConfig.SetReadGroup(readGroup);
    FillClusterAuth(clusterConfig, ds.auth(), authToken, accountIdSignatures);
}

void FillS3ClusterConfig(NYql::TS3ClusterConfig& clusterConfig,
        const TString& name, const TString& authToken,
        const TString& objectStorageEndpoint,
        const THashMap<TString, TString>& accountIdSignatures,
        const FederatedQuery::ObjectStorageConnection& s3) {
    clusterConfig.SetName(name);
    TString objectStorageUrl;


    if (objectStorageEndpoint == "https://s3.mds.yandex.net") {
        TUrlBuilder builder{"https://"};
        objectStorageUrl = builder.AddPathComponent(s3.bucket() + ".s3.mds.yandex.net/").Build();
    } else {
        TUrlBuilder builder{UrlEscapeRet(objectStorageEndpoint, true)};
        objectStorageUrl = builder.AddPathComponent(s3.bucket() + "/").Build();
    }
    clusterConfig.SetUrl(objectStorageUrl);
    FillClusterAuth(clusterConfig, s3.auth(), authToken, accountIdSignatures);
}

std::pair<TString, bool> ParseHttpEndpoint(const TString& endpoint) {
    TStringBuf scheme;
    TStringBuf host;
    TStringBuf uri;
    NHttp::CrackURL(endpoint, scheme, host, uri);

    // by default useSsl is true
    // explicit "http://" scheme should disable ssl usage
    return std::make_pair(ToString(host), scheme != "http");
}

std::pair<TString, TIpPort> ParseGrpcEndpoint(const TString& endpoint) {
    TStringBuf scheme;
    TStringBuf address;
    TStringBuf uri;
    NHttp::CrackURL(endpoint, scheme, address, uri);

    TString hostname;
    TIpPort port;
    NHttp::CrackAddress(TString(address), hostname, port);

    return {hostname, port};
}

void FillSolomonClusterConfig(NYql::TSolomonClusterConfig& clusterConfig,
    const TString& name,
    const TString& authToken,
    const TString& endpoint,
    const THashMap<TString, TString>& accountIdSignatures,
    const FederatedQuery::Monitoring& monitoring) {
    clusterConfig.SetName(name);

    auto [address, useSsl] = ParseHttpEndpoint(endpoint);

    clusterConfig.SetCluster(address);
    clusterConfig.SetClusterType(TSolomonClusterConfig::SCT_MONITORING);
    clusterConfig.MutablePath()->SetProject(monitoring.project());
    clusterConfig.MutablePath()->SetCluster(monitoring.cluster());
    clusterConfig.SetUseSsl(useSsl);

    FillClusterAuth(clusterConfig, monitoring.auth(), authToken, accountIdSignatures);
}

template <typename TConnection>
void FillGenericClusterConfigBase(
    const NConfig::TCommonConfig& common,
    TGenericClusterConfig& clusterCfg,
    const TConnection& connection,
    const TString& connectionName,
    NYql::EGenericDataSourceKind dataSourceKind,
    const TString& authToken,
    const THashMap<TString, TString>& accountIdSignatures
) {
    clusterCfg.SetKind(dataSourceKind);
    clusterCfg.SetName(connectionName);
    clusterCfg.SetDatabaseId(connection.database_id());
    clusterCfg.SetDatabaseName(connection.database_name());
    clusterCfg.mutable_credentials()->mutable_basic()->set_username(connection.login());
    clusterCfg.mutable_credentials()->mutable_basic()->set_password(connection.password());
    FillClusterAuth(clusterCfg, connection.auth(), authToken, accountIdSignatures);
    clusterCfg.SetUseSsl(!common.GetDisableSslForGenericDataSources());

    // In YQv1 we just hardcode the appropriate protocols here.
    // In YQv2 protocol can be configured via `CREATE EXTERNAL DATA SOURCE` params.
    switch (dataSourceKind) {
        case NYql::EGenericDataSourceKind::CLICKHOUSE:
            clusterCfg.SetProtocol(common.GetUseNativeProtocolForClickHouse() ? NYql::EGenericProtocol::NATIVE : NYql::EGenericProtocol::HTTP);
            break;
        case NYql::EGenericDataSourceKind::GREENPLUM:
            clusterCfg.SetProtocol(NYql::EGenericProtocol::NATIVE);
            break;
        case NYql::EGenericDataSourceKind::MYSQL:
            clusterCfg.SetProtocol(NYql::EGenericProtocol::NATIVE);
            break;
        case NYql::EGenericDataSourceKind::POSTGRESQL:
            clusterCfg.SetProtocol(NYql::EGenericProtocol::NATIVE);
            break;
        case NYql::EGenericDataSourceKind::ICEBERG:
            clusterCfg.SetProtocol(NYql::EGenericProtocol::NATIVE);
            break;
        default:
            ythrow yexception() << "Unexpected data source kind: '" 
                                << NYql::EGenericDataSourceKind_Name(dataSourceKind) << "'";
    }

    ValidateGenericClusterConfig(clusterCfg, "NFq::FillGenericClusterFromConfig");
}

template <typename TConnection>
void FillGenericClusterConfig(
    const NConfig::TCommonConfig& common,
    TGenericClusterConfig& clusterCfg,
    const TConnection& connection,
    const TString& connectionName,
    NYql::EGenericDataSourceKind dataSourceKind,
    const TString& authToken,
    const THashMap<TString, TString>& accountIdSignatures
) {
    FillGenericClusterConfigBase(common, clusterCfg, connection, connectionName, dataSourceKind, authToken, accountIdSignatures);
}

template<>
void FillGenericClusterConfig<FederatedQuery::PostgreSQLCluster>(
    const NConfig::TCommonConfig& common,
    TGenericClusterConfig& clusterCfg,
    const FederatedQuery::PostgreSQLCluster& connection,
    const TString& connectionName,
    NYql::EGenericDataSourceKind dataSourceKind,
    const TString& authToken,
    const THashMap<TString, TString>& accountIdSignatures
){
    FillGenericClusterConfigBase(common, clusterCfg, connection, connectionName, dataSourceKind, authToken, accountIdSignatures);
    clusterCfg.mutable_datasourceoptions()->insert({TString("schema"), TString(connection.schema())});
}

} //namespace

NYql::TPqClusterConfig CreatePqClusterConfig(const TString& name,
        bool useBearerForYdb, const TString& authToken,
        const TString& accountSignature, const FederatedQuery::DataStreams& ds,
        const TString& readGroup) {
    NYql::TPqClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    if (ds.auth().has_service_account()) {
        accountIdSignatures[ds.auth().service_account().id()] = accountSignature;
    }
    FillPqClusterConfig(cluster, name, useBearerForYdb, authToken, accountIdSignatures, ds, readGroup);
    return cluster;
}

NYql::TS3ClusterConfig CreateS3ClusterConfig(const TString& name,
        const TString& authToken, const TString& objectStorageEndpoint,
        const TString& accountSignature, const FederatedQuery::ObjectStorageConnection& s3) {
    NYql::TS3ClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    accountIdSignatures[s3.auth().service_account().id()] = accountSignature;
    FillS3ClusterConfig(cluster, name, authToken, objectStorageEndpoint, accountIdSignatures, s3);
    return cluster;
}

NYql::TSolomonClusterConfig CreateSolomonClusterConfig(const TString& name,
        const TString& authToken,
        const TString& monitoringEndpoint,
        const TString& accountSignature,
        const FederatedQuery::Monitoring& monitoring) {
    NYql::TSolomonClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    accountIdSignatures[monitoring.auth().service_account().id()] = accountSignature;
    FillSolomonClusterConfig(cluster, name, authToken, monitoringEndpoint, accountIdSignatures, monitoring);
    return cluster;
}

void AddClustersFromConnections(
    const NConfig::TCommonConfig& common,
    const THashMap<TString, FederatedQuery::Connection>& connections,
    const TString& monitoringEndpoint,
    const TString& authToken,
    const THashMap<TString, TString>& accountIdSignatures,
    TGatewaysConfig& gatewaysConfig,
    THashMap<TString, TString>& clusters)
{
    for (const auto&[_, conn] : connections) {
        auto connectionName = conn.content().name();
        switch (conn.content().setting().connection_case()) {
        case FederatedQuery::ConnectionSetting::kYdbDatabase: {
            const auto& db = conn.content().setting().ydb_database();
            auto* clusterCfg = gatewaysConfig.MutableGeneric()->AddClusterMapping();
            clusterCfg->SetKind(NYql::EGenericDataSourceKind::YDB);
            clusterCfg->SetProtocol(NYql::EGenericProtocol::NATIVE);
            clusterCfg->SetName(connectionName);
            if (const auto& databaseId = db.database_id()) {
                clusterCfg->SetDatabaseId(databaseId);
                clusterCfg->SetUseSsl(!common.GetDisableSslForGenericDataSources());
            } else {
                const auto& [host, port] = ParseGrpcEndpoint(db.endpoint());

                auto& endpoint = *clusterCfg->MutableEndpoint();
                endpoint.set_host(host);
                endpoint.set_port(port);
                clusterCfg->SetUseSsl(db.secure());
                clusterCfg->SetDatabaseName(db.database());
            }
            FillClusterAuth(*clusterCfg, db.auth(), authToken, accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kClickhouseCluster: {
            FillGenericClusterConfig(
                common,
                *gatewaysConfig.MutableGeneric()->AddClusterMapping(),
                conn.content().setting().clickhouse_cluster(),
                connectionName,
                NYql::EGenericDataSourceKind::CLICKHOUSE,
                authToken,
                accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kObjectStorage: {
            const auto& s3 = conn.content().setting().object_storage();
            auto* clusterCfg = gatewaysConfig.MutableS3()->AddClusterMapping();
            FillS3ClusterConfig(*clusterCfg, connectionName, authToken, common.GetObjectStorageEndpoint(), accountIdSignatures, s3);
            clusters.emplace(connectionName, S3ProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kDataStreams: {
            const auto& ds = conn.content().setting().data_streams();
            auto* clusterCfg = gatewaysConfig.MutablePq()->AddClusterMapping();
            FillPqClusterConfig(*clusterCfg, connectionName, common.GetUseBearerForYdb(), authToken, accountIdSignatures, ds, conn.meta().id());
            clusters.emplace(connectionName, PqProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kMonitoring: {
            const auto& monitoring = conn.content().setting().monitoring();
            auto* clusterCfg = gatewaysConfig.MutableSolomon()->AddClusterMapping();
            FillSolomonClusterConfig(*clusterCfg, connectionName, authToken, monitoringEndpoint, accountIdSignatures, monitoring);
            clusters.emplace(connectionName, SolomonProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kPostgresqlCluster: {
            FillGenericClusterConfig(
                common,
                *gatewaysConfig.MutableGeneric()->AddClusterMapping(),
                conn.content().setting().postgresql_cluster(),
                connectionName,
                NYql::EGenericDataSourceKind::POSTGRESQL,
                authToken,
                accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kGreenplumCluster: {
            FillGenericClusterConfig(
                common,
                *gatewaysConfig.MutableGeneric()->AddClusterMapping(),
                conn.content().setting().greenplum_cluster(),
                connectionName,
                NYql::EGenericDataSourceKind::GREENPLUM,
                authToken,
                accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kMysqlCluster: {
            FillGenericClusterConfig(
                common,
                *gatewaysConfig.MutableGeneric()->AddClusterMapping(),
                conn.content().setting().mysql_cluster(),
                connectionName,
                NYql::EGenericDataSourceKind::MYSQL,
                authToken,
                accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }
        case FederatedQuery::ConnectionSetting::kLogging: {
            const auto& connection = conn.content().setting().logging();
            auto* clusterCfg = gatewaysConfig.MutableGeneric()->AddClusterMapping();
            clusterCfg->SetKind(NYql::EGenericDataSourceKind::LOGGING);
            clusterCfg->SetName(connectionName);
            clusterCfg->mutable_datasourceoptions()->insert({"folder_id", connection.folder_id()});
            FillClusterAuth(*clusterCfg, connection.auth(), authToken, accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }

        case FederatedQuery::ConnectionSetting::kIceberg: {
            const auto& db = conn.content().setting().iceberg();
            auto& clusterConfig = *gatewaysConfig.MutableGeneric()->AddClusterMapping();

            clusterConfig.SetName(connectionName);
            NFq::FillIcebergGenericClusterConfig(common, db, clusterConfig);
            FillClusterAuth(clusterConfig, db.warehouse_auth(), authToken, accountIdSignatures);
            clusters.emplace(connectionName, GenericProviderName);
            break;
        }

        // Do not replace with default. Adding a new connection should cause a compilation error
        case FederatedQuery::ConnectionSetting::CONNECTION_NOT_SET:
            break;
        }
    }
}
} //NFq
