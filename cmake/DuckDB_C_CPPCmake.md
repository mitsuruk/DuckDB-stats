# DuckDB_C_CPP.cmake Reference

## Overview

`DuckDB_C_CPP.cmake` is a CMake configuration file that automatically downloads, builds, and links the DuckDB library from source.
It uses CMake's `file(DOWNLOAD)` and `execute_process` to manage the dependency, with caching in the `download/` directory to avoid redundant downloads and rebuilds.

DuckDB is an in-process SQL OLAP database management system. It provides both a C API (`duckdb.h`) and a C++ API (`duckdb.hpp`) for embedding a high-performance analytical database directly into applications. DuckDB supports full SQL, ACID transactions, columnar storage, and vectorized query execution.

## File Information

| Item | Details |
|------|---------|
| Source Directory | `${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB/DuckDB_C_CPP` |
| Install Directory | `${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB/DuckDB_C_CPP-install` |
| Download URL | <https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.6.tar.gz> |
| Version | 1.5.6 |
| License | MIT License |

---

## Include Guard

```cmake
include_guard(GLOBAL)
```

This file uses `include_guard(GLOBAL)` to ensure it is only executed once, even if included multiple times.

**Why it's needed:**

- Prevents duplicate `execute_process` invocations during configure
- Prevents duplicate linking in `target_link_libraries`

---

## Directory Structure

```text
DuckDB-stats/
├── cmake/
│   ├── DuckDB_C_CPP.cmake          # This configuration file
│   ├── DuckDB_C_CPPCmake.md        # This document (English)
│   └── DuckDB_C_CPPCmake-jp.md     # This document (Japanese)
├── download/
│   └── DuckDB/
│       ├── duckdb-1.5.6.tar.gz      # Source tarball (cached)
│       ├── DuckDB_C_CPP/            # DuckDB source (cached, downloaded from GitHub)
│       │   └── _build/              # CMake build directory (inside source)
│       └── DuckDB_C_CPP-install/    # DuckDB built artifacts (lib/, include/)
│           ├── include/
│           │   ├── duckdb.hpp
│           │   ├── duckdb.h
│           │   └── duckdb/
│           │       └── ...
│           └── lib/
│               ├── libduckdb.dylib  (libduckdb.so on Linux)
│               └── libduckdb_static.a
├── src/
│   └── main.cpp
├── build/
└── CMakeLists.txt
```

## Usage

### Adding to CMakeLists.txt

```cmake
# Include DuckDB_C_CPP.cmake at the end of CMakeLists.txt
include("./cmake/DuckDB_C_CPP.cmake")
```

### Build

```bash
mkdir build && cd build
cmake ..
make
```

---

## Processing Flow

### 1. Setting the Directory Paths

```cmake
set(DUCKDB_DOWNLOAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB)
set(DUCKDB_SOURCE_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP)
set(DUCKDB_INSTALL_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP-install)
set(DUCKDB_BUILD_DIR ${DUCKDB_SOURCE_DIR}/_build)
set(DUCKDB_VERSION "1.5.6")
set(DUCKDB_URL "https://github.com/duckdb/duckdb/archive/refs/tags/v${DUCKDB_VERSION}.tar.gz")
```

### 2. Cache Check and Conditional Build

```cmake
if(EXISTS ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a)
    message(STATUS "DuckDB already built: ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a")
else()
    # Download, configure, build, and install ...
endif()
```

The cache logic works as follows:

| Condition | Action |
|-----------|--------|
| `DuckDB_C_CPP-install/lib/libduckdb_static.a` exists | Skip everything (use cached build) |
| `DuckDB_C_CPP/CMakeLists.txt` exists (install missing) | Skip download, run CMake configure/build/install |
| Nothing exists | Download, extract, CMake configure, build, install |

### 3. Download (if needed)

```cmake
file(DOWNLOAD
    ${DUCKDB_URL}
    ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION}.tar.gz
    SHOW_PROGRESS
    STATUS DOWNLOAD_STATUS
)
file(ARCHIVE_EXTRACT
    INPUT ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION}.tar.gz
    DESTINATION ${DUCKDB_DOWNLOAD_DIR}
)
file(RENAME ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION} ${DUCKDB_SOURCE_DIR})
```

- Downloads from GitHub Releases
- Extracts and renames `duckdb-1.5.6/` to `DuckDB_C_CPP/` for a clean path

### 4. Configure, Build, and Install (CMake)

