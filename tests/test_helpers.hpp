/**
 * @file test_helpers.hpp
 * @brief Shared helpers and fixture for the statistical function tests
 *
 * Provides a Google Test fixture that centralises creating the in-memory database,
 * loading the extension and inserting the test data, for use by every test file.
 *
 * The helpers keep the names and semantics of the SQLite version so that test
 * bodies port unchanged: db_ is a duckdb::Connection*, and every helper takes it
 * as its first argument.
 */
#pragma once

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.hpp"

// =====================================================================
// Path to the extension (injected by CMake)
// =====================================================================
#ifndef EXT_FUNCS_PATH
#define EXT_FUNCS_PATH "./ext_funcs.duckdb_extension"
#endif

// =====================================================================
// Helper functions (free functions)
// =====================================================================

/**
 * @brief Run a query, failing the current test (non-fatally) if it errors
 * @return The materialized result (check HasError() before reading it)
 */
inline std::unique_ptr<duckdb::MaterializedQueryResult> run_query(duckdb::Connection* db, const char* sql) {
    auto result = db->Query(sql);
    EXPECT_FALSE(result->HasError()) << "query failed: " << result->GetError() << "\n  SQL: " << sql;
    return result;
}

/**
 * @brief Return the first cell of a result, or a NULL Value if there is none
 */
inline duckdb::Value first_cell(duckdb::MaterializedQueryResult& result) {
    if (result.HasError() || result.ColumnCount() == 0 || result.RowCount() == 0) {
        return duckdb::Value();
    }
    return result.GetValue(0, 0);
}

/**
 * @brief Run DDL/DML. Fails the test immediately via FAIL() on error
 */
inline void exec_sql(duckdb::Connection* db, const char* sql) {
    auto result = db->Query(sql);
    if (result->HasError()) {
        FAIL() << "exec_sql failed: " << result->GetError() << "\n  SQL: " << sql;
    }
}

/**
 * @brief Fetch a SELECT result as a double
 * @return The value, or NaN if it is NULL
 */
inline double query_double(duckdb::Connection* db, const char* sql) {
    auto result = run_query(db, sql);
    const duckdb::Value v = first_cell(*result);
    if (v.IsNull()) return std::numeric_limits<double>::quiet_NaN();
    return v.GetValue<double>();
}

/**
 * @brief Fetch a SELECT result as text (JSON and the like)
 * @return The string, or empty if it is NULL
 */
inline std::string query_text(duckdb::Connection* db, const char* sql) {
    auto result = run_query(db, sql);
    const duckdb::Value v = first_cell(*result);
    if (v.IsNull()) return "";
    return v.ToString();
}

/**
 * @brief Fetch a SELECT result as an int64_t
 * @return The value, or 0 if it is NULL
 */
inline int64_t query_int(duckdb::Connection* db, const char* sql) {
    auto result = run_query(db, sql);
    const duckdb::Value v = first_cell(*result);
    if (v.IsNull()) return 0;
    return v.GetValue<int64_t>();
}

/**
 * @brief Determine whether a SELECT result is NULL
 */
inline bool query_is_null(duckdb::Connection* db, const char* sql) {
    auto result = run_query(db, sql);
    return first_cell(*result).IsNull();
}

/**
 * @brief Assert that a SELECT fails and return the error message
 *
 * statcpp throws std::invalid_argument when an argument is out of range. The
 * extension converts it to a DuckDB error at the callback boundary; that this
 * helper returns a message rather than the process aborting is evidence the
 * guard is working.
 *
 * @param db Target database connection
 * @param sql SQL to execute
 * @return The error message if it failed, or an empty string on success
 */
inline std::string query_error(duckdb::Connection* db, const char* sql) {
    auto result = db->Query(sql);
    return result->HasError() ? result->GetError() : "";
}

/**
 * @brief Fetch a multi-row SELECT result as a vector<double> (for window functions)
 *        NULL rows are stored as NaN
 */
inline std::vector<double> query_doubles(duckdb::Connection* db, const char* sql) {
    auto result = run_query(db, sql);
    std::vector<double> results;
    if (result->HasError()) return results;
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
        const duckdb::Value v = result->GetValue(0, row);
        results.push_back(v.IsNull() ? std::numeric_limits<double>::quiet_NaN() : v.GetValue<double>());
    }
    return results;
}

/**
 * @brief Extract a double from a JSON string via json_extract()
 *
 * DuckDB's json_extract() returns a JSON value, so it is cast to DOUBLE
 * (SQLite's json_extract() returns a REAL directly).
 */
inline double json_double(duckdb::Connection* db, const std::string& json,
                          const char* path) {
    std::string sql = "SELECT json_extract('" + json + "', '" +
                      std::string(path) + "')::DOUBLE";
    return query_double(db, sql.c_str());
}

/**
 * @brief Extract text from a JSON string via json_extract_string()
 *
 * Like SQLite's json_extract(), returns a JSON string without its quotes and
 * any other value as its JSON text.
 */
inline std::string json_text(duckdb::Connection* db, const std::string& json,
                             const char* path) {
    std::string sql = "SELECT json_extract_string('" + json + "', '" +
                      std::string(path) + "')";
    return query_text(db, sql.c_str());
}

// =====================================================================
// Test fixtures
// =====================================================================

/**
 * @brief Base fixture for the statistical function tests
 *
 * SetUp() creates the in-memory database, loads the ext_funcs extension and inserts
 * the test data; TearDown() closes the database.
 */
class StatFuncTest : public ::testing::Test {
protected:
    std::unique_ptr<duckdb::DuckDB> database_;  ///< Owns the in-memory database
    std::unique_ptr<duckdb::Connection> con_;   ///< Owns the connection
    duckdb::Connection* db_ = nullptr;          ///< Non-owning alias passed to the helpers

