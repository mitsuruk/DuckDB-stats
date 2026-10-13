# Statistical Functions for DuckDB

A loadable extension that adds **259 statistical functions** to DuckDB.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![DuckDB](https://img.shields.io/badge/DuckDB-v1.5.6-yellow.svg)](https://duckdb.org/)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](#tested-environments)

[日本語](README-ja.md)

## Overview

DuckDB-stats is a DuckDB loadable extension that provides 259 statistical functions callable directly from SQL. It is the DuckDB port of [sqlite3-stats](https://github.com/mitsuruk/sqlite3-stats), the SQLite3 extension, and keeps its function names, arguments and results. Like sqlite3-stats it is built on [statcpp](https://github.com/mitsuruk/statcpp) (a C++17 header-only statistics library with 386 functions).

All 259 functions are verified by 266 integration tests and 451 Google Test automated tests.

### Key Features

- **259 SQL functions** covering descriptive statistics, hypothesis testing, distributions, resampling, and more
- **One file** — the extension is a single `ext_funcs.duckdb_extension`
- **Standard SQL interface** — use with `SELECT`, `GROUP BY`, `HAVING`, subqueries, etc.
- **Window function support** — rolling statistics, moving averages, ranking, imputation, outlier detection
- **JSON output** — complex results (multi-value, frequency tables, test results) returned as JSON text
- **Platforms** — macOS and Linux

### Function Categories

| Category | Count | Description |
| --- | --- | --- |
| Basic aggregates | 24 | Single-column: `SELECT stat_xxx(col) FROM t` |
| Parameterized aggregates | 20 | With parameters: `SELECT stat_xxx(col, param) FROM t` |
| Two-column aggregates | 27 | Two-column input: `SELECT stat_xxx(col1, col2) FROM t` |
| Window functions | 26 | Full-scan window functions: `stat_xxx(col) OVER (ORDER BY id)` |
| Complex aggregates | 41 | JSON-returning aggregates, two-sample tests, survival analysis |
| Scalar — tests/helpers | 38 | Distribution functions, proportion tests, multiple testing corrections |
| Scalar — distributions | 83 | Continuous/discrete distributions, effect size conversions, power analysis |

## Quick Start

### Prerequisites

- CMake 3.20 or later, and Ninja (macOS: `brew install ninja`, Ubuntu: `sudo apt install ninja-build`)
- 8 GB of memory or more to build DuckDB from source (CMake compiles it with 4 parallel jobs)
- C++17 compiler: Apple Clang 17+ or GCC 13+
- DuckDB v1.5.6 (built from source automatically by CMake)
- [statcpp](https://github.com/mitsuruk/statcpp) (downloaded automatically by CMake)

### Build

```bash
cd DuckDB-stats
cmake -B build          # the first run builds DuckDB from source (10-30 minutes)
cmake --build build
```

This produces:

- `build/ext_funcs.duckdb_extension` — the loadable extension
- `build/a.out` — integration test runner (266 tests)

### Run the Tests

```bash
./build/a.out
# [OK] All tests completed (266 tests).
```

#### Google Test Suite

Requires [Google Test](https://github.com/google/googletest) (macOS: `brew install googletest`).

```bash
cmake -B build -DSTAT_TESTS=ON
cmake --build build

# Run via CTest
ctest --test-dir build --output-on-failure

# Run the Google Test executable directly
./build/stat_tests

# Run a specific test suite
./build/stat_tests --gtest_filter="BasicAggregates.*"
```

| Test File | Functions | Tests |
|---|---|---|
| basic_aggregates_test.cpp | 24 | 63 |
| parameterized_aggregates_test.cpp | 20 | 53 |
| two_column_aggregates_test.cpp | 27 | 58 |
| window_functions_test.cpp | 26 | 61 |
| complex_aggregates_test.cpp | 41 | 80 |
| scalar_tests_helpers_test.cpp | 38 | 43 |
| scalar_distributions_test.cpp | 83 | 93 |
| **Total** | **259** | **451** |

### Loading the Extension

The extension is unsigned, and it is built with DuckDB's C++ extension API, so it loads only into **DuckDB v1.5.6** with unsigned extensions allowed.

#### From the duckdb CLI

```sql
-- start the CLI with: duckdb -unsigned
LOAD './build/ext_funcs.duckdb_extension';
```

The official duckdb CLI on macOS refuses a relative path ("relative path not allowed in hardened
program"). Give the absolute path instead, e.g. `duckdb -unsigned -cmd "LOAD '$PWD/build/ext_funcs.duckdb_extension'"`.

#### From C++

```cpp
duckdb::DBConfig config;
config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
duckdb::DuckDB db(nullptr, &config);
duckdb::Connection con(db);
con.Query("LOAD './build/ext_funcs.duckdb_extension'");
```

## Usage Examples

### Descriptive Statistics

```sql
SELECT stat_mean(score), stat_stdev(score), stat_median(score)
FROM exam_results;

SELECT class, stat_mean(score), stat_cv(score)
FROM exam_results
GROUP BY class;
```

### Hypothesis Testing

```sql
-- One-sample t-test (H0: mu = 50)
SELECT stat_t_test(score, 50) FROM exam_results;

-- Two-sample Welch's t-test (returns JSON)
SELECT stat_t_test_welch(group_a, group_b) FROM measurements;

-- Pick a field out of the JSON result
SELECT json_extract(stat_t_test(score, 50), '$.p_value')::DOUBLE AS p
FROM exam_results;
```

### Window Functions

```sql
SELECT date, price,
       stat_rolling_mean(price, 7) OVER (ORDER BY date) AS ma7,
       stat_ema(price, 5)          OVER (ORDER BY date) AS ema
FROM stock_prices
ORDER BY date;
```

### Distribution Functions (Scalar)

```sql
-- Normal distribution quantile (95% z-value)
SELECT stat_normal_quantile(0.975, 0, 1);  -- 1.96

-- Power analysis: required sample size for a two-sample t-test
SELECT stat_n_t2(0.5, 0.8, 0.05);  -- effect size=0.5, power=0.80, alpha=0.05
```

## Sample Database

`tests/sample/` contains a small data set and queries that you can run as they are,
to see what the extension does at a glance:

| File | Contents |
| --- | --- |
| `sample_data.sql` | Table `students`: exam results of 100 students (`id`, `class`, `study_hours`, `score`) |
| `sample_queries.sql` | Seven queries: descriptive statistics, `GROUP BY`, a two-sample test, correlation and regression, window functions, and a distribution function |

The score rises with study time, class B scores higher on average, and three unusual
results are planted. Run from the project root after building:

```bash
duckdb -unsigned \
    -cmd "LOAD '$PWD/build/ext_funcs.duckdb_extension'" \
    -c ".read tests/sample/sample_data.sql" \
    -c ".read tests/sample/sample_queries.sql"
```

Selected results (DuckDB v1.5.6):

| Question | Function | Result |
| --- | --- | --- |
| Typical score | `stat_mean`, `stat_median` | mean 65.52, median 68.0 |
| Does class B score higher? | `stat_t_test_welch` | p = 0.013 (significant), Cohen's d = -0.51 |
| Does studying pay off? | `stat_pearson_r`, `stat_simple_regression` | r = 0.638, +4.07 points per hour |
| Which scores are outliers? | `stat_outliers_iqr` (window) | students 88 and 64 (scores 9 and 14) |
| Share above 80 under a normal model | `stat_normal_cdf` | 0.202 (observed 0.20) |

## Common Specifications

- **NULL handling**: NULL values are silently ignored in all aggregate and window functions.
- **Insufficient data**: Returns `NULL` when there are not enough non-NULL values (e.g., variance requires n >= 2).
- **Numeric precision**: IEEE 754 double precision (64-bit). `NaN` and `Inf` are mapped to `NULL`.
- **Return types**: `DOUBLE`, `BIGINT` (quantiles of discrete distributions, sample sizes) or `VARCHAR` (JSON and text).
- **JSON output**: Complex aggregate results are returned as JSON strings (`VARCHAR`). DuckDB's `json_extract()` returns a JSON value, so cast numbers with `::DOUBLE`.
- **Invalid arguments**: raise an ordinary SQL error (`Invalid Input Error: statcpp::...`); the connection stays usable.
- **Random functions**: the `_rand` scalar functions are `VOLATILE`, so every row gets its own draw.

## Differences from sqlite3-stats

| Item | sqlite3-stats | DuckDB-stats |
|---|---|---|
| Loading | `.load ./ext_funcs sqlite3_ext_funcs_init` | `LOAD './build/ext_funcs.duckdb_extension'` (`duckdb -unsigned`) |
| Engine version | any SQLite3 with extension loading | exactly DuckDB v1.5.6 (C++ extension API) |
| Window functions | full-partition frame `OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)` | `OVER (ORDER BY id)`; `OVER ()` and calls without `OVER` raise an error ([details](doc/ref/window_functions.md#how-to-call)) |
| Scalar Bonferroni | `stat_bonferroni(p, m)` | `stat_bonferroni_scalar(p, m)` (a scalar cannot share the name of the window function `stat_bonferroni(p)`) |
| Function count | 258 | 259 (the rename above) |
| Variadic scalars | one registration taking any argument count | one overload per accepted argument count; a wrong count is a binder error |
| Return types | REAL / INTEGER / TEXT | DOUBLE / BIGINT / VARCHAR |
| NaN in the data | cannot be stored (becomes `NULL`) | a value distinct from `NULL`, passed to statcpp ([details](doc/nan_notes.md)) |
| Inf result of a window function | returned as Inf | returned as `NULL`, as for the aggregates |
| `_rand` functions | deterministic flag | `VOLATILE` |
| Platforms | macOS, Linux, Windows (MSVC) | macOS, Linux |

The rolling family, `stat_moving_avg` and `stat_lag` contradicted their documentation in earlier versions of sqlite3-stats; they were corrected in this port, and sqlite3-stats has since been corrected the same way. The `NULL` rules for parameters and scalar arguments are the same in both.

## Function Reference

Detailed documentation for all 259 functions:

- [Function Reference (hub)](doc/function_reference.md)
  - [Basic Aggregates (24)](doc/ref/basic_aggregates.md)
  - [Parameterized Aggregates (20)](doc/ref/parameterized_aggregates.md)
  - [Two-Column Aggregates (27)](doc/ref/two_column_aggregates.md)
  - [Window Functions (26)](doc/ref/window_functions.md)
  - [Complex Aggregates (41)](doc/ref/complex_aggregates.md)
  - [Scalar — Tests & Helpers (38)](doc/ref/scalar_tests_helpers.md)
  - [Scalar — Distributions & Transforms (83)](doc/ref/scalar_distributions.md)
- [NULL, NaN and Inf Notes](doc/nan_notes.md)
- [DuckDB Loadable Extension Guide](doc/duckdb_LOAD_EXTENSION.md) — how the extension is built and why it uses the C++ API

## Project Structure

```text
DuckDB-stats/
├── CMakeLists.txt
├── LICENSE
├── README.md                              # This file
├── README-ja.md                           # Japanese README
├── mkdocs.yml                             # Documentation site (mkdocs serve)
├── tests/
│   ├── test_helpers.hpp                   # Google Test fixture and helpers
│   ├── sample/                            # Sample database and queries (100 students)
│   ├── basic_aggregates_test.cpp          # 24 functions, 63 tests
│   ├── parameterized_aggregates_test.cpp  # 20 functions, 53 tests
│   ├── two_column_aggregates_test.cpp     # 27 functions, 58 tests
│   ├── window_functions_test.cpp          # 26 functions, 61 tests
│   ├── complex_aggregates_test.cpp        # 41 functions, 80 tests
│   ├── scalar_tests_helpers_test.cpp      # 38 functions, 43 tests
│   └── scalar_distributions_test.cpp      # 83 functions, 93 tests
├── src/
│   ├── ext_funcs.cpp                      # Extension: 259 SQL functions
│   └── main.cpp                           # Integration test runner (266 tests)
├── doc/
│   ├── index.md                           # Documentation home
│   ├── function_reference.md              # Function reference (English)
│   ├── function_reference-ja.md           # Function reference (Japanese)
│   ├── nan_notes.md                       # NULL, NaN and Inf notes (English)
│   ├── nan_notes-ja.md                    # NULL, NaN and Inf notes (Japanese)
│   ├── duckdb_LOAD_EXTENSION.md           # Extension implementation guide (English)
│   ├── duckdb_LOAD_EXTENSION-ja.md        # Extension implementation guide (Japanese)
│   ├── statcpp_coverage_analysis-ja.md    # statcpp coverage analysis (Japanese)
│   ├── CHANGELOG.md                       # Changelog (English)
│   ├── CHANGELOG-ja.md                    # Changelog (Japanese)
│   └── ref/                               # Detailed per-category reference (English / Japanese)
├── cmake/
│   ├── DuckDB_C_CPP.cmake                 # Builds DuckDB from source
│   ├── statcpp.cmake                      # Downloads statcpp
│   ├── extension_metadata.cmake           # Appends the LOAD metadata footer
│   └── null.txt                           # One 0x00 byte used by the footer script
└── download/                              # Downloaded dependencies (not tracked)
    ├── DuckDB/
    └── statcpp/
```

## Related Projects

DuckDB-stats is part of the **statcpp family**:

| Use Case | Repository | Description |
| --- | --- | --- |
| C++ library | [statcpp](https://github.com/mitsuruk/statcpp) | C++17 header-only statistics library (386 functions) |
| UNIX CLI | [statcppCLI](https://github.com/mitsuruk/statcppCLI) | Command-line tool for UNIX pipelines |
| SQL (SQLite3) | [sqlite3-stats](https://github.com/mitsuruk/sqlite3-stats) | SQLite3 loadable extension (258 functions) |
| SQL (DuckDB) | **DuckDB-stats** (this repository) | DuckDB loadable extension (259 functions) |

**Compatible statcpp version**: v0.5.0

## Tested Environments

- macOS (Apple Silicon) + Apple Clang 21.0.0, DuckDB v1.5.6 built from source
- The extension also loads into the official duckdb CLI v1.5.6 release for macOS
- Ubuntu 24.04 (arm64, Parallels VM and Docker) + GCC 13.2 / 13.3, DuckDB v1.5.6 built from source; the extension loads into the official
  duckdb CLI v1.5.6 release for Linux
- Windows (MSVC) is not supported

## License

This project is released under the MIT License.

### Dependencies

| Library | License |
| --- | --- |
| [statcpp](https://github.com/mitsuruk/statcpp) | MIT License |
| [DuckDB](https://duckdb.org/) | MIT License |

## Acknowledgments

This project was developed with the help of the following tools and AI assistants:

- **[statcpp](https://github.com/mitsuruk/statcpp)** — C++17 header-only statistics library (386 functions)
- **Claude Code for VS Code** — Suggested corrections to the English grammar and wording in the documentation, the code comments, and the English names of variables, and created the data for the SQL tests.

---

**Disclaimer**: This library does not guarantee numerical stability or edge-case handling at the level of commercial statistical software. For research or production use, cross-validation with established tools such as R or Python/SciPy is recommended.