```cmake
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DCMAKE_INSTALL_PREFIX=${DUCKDB_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHELL=FALSE
            -DBUILD_UNITTESTS=FALSE
            -DBUILD_BENCHMARKS=FALSE
            -DBUILD_COMPLETE_EXTENSION_SET=FALSE
            -DDISABLE_BUILTIN_EXTENSIONS=FALSE
            -DENABLE_EXTENSION_AUTOLOADING=FALSE
            -DENABLE_EXTENSION_AUTOINSTALL=FALSE
            -DSKIP_EXTENSIONS=parquet
            -DBUILD_EXTENSIONS=json
            -DOVERRIDE_GIT_DESCRIBE=v${DUCKDB_VERSION}-0-g0000000000
            -G Ninja
            ${DUCKDB_SOURCE_DIR}
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR}
)
execute_process(COMMAND ${CMAKE_COMMAND} --build . --config Release -j4
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR})
execute_process(COMMAND ${CMAKE_COMMAND} --install . --config Release
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR})
```

- `-DBUILD_SHELL=FALSE`: Disables building the CLI shell
- `-DBUILD_UNITTESTS=FALSE`: Disables building test binaries
- `-DBUILD_BENCHMARKS=FALSE`: Disables building benchmark suite
- `-DBUILD_COMPLETE_EXTENSION_SET=FALSE`: Disables building all extensions
- `-DDISABLE_BUILTIN_EXTENSIONS=FALSE`: Links the selected extensions into `libduckdb`
  (with `TRUE`, DuckDB links only `core_functions` and ignores `BUILD_EXTENSIONS`)
- `-DENABLE_EXTENSION_AUTOLOADING=FALSE`: Disables extension auto-loading
- `-DENABLE_EXTENSION_AUTOINSTALL=FALSE`: Disables extension auto-install
- `-DSKIP_EXTENSIONS=parquet`: Leaves out the Parquet extension
- `-DBUILD_EXTENSIONS=json`: Links the JSON extension (`json_extract()` and friends, used by the tests);
  the result links `json` and `core_functions`
- `-DOVERRIDE_GIT_DESCRIBE=...`: Makes the library report `v1.5.6`, the version the extension footer must match
- `-G Ninja`: Uses Ninja generator for faster builds
- `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`: Generates position-independent code
- All steps run at CMake configure time, not at build time

### 5. Linking the Library

```cmake
target_include_directories(${PROJECT_NAME} PRIVATE ${DUCKDB_INSTALL_DIR}/include)

file(GLOB DUCKDB_ALL_LIBS "${DUCKDB_INSTALL_DIR}/lib/libduckdb${CMAKE_SHARED_LIBRARY_SUFFIX}")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${DUCKDB_ALL_LIBS}
    Threads::Threads
)
```

`${PROJECT_NAME}` (the `a.out` runner) links the shared library
`libduckdb.dylib` / `libduckdb.so`.

How DuckDB-stats uses the build:

| Target | DuckDB headers | DuckDB library |
|---|---|---|
| `ext_funcs` (the extension) | `SYSTEM` include | **not linked**: DuckDB symbols are resolved from the host at `LOAD` time (`-undefined dynamic_lookup` on macOS) |
| `a.out` | via this file | `libduckdb` (shared) |
| `stat_tests` | `SYSTEM` include | `${DUCKDB_ALL_LIBS}` |

Because the extension uses DuckDB's C++ API, it loads only into a DuckDB whose
version equals `DUCKDB_VERSION`; the `LOAD` metadata footer records that
version (see `cmake/extension_metadata.cmake`).

---

## Key Features of DuckDB

| Feature | Description |
|---------|-------------|
| In-process | Runs within the host process, no separate server needed |
| SQL support | Full SQL support including joins, aggregations, window functions, CTEs |
| ACID transactions | Full transactional support with serializable isolation |
| Columnar storage | Column-oriented storage engine optimized for analytical queries |
| Vectorized execution | Processes data in vectors/batches for high performance |
| C/C++ API | Both C (`duckdb.h`) and C++ (`duckdb.hpp`) APIs available |
| Prepared statements | Parameterized queries for safe and efficient repeated execution |
| Data import/export | CSV, Parquet, JSON file reading and writing |
| In-memory & persistent | Supports both in-memory and file-based databases |
| Thread safety | Multi-threaded query execution with parallel operators |

---

## Usage Examples in C++