    void SetUp() override {
        // Create the in-memory database (the extension is unsigned)
        duckdb::DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
        database_ = std::make_unique<duckdb::DuckDB>(nullptr, &config);
        con_ = std::make_unique<duckdb::Connection>(*database_);
        db_ = con_.get();

        // Load the ext_funcs extension
        auto load = db_->Query(std::string("LOAD '") + EXT_FUNCS_PATH + "'");
        if (load->HasError()) {
            FAIL() << "Load extension failed: " << load->GetError() << "\n  Path: " << EXT_FUNCS_PATH;
        }

        // Insert the test data
        CreateTestData();
    }

    void TearDown() override {
        db_ = nullptr;
        con_.reset();
        database_.reset();
    }

private:
    /// @brief Create every test data table
    ///
    /// SQLite's REAL is a double; DuckDB's REAL is a 4-byte float, so DOUBLE is
    /// used throughout. SQLite's INTEGER PRIMARY KEY auto-numbering is replaced
    /// by explicit ids.
    void CreateTestData() {
        // data: basic statistics (val: 1-10, mean=5.5, median=5.5)
        exec_sql(db_, "CREATE TABLE data (id INTEGER PRIMARY KEY, val DOUBLE)");
        for (int i = 1; i <= 10; ++i) {
            std::string sql = "INSERT INTO data (id, val) VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i) + ")";
            exec_sql(db_, sql.c_str());
        }

        // data2: mode (mode=3)
        exec_sql(db_, "CREATE TABLE data2 (val DOUBLE)");
        exec_sql(db_,
            "INSERT INTO data2 VALUES (1),(2),(2),(3),(3),(3),(4),(4),(5)");

        // pos_data: geometric mean (2,4,8 -> geometric_mean=4.0)
        exec_sql(db_, "CREATE TABLE pos_data (val DOUBLE)");
        exec_sql(db_, "INSERT INTO pos_data VALUES (2),(4),(8)");

        // nulldata: NULL handling (1,NULL,3,NULL,5)
        exec_sql(db_, "CREATE TABLE nulldata (val DOUBLE)");
        exec_sql(db_,
            "INSERT INTO nulldata VALUES (1),(NULL),(3),(NULL),(5)");

        // empty_data: empty table (edge cases)
        exec_sql(db_, "CREATE TABLE empty_data (val DOUBLE)");

        // grouped: GROUP BY tests
        exec_sql(db_, "CREATE TABLE grouped (grp TEXT, val DOUBLE)");
        exec_sql(db_,
            "INSERT INTO grouped VALUES "
            "('A',1),('A',2),('A',3),('B',10),('B',20),('B',30)");

        // xy_data: correlation and regression (10 rows)
        exec_sql(db_, "CREATE TABLE xy_data (x DOUBLE, y DOUBLE)");
        exec_sql(db_,
            "INSERT INTO xy_data VALUES "
            "(1,2),(2,4),(3,5),(4,4),(5,5),(6,7),(7,8),(8,6),(9,11),(10,8)");

        // wt_data: weighted statistics (5 rows)
        exec_sql(db_, "CREATE TABLE wt_data (val DOUBLE, wt DOUBLE)");
        exec_sql(db_,
            "INSERT INTO wt_data VALUES "
            "(10,1),(20,2),(30,3),(40,2),(50,1)");

        // pred_data: prediction accuracy metrics
        exec_sql(db_, "CREATE TABLE pred_data (actual DOUBLE, predicted DOUBLE)");
        exec_sql(db_,
            "INSERT INTO pred_data VALUES "
            "(3,2.5),(5,5.2),(7,6.8),(9,9.1),(11,10.5)");

        // ts_data: window functions (10 rows, NULL at id=3,8)
        exec_sql(db_, "CREATE TABLE ts_data (id INTEGER PRIMARY KEY, val DOUBLE)");
        exec_sql(db_,
            "INSERT INTO ts_data VALUES "
            "(1,10),(2,20),(3,NULL),(4,40),(5,50),"
            "(6,30),(7,70),(8,NULL),(9,90),(10,100)");

        // cat_data: categorical encoding
        exec_sql(db_,
            "CREATE TABLE cat_data (id INTEGER PRIMARY KEY, val DOUBLE)");
        exec_sql(db_,
            "INSERT INTO cat_data VALUES "
            "(1,100),(2,200),(3,100),(4,300),(5,200),(6,100)");

        // surv_data: survival analysis
        exec_sql(db_, "CREATE TABLE surv_data(time DOUBLE, event INT)");
        exec_sql(db_,
            "INSERT INTO surv_data VALUES "
            "(1,1),(2,0),(3,1),(4,1),(5,0),(6,1),(7,1),(8,0),(9,1),(10,1)");

        // grp_data: two-sample tests
        exec_sql(db_, "CREATE TABLE grp_data(val DOUBLE, grp INT)");
        exec_sql(db_,
            "INSERT INTO grp_data VALUES "
            "(10,0),(12,0),(14,0),(16,0),(18,0),"
            "(20,1),(22,1),(24,1),(26,1),(28,1)");

        // surv2: log-rank test (two groups)
        exec_sql(db_, "CREATE TABLE surv2(time DOUBLE, event INT, grp INT)");
        exec_sql(db_,
            "INSERT INTO surv2 VALUES "
            "(1,1,0),(3,0,0),(5,1,0),(7,1,0),(9,0,0),"
            "(2,1,1),(4,1,1),(6,0,1),(8,1,1),(10,1,1)");
    }
};
