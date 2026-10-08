# Statistical Functions for DuckDB

A loadable extension that adds **259 statistical functions** to DuckDB.

## Overview

DuckDB-stats is a DuckDB loadable extension that provides 259 statistical functions callable directly from SQL. It is the DuckDB port of sqlite3-stats and keeps its function names, arguments and results. Built on [statcpp](https://github.com/mitsuruk/statcpp) (a C++17 header-only statistics library with 386 functions), this extension exposes a curated subset of statistical capabilities as native SQL functions.

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

### Build

```bash
cd DuckDB-stats
cmake -B build
cmake --build build
```

### Load the Extension

```sql
-- start the CLI with: duckdb -unsigned   (DuckDB v1.5.6)
LOAD './build/ext_funcs.duckdb_extension';
```

### Usage Examples

```sql
-- Descriptive statistics
SELECT stat_mean(score), stat_stdev(score), stat_median(score)
FROM exam_results;

-- One-sample t-test
SELECT stat_t_test(score, 50) FROM exam_results;

-- Window function: rolling mean
SELECT date, price,
       stat_rolling_mean(price, 7) OVER (ORDER BY date) AS ma7
FROM stock_prices
ORDER BY date;

-- Distribution function
SELECT stat_normal_quantile(0.975, 0, 1);  -- 1.96
```

## Documentation

- [Function Reference](function_reference.md) — All 259 functions overview
- [Load Extension Guide](duckdb_LOAD_EXTENSION.md) — How the extension is built and loaded

## License

MIT License