### Basic: In-Memory Database and Query

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    // Create an in-memory database
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    // Create a table and insert data
    con.Query("CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER)");
    con.Query("INSERT INTO users VALUES (1, 'Alice', 30)");
    con.Query("INSERT INTO users VALUES (2, 'Bob', 25)");
    con.Query("INSERT INTO users VALUES (3, 'Charlie', 35)");

    // SELECT query
    auto result = con.Query("SELECT * FROM users ORDER BY id");
    result->Print();

    return 0;
}
```

### Aggregation Queries

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE sales (product VARCHAR, amount DOUBLE, quantity INTEGER)");
    con.Query("INSERT INTO sales VALUES ('Apple', 1.50, 100)");
    con.Query("INSERT INTO sales VALUES ('Banana', 0.75, 200)");
    con.Query("INSERT INTO sales VALUES ('Apple', 1.50, 150)");
    con.Query("INSERT INTO sales VALUES ('Cherry', 3.00, 50)");

    // GROUP BY with aggregation
    auto result = con.Query(
        "SELECT product, SUM(amount * quantity) AS revenue, SUM(quantity) AS total_qty "
        "FROM sales GROUP BY product ORDER BY revenue DESC"
    );
    result->Print();

    return 0;
}
```

### Prepared Statements

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE employees (id INTEGER, name VARCHAR, salary DOUBLE)");
    con.Query("INSERT INTO employees VALUES (1, 'Alice', 75000)");
    con.Query("INSERT INTO employees VALUES (2, 'Bob', 65000)");
    con.Query("INSERT INTO employees VALUES (3, 'Charlie', 85000)");

    // Prepared statement with parameter
    auto prepared = con.Prepare("SELECT * FROM employees WHERE salary >= $1");
    auto result = prepared->Execute(70000);
    result->Print();

    return 0;
}
```

### Persistent Database (File-Based)

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    // Create or open a persistent database file
    duckdb::DuckDB db("my_database.duckdb");
    duckdb::Connection con(db);

    con.Query("CREATE TABLE IF NOT EXISTS logs (ts TIMESTAMP, message VARCHAR)");
    con.Query("INSERT INTO logs VALUES (NOW(), 'Application started')");

    auto result = con.Query("SELECT * FROM logs ORDER BY ts DESC LIMIT 10");
    result->Print();

    return 0;
}
```

### Window Functions

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE scores (student VARCHAR, subject VARCHAR, score INTEGER)");
    con.Query("INSERT INTO scores VALUES ('Alice', 'Math', 90)");
    con.Query("INSERT INTO scores VALUES ('Alice', 'Science', 85)");
    con.Query("INSERT INTO scores VALUES ('Bob', 'Math', 78)");
    con.Query("INSERT INTO scores VALUES ('Bob', 'Science', 92)");
    con.Query("INSERT INTO scores VALUES ('Charlie', 'Math', 95)");
    con.Query("INSERT INTO scores VALUES ('Charlie', 'Science', 88)");

    // Window function: rank within each subject
    auto result = con.Query(
        "SELECT student, subject, score, "
        "RANK() OVER (PARTITION BY subject ORDER BY score DESC) AS rank "
        "FROM scores ORDER BY subject, rank"
    );
    result->Print();

    return 0;
}
```

### Multiple Connections and Transactions

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);

    // Connection 1: Create table and insert data
    duckdb::Connection con1(db);
    con1.Query("CREATE TABLE accounts (id INTEGER, balance DOUBLE)");
    con1.Query("INSERT INTO accounts VALUES (1, 1000.0)");
    con1.Query("INSERT INTO accounts VALUES (2, 2000.0)");

    // Connection 2: Transaction with BEGIN/COMMIT
    duckdb::Connection con2(db);
    con2.Query("BEGIN TRANSACTION");
    con2.Query("UPDATE accounts SET balance = balance - 500 WHERE id = 1");
    con2.Query("UPDATE accounts SET balance = balance + 500 WHERE id = 2");
    con2.Query("COMMIT");

    auto result = con1.Query("SELECT * FROM accounts ORDER BY id");
    result->Print();

    return 0;
}
```

---

## DuckDB C++ API Key Classes

| Class | Description |
|-------|-------------|
| `duckdb::DuckDB` | Database instance; pass `nullptr` for in-memory, or a file path for persistent |
| `duckdb::Connection` | Database connection for executing queries |
| `duckdb::QueryResult` | Result of a query; use `Print()` to display or iterate rows |
| `duckdb::PreparedStatement` | Compiled parameterized query for repeated execution |
| `duckdb::Appender` | Bulk data insertion interface for high-throughput loading |
| `duckdb::Value` | Represents a single value with type information |
| `duckdb::DataChunk` | Vectorized data batch used internally for query results |

---

## Commonly Used Methods

| Method | Description |
|--------|-------------|
| `DuckDB(path)` | Create database instance (`nullptr` = in-memory, `"file.db"` = persistent) |
| `Connection(db)` | Create a connection to a database |
| `con.Query(sql)` | Execute a SQL query and return the result |
| `con.Prepare(sql)` | Prepare a parameterized SQL statement |
| `prepared->Execute(args...)` | Execute a prepared statement with arguments |
| `result->Print()` | Print query result to stdout |
| `result->Fetch()` | Fetch the next DataChunk from the result |
| `result->GetValue(col, row)` | Get a specific value from the result |
| `Appender(con, table)` | Create a bulk appender for a table |
| `appender.AppendRow(args...)` | Append a row of values |
| `appender.Close()` | Flush and close the appender |

