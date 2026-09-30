#include "storage/unity_catalog_storage.h"

#include "catalog/duckdb_catalog.h"
#include "common/string_utils.h"
#include "connector/unity_catalog_connector.h"
#include "extension/extension.h"
#include "function/clear_cache.h"
#include "storage/attached_duckdb_database.h"

namespace lbug {
namespace unity_catalog_extension {

std::unique_ptr<main::AttachedDatabase> attachUnityCatalog(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    if (dbName == "") {
        dbName = dbPath;
    }
    auto connector = std::make_unique<UnityCatalogConnector>();
    // The DuckDB-side catalog is attached AS dbName, so DuckDBCatalog must use
    // dbName (not dbPath) as its catalog name: information_schema lookups and
    // generated SQL reference the attached alias. The catalog is constructed
    // before connecting so that ATTACH option validation happens before any
    // network I/O.
    auto catalog = std::make_unique<duckdb_extension::DuckDBCatalog>(dbPath, dbName,
        UnityCatalogStorageExtension::DEFAULT_SCHEMA_NAME, clientContext, *connector, attachOption,
        dbName);
    connector->connect(dbPath, dbName, UnityCatalogStorageExtension::DEFAULT_SCHEMA_NAME,
        clientContext);
    catalog->init();
    return std::make_unique<duckdb_extension::AttachedDuckDBDatabase>(dbName,
        UnityCatalogStorageExtension::DB_TYPE, std::move(catalog), std::move(connector));
}

UnityCatalogStorageExtension::UnityCatalogStorageExtension(main::Database& database)
    : StorageExtension{attachUnityCatalog} {
    extension::ExtensionUtils::addStandaloneTableFunc<duckdb_extension::ClearCacheFunction>(
        database);
}

bool UnityCatalogStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace unity_catalog_extension
} // namespace lbug
