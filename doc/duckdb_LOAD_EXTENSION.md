# DuckDB Loadable Extension Guide

[日本語](duckdb_LOAD_EXTENSION-ja.md)

An implementation guide for the DuckDB loadable extension of this project.
The extension is written in C++ (`src/ext_funcs.cpp`) against DuckDB's C++ extension API.

---

## Table of Contents

1. [What is a Loadable Extension](#1-what-is-a-loadable-extension)
2. [Project Structure](#2-project-structure)
3. [Build and Run](#3-build-and-run)
4. [How Loadable Extensions Work](#4-how-loadable-extensions-work)
5. [Extension Function Implementation Patterns](#5-extension-function-implementation-patterns)
6. [DuckDB C++ API Reference](#6-duckdb-c-api-reference)
7. [Window Functions in DuckDB](#7-window-functions-in-duckdb)
8. [Dynamic Library Considerations](#8-dynamic-library-considerations)

---

## 1. What is a Loadable Extension

### Overview

A DuckDB loadable extension is a shared library that DuckDB opens at runtime with `LOAD`.
It registers new SQL functions into the running database without rebuilding DuckDB.
The file name ends in `.duckdb_extension`, and a metadata footer at the end of the file tells DuckDB which
platform and version the extension was built for.

DuckDB offers two extension APIs:

| API | ABI | Loads into | Window functions |
|---|---|---|---|
| C extension API (`duckdb_extension.h`) | stable | any DuckDB with a C API at least as new as the one declared | not available |
| C++ extension API (`duckdb.hpp`) | none | **only the exact DuckDB version** it was compiled against | `window` / `window_init` callbacks |

This project uses the **C++ API**. The 26 window functions compute their result over the whole partition and
return the element of the current row; that needs the custom window callbacks, which the stable C API does not
expose. With the C API, 15 of them (`stat_rank`, `stat_fillna_mean`, `stat_bh_correction` and so on) could not be
implemented. The price is the version pin described below.

### Capabilities

- Scalar functions, aggregate functions and aggregate functions with custom window evaluation
- Function overloads by argument count (DuckDB resolves `stat_normal_cdf(x)` and `stat_normal_cdf(x, mu, sigma)`
  to different overloads)
- Implicit casts of the arguments: every argument is declared `DOUBLE`, so `INTEGER`, `BIGINT`, `DECIMAL` and `FLOAT`
  inputs are cast before they reach the extension
- `VOLATILE` functions, which DuckDB neither constant-folds nor caches (used for the `_rand` functions)

### Limitations

- **Version pin**: a C++ extension loads only into the DuckDB version it was built for (v1.5.6 here). DuckDB checks
  the version in the footer and refuses any other.
- **Unsigned**: the extension is not signed by DuckDB, so it loads only with `allow_unsigned_extensions` enabled
  (`duckdb -unsigned`).
- **Windows (MSVC)** is not supported (not built or tested). A C++ extension resolves DuckDB's C++ symbols from
  the host process, which needs a different linking setup on Windows.
- A scalar function cannot share its name with an aggregate or window function. The scalar form of
  `stat_bonferroni` is therefore named `stat_bonferroni_scalar`.

### Security Notes

Loading an extension runs native code inside the database process with the privileges of that process.
Load only extensions you built yourself or trust. `allow_unsigned_extensions` lifts DuckDB's signature check for
every unsigned extension, not only this one; enable it only where needed.

---

## 2. Project Structure

```text
DuckDB-stats/
├── CMakeLists.txt
├── src/
│   ├── ext_funcs.cpp              # The extension: 259 SQL functions
│   └── main.cpp                   # Integration runner (266 demos)
├── tests/                         # Google Test suites (451 tests)
├── cmake/
│   ├── DuckDB_C_CPP.cmake         # Downloads and builds DuckDB from source
│   ├── statcpp.cmake              # Downloads statcpp (header-only)
│   ├── extension_metadata.cmake   # Appends the LOAD metadata footer
│   └── null.txt                   # A single 0x00 byte used by the footer script
└── doc/
```

### External Libraries

#### statcpp

[statcpp](https://github.com/mitsuruk/statcpp) is a C++17 header-only statistics library. Every statistical
computation of the extension is delegated to it; the extension only adapts SQL rows to statcpp calls.

#### DuckDB

`cmake/DuckDB_C_CPP.cmake` downloads the DuckDB v1.5.6 source and builds it once into `download/DuckDB/`. The
extension takes only the headers from that build; the runner and the tests link `libduckdb`. See
[DuckDB_C_CPPCmake.md](../cmake/DuckDB_C_CPPCmake.md).

### Provided SQL Functions

| Category | Count | Kind |
|---|---|---|
| Basic aggregates | 24 | aggregate |
| Parameterized aggregates | 20 | aggregate |
| Two-column aggregates | 27 | aggregate |
| Window functions | 26 | aggregate with custom window evaluation |
| Complex aggregates | 41 | aggregate (mostly JSON results) |
| Scalar — tests/helpers | 38 | scalar |
| Scalar — distributions | 83 | scalar |
| **Total** | **259** | |

The full list is in the [Function Reference](function_reference.md).

---

## 3. Build and Run

```bash
# Configure (the first run builds DuckDB from source; this takes a while)
cmake -B build

# Build the extension and the runner
cmake --build build

# Run the integration runner (266 demos)
./build/a.out

# Build and run the Google Test suites
cmake -B build -DSTAT_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces:

| File | Description |
|---|---|
| `build/ext_funcs.duckdb_extension` | The loadable extension (with metadata footer) |
| `build/ext_funcs.duckdb_extension_raw` | The linked module before the footer is appended |
| `build/a.out` | Integration runner |
| `build/stat_tests` | Google Test executable (`-DSTAT_TESTS=ON`) |

### Using from the duckdb CLI

#### Prerequisites

- The duckdb CLI of **exactly** the version the extension was built for (v1.5.6)
- Unsigned extensions allowed: start the CLI with `-unsigned`

#### Loading

```sql
-- duckdb -unsigned
LOAD './build/ext_funcs.duckdb_extension';

SELECT stat_mean(x) FROM range(1, 11) t(x);
-- 5.5
```

A version mismatch is reported at `LOAD` time, before any code of the extension runs:

```text
Invalid Input Error: Failed to load 'build/ext_funcs.duckdb_extension', The file was built
specifically for DuckDB version 'v1.5.6' and can only be loaded with that version of DuckDB.
(this version of DuckDB is 'v1.4.4')
```

The extension built against the locally compiled DuckDB has been checked to load into the official
duckdb CLI v1.5.6 release for macOS and to run every function there.

### Using from C++

```cpp
duckdb::DBConfig config;
config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
duckdb::DuckDB db(nullptr, &config);
duckdb::Connection con(db);
auto r = con.Query("LOAD './build/ext_funcs.duckdb_extension'");
if (r->HasError()) { /* r->GetError() */ }
```

The program must link the same DuckDB version (`libduckdb`) as the extension was compiled against.

---

## 4. How Loadable Extensions Work

### Overall Flow

```text
LOAD 'ext_funcs.duckdb_extension'
  │
  ├─ 1. read the 534-byte metadata footer at the end of the file
  │     └─ check platform (osx_arm64), DuckDB version (v1.5.6), ABI type (CPP),
  │        and the signature (skipped when allow_unsigned_extensions is on)
  ├─ 2. dlopen() the file
  ├─ 3. look up the symbol ext_funcs_duckdb_cpp_init
  └─ 4. call it with an ExtensionLoader&; the extension registers its functions
```

### Entry Point Naming Convention

The entry point is `<extension name>_duckdb_cpp_init`, where the extension name is the file name without
`.duckdb_extension`. It is declared with DuckDB's macro, wrapped in `extern "C"`:

```cpp
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ext_funcs, loader) {
    duckdb_stats::register_all(loader);
}

}
```

Without `extern "C"` the symbol is C++-mangled and `LOAD` fails with "did not contain the expected entrypoint
function".

### Metadata Footer

DuckDB refuses a file without the footer. `cmake/extension_metadata.cmake` appends it to a copy of the linked
module after every build (so a rebuild never stacks a second footer):

| Field | Value |
|---|---|
| 1 | `4` (footer format) |
| 2 | platform, e.g. `osx_arm64` |
| 3 | DuckDB version, e.g. `v1.5.6` — matched **exactly** for ABI type `CPP` |
| 4 | extension version |
| 5 | ABI type `CPP` |
| signature | 256 zero bytes (unsigned) |

### Compile Flags

- C++17, `-Wall -Wextra -Wpedantic`
- DuckDB headers are included as `SYSTEM` headers so their own warnings stay out of the build
- macOS: `-undefined dynamic_lookup` (DuckDB symbols are resolved from the host when loaded)
- Linux: DuckDB is linked statically into the extension, with hidden visibility, `--exclude-libs,ALL`,
  `--gc-sections` and `--no-undefined` (see [DuckDB Symbols](#duckdb-symbols))

---

## 5. Extension Function Implementation Patterns

### 5.1. Argument and Result Access

The statistical code is shared with sqlite3-stats. The SQLite version reads arguments with
`sqlite3_value_double()` and writes results with `sqlite3_result_*()`; the DuckDB version keeps the same shape
through two small classes:

| Class | Counterpart | Semantics |
|---|---|---|
| `ArgRow` | `sqlite3_value** argv` | `Double(i)`, `Int64(i)`, `Int(i)`, `IsNull(i)`, `Count()`; a NULL argument reads as `0.0` |
| `ResultSlot` | `sqlite3_context*` | `SetDouble`, `SetInt64`, `SetText`, `SetNull`; NaN / Inf and empty text become NULL |

Functions are registered with `FunctionNullHandling::SPECIAL_HANDLING`, so NULL arguments reach the code and are
handled as in the SQLite version: a scalar returns NULL when any argument is NULL, and a NULL parameter makes an
aggregate return NULL and a window function return NULL on every row (see [NULL, NaN and Inf Notes](nan_notes.md)).

### 5.2. Aggregate Functions

Every aggregate is described by a small `Spec` and turned into a DuckDB `AggregateFunction` by
`StatAggregate<Spec>`:

```cpp
struct Spec {
    struct State { /* collected rows */ };
    static void Step(State&, const ArgRow&);          // one input row
    static void Merge(State& target, const State&);   // combine two partial states
    static void Final(const State&, ResultSlot&);     // compute the result
};
```

DuckDB gives every group a fixed-size state slot. The slot holds a pointer to a heap-allocated `State`, created
on the first row and deleted by the aggregate's destructor callback, so it is released on every path.

`Merge` must leave its source unchanged: when an aggregate is evaluated over a window frame, DuckDB combines
segment-tree nodes and reuses them afterwards.

The SQLite templates map onto Specs as follows:

| SQLite template | DuckDB Spec |
|---|---|
| `SingleColumnAggregate` | `SingleColumnAggregate<Func>` |
| `SingleColumnParamAggregate[Text]` | `SingleColumnParamAggregate[Text]<N, Func>` |
| `TwoColumnAggregate[Text]` | `TwoColumnAggregate[Text]<Func>` |
| `TwoColumnParamAggregate[Text]` | `TwoColumnParamAggregate[Text]<N, Func>` |
| `SingleColumnAggregateText` | `SingleColumnAggregateText<Func>` |
| `FullScanWindowFunction` | `FullScanWindowFunction<Func>` (see [section 7](#7-window-functions-in-duckdb)) |
| `logrank_step` / `logrank_final` | `LogrankAggregate` |

### 5.3. Scalar Functions

A scalar implementation keeps the SQLite signature shape:

```cpp
static void sf_normal_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::normal_cdf(x, mu, sigma));
}

register_scalar<sf_normal_cdf>(loader, "stat_normal_cdf", {1, 2, 3});
```

SQLite registered such functions with a variable argument count (`-1`); DuckDB registers one overload per
accepted count, so a wrong count is a binder error. The return type is fixed per function: `DOUBLE`, `BIGINT`
(quantiles of discrete distributions, sample sizes) or `VARCHAR` (JSON and text). `register_scalar_nd` registers a
`VOLATILE` function.

### 5.4. Errors

statcpp reports invalid arguments by throwing `std::invalid_argument`. Every callback runs through
`invoke_guarded()`, which converts any non-DuckDB exception into `duckdb::InvalidInputException`:

```text
D SELECT stat_poisson_quantile(1.5, 2.5);
Invalid Input Error: statcpp::poisson_quantile: p must be in [0, 1]
```

The statement fails and the connection stays usable.

---

## 6. DuckDB C++ API Reference

The DuckDB APIs the extension uses:

| API | Use |
|---|---|
| `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)` | Declares the entry point |
| `ExtensionLoader::RegisterFunction(...)` | Registers a `ScalarFunctionSet` / `AggregateFunctionSet` |
| `ScalarFunction(args, ret, fn)` | Scalar function; `fn(DataChunk&, ExpressionState&, Vector&)` |
| `ScalarFunction::SetNullHandling` / `SetVolatile` | NULL handling and stability |
| `AggregateFunction(args, ret, size, init, update, combine, finalize, null_handling, simple_update, bind, destructor)` | Aggregate function |
| `AggregateFunction::window`, `SetWindowInitCallback` | Custom window evaluation |
| `WindowPartitionInput` | The partition's argument rows (a `ColumnDataCollection`) |
| `SubFrames` | The frame bounds of the row being evaluated |
| `Vector::ToUnifiedFormat`, `UnifiedVectorFormat::GetData` | Reading arguments and state pointers |
| `FlatVector::GetData`, `FlatVector::SetNull`, `StringVector::AddString` | Writing results |
| `duckdb::InvalidInputException` | Reporting a SQL error |

The C++ API is not stable: names and signatures change between DuckDB releases. Upgrading DuckDB means
rebuilding the extension and, occasionally, adapting this layer.

---

## 7. Window Functions in DuckDB

### The Problem

The SQLite version collects every row of the partition and returns one element of the result per row; it relies
on SQLite calling `xValue` once per row with the full-partition frame
`ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING`.

DuckDB evaluates `OVER ()` and full-partition frames with its **constant aggregator**: it computes the aggregate
once per partition and copies the value to every row. No extension callback can return per-row values in that
case.

### The Design

The window functions use DuckDB's custom window callbacks:

1. `window_init` receives the whole partition (`WindowPartitionInput`) once. It runs the statcpp computation and
   keeps the result vector in the shared state.
2. `window` is called for every row. It is **not** told the row's position in the partition, only its frame
   bounds (`SubFrames`). The row is therefore identified as **the last row of its frame**, which holds for the
   default frame of `OVER (ORDER BY ...)` and for `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`.

```sql
SELECT id, val, stat_rank(val) OVER (PARTITION BY grp ORDER BY id) AS r
FROM t
ORDER BY id;
```

### Consequences

| Call | Result |
|---|---|
| `OVER (ORDER BY key)` with a unique key | correct per-row result |
| `OVER (PARTITION BY g ORDER BY key)` | correct, computed per partition |
| `OVER (ORDER BY key)` with ties in `key` | tied rows share the result of the last tied row |
| `OVER ()`, full-partition frame, or no `OVER` | error: "this is a window function: call it with an ORDER BY in the OVER clause" |

A call without `OVER` and `OVER ()` both reach the aggregate's finalize step and cannot be told apart, so both
raise the error.

The computation is done once per partition, so the cost is that of a single statcpp call, independent of the
frame.

---

## 8. Dynamic Library Considerations

### Entry Point Symbol

```bash
nm -gU build/ext_funcs.duckdb_extension_raw | grep init
# T _ext_funcs_duckdb_cpp_init
```

### DuckDB Symbols

On macOS the extension does not link DuckDB. It is linked with `-undefined dynamic_lookup`, and the 32 DuckDB
symbols it references are resolved from the host process when it is loaded: the duckdb CLI, or a program that
links `libduckdb`. The official duckdb CLI for macOS exports DuckDB's C++ symbols, so this works.

On Linux the same approach fails: the official duckdb CLI for Linux exports no symbols at all, and `LOAD` stops
with `undefined symbol: _ZTIN6duckdb18BaseScalarFunctionE` or similar. The extension therefore links DuckDB
statically, as DuckDB builds its own Linux extensions (`EXTENSION_STATIC_BUILD` in DuckDB's
`extension_build_tools.cmake`):

- `libduckdb_static.a`, `libdummy_static_extension_loader.a` and the bundled third-party libraries
  (`libduckdb_*.a`) are linked; the built-in extension loader and jemalloc are left out, since the host
  provides them
- hidden visibility and `--exclude-libs,ALL` keep every symbol private except the entry point, so the bundled
  copy does not clash with a host that links `libduckdb` (`a.out`, `stat_tests`)
- `--no-undefined` reports a missing symbol at link time instead of at `LOAD`

The Linux extension is about 37 MB instead of about 2 MB.

```bash
otool -L build/ext_funcs.duckdb_extension_raw
#   /usr/lib/libc++.1.dylib
#   /usr/lib/libSystem.B.dylib
```

Because the symbols come from the host, the host must be the same DuckDB version and must have been built with
compatible settings. The footer's exact version match is DuckDB's guard against the first; the second holds for
release builds of the same tag.

### Version Upgrades

To move to a new DuckDB release, change `DUCKDB_VERSION` in `cmake/DuckDB_C_CPP.cmake`, delete
`download/DuckDB/DuckDB_C_CPP-install`, reconfigure, and run the tests. The duckdb CLI used to load the extension
must be upgraded to the same version.