---

## DuckDB SQL Type Mapping

| SQL Type | C++ Type | Description |
|----------|----------|-------------|
| `BOOLEAN` | `bool` | True/false value |
| `TINYINT` | `int8_t` | 8-bit signed integer |
| `SMALLINT` | `int16_t` | 16-bit signed integer |
| `INTEGER` | `int32_t` | 32-bit signed integer |
| `BIGINT` | `int64_t` | 64-bit signed integer |
| `FLOAT` | `float` | 32-bit floating point |
| `DOUBLE` | `double` | 64-bit floating point |
| `VARCHAR` | `std::string` | Variable-length string |
| `DATE` | `duckdb::date_t` | Calendar date |
| `TIMESTAMP` | `duckdb::timestamp_t` | Date and time |
| `BLOB` | `std::string` | Binary large object |
| `DECIMAL` | `duckdb::hugeint_t` | Fixed-point decimal |

---

## CMake Build Options Reference

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHELL` | TRUE | Build the DuckDB CLI shell |
| `BUILD_UNITTESTS` | TRUE | Build C++ unit tests |
| `BUILD_BENCHMARKS` | FALSE | Build benchmark suite |
| `BUILD_COMPLETE_EXTENSION_SET` | TRUE | Build all extensions |
| `DISABLE_BUILTIN_EXTENSIONS` | FALSE | Disable built-in extensions |
| `ENABLE_EXTENSION_AUTOLOADING` | FALSE | Auto-load extensions at runtime |
| `ENABLE_EXTENSION_AUTOINSTALL` | FALSE | Auto-install extensions |
| `BUILD_EXTENSIONS` | (varies) | Semicolon-separated list of extensions to build |
| `DISABLE_THREADS` | FALSE | Disable multi-threading |
| `OSX_BUILD_UNIVERSAL` | FALSE | Build universal binary on macOS |
| `SMALLER_BINARY` | FALSE | Optimize for smaller binary size |

---

## Troubleshooting

### Download Fails

If GitHub is unreachable, you can manually download and place the tarball:

```bash
curl -L -o download/DuckDB/duckdb-1.5.6.tar.gz https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.6.tar.gz
```

Then re-run `cmake ..` and the extraction will proceed from the cached tarball.

### Configure Fails

Ensure CMake 3.20+ and Ninja are available:

```bash
cmake --version
ninja --version
```

On macOS, ensure Xcode Command Line Tools are installed:

```bash
xcode-select --install
```

Install Ninja if not available:

```bash
brew install ninja
```

### Rebuild DuckDB from Scratch

To force a full rebuild, remove the install and source directories:

```bash
rm -rf download/DuckDB/DuckDB_C_CPP-install download/DuckDB/DuckDB_C_CPP
cd build && cmake ..
```

### Link Error: Undefined Reference to DuckDB Symbols

Verify that `libduckdb_static.a` exists in `download/DuckDB/DuckDB_C_CPP-install/lib/`. If missing, delete the install directory and re-run cmake.

Also ensure that system libraries (`pthread`, `dl`, `m`) are linked. These are required by the static library.

### `duckdb.hpp` Not Found

Ensure `target_include_directories` points to the correct install directory. The installed headers are at `download/DuckDB/DuckDB_C_CPP-install/include/`.

### Build Takes Too Long

DuckDB is a large project. The initial build from source may take 10-30 minutes depending on hardware. Subsequent builds use the cached artifacts in `download/DuckDB/DuckDB_C_CPP-install/`.

To speed up builds:

- Use Ninja generator (already configured with `-G Ninja`)
- Increase parallel jobs: modify `-j4` to `-j$(nproc)` in the cmake file
- Disable extensions (already configured)

---

## References

- [DuckDB GitHub Repository](https://github.com/duckdb/duckdb)
- [DuckDB Documentation](https://duckdb.org/docs/)
- [DuckDB C++ API Reference](https://duckdb.org/docs/stable/api/cpp/overview)
- [DuckDB C API Reference](https://duckdb.org/docs/stable/api/c/overview)
- [DuckDB Building from Source](https://duckdb.org/docs/stable/dev/building/overview)
- [CMake execute_process Documentation](https://cmake.org/cmake/help/latest/command/execute_process.html)
- [CMake file(DOWNLOAD) Documentation](https://cmake.org/cmake/help/latest/command/file.html#download)
