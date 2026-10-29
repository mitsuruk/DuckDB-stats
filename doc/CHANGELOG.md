# Changelog

This document records the change history of DuckDB-stats.

This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Initial port of sqlite3-stats to DuckDB** as a loadable extension built with DuckDB's
  C++ extension API (`ext_funcs.duckdb_extension`, DuckDB v1.5.6).
  All 258 functions of sqlite3-stats are provided; the statistical code is carried over
  unchanged and delegates to statcpp v0.5.1, which follows R's default handling of
  missing values.
- The window functions are implemented with DuckDB's custom window callbacks: the result is
  computed once per partition and returned per row. They are called with
  `OVER (ORDER BY ...)` and support `PARTITION BY`.
- **SQL conventions for `NULL`**, the same as sqlite3-stats:
  - aggregates skip `NULL` rows (two-column and group-column functions skip a row in which
    any column is `NULL`), and return `NULL` for no rows or a NaN / Inf result;
  - a scalar function returns `NULL` when any argument is `NULL`; an omitted optional
    argument still takes its default;
  - a `NULL` parameter makes an aggregate return `NULL` and a window function return `NULL`
    on every row, as SQL's `lag(v, NULL)` does.
- **NaN in the data** is a value distinct from `NULL` in DuckDB. It is passed to statcpp,
  which handles it as R handles `NA`: descriptive statistics and correlations give `NULL`,
  tests and models drop the NaN rows, and quantile- and order-based functions raise
  "data contains NaN".
- `doc/nan_notes.md` / `doc/nan_notes-ja.md`: how `NULL`, NaN and Inf are handled, by kind
  of function.
- Google Test suite (451 tests) and the integration runner `a.out` (266 demos). The suite
  includes the tests of sqlite3-stats, value tests for the rolling family, `stat_moving_avg`
  and `stat_lag`, and tests for the `NULL`, NaN and Inf rules above.
- On Linux, DuckDB is linked statically into the extension, as DuckDB's own Linux extensions are,
  because the official duckdb CLI for Linux exports no symbols. The extension loads into the
  official CLI v1.5.6 on Ubuntu 24.04.
- `tests/sample/`: a 100-row sample database and seven queries that can be run as they are.

### Changed (compared with sqlite3-stats)

- **`stat_bonferroni(p, m)` is renamed `stat_bonferroni_scalar(p, m)`.** DuckDB cannot give a
  scalar function the name of the window function `stat_bonferroni(p)`. The function count is
  259.
- Window functions take `OVER (ORDER BY id)` instead of the full-partition frame. `OVER ()`,
  a full-partition frame and a call without `OVER` raise an error instead of returning a
  wrong value.
- An Inf result of a window function is returned as `NULL`, as for the aggregates
  (sqlite3-stats returns Inf).
- Scalar functions registered with a variable argument count are registered with one overload
  per accepted count; a wrong count is a binder error.
- Return types are `DOUBLE`, `BIGINT` and `VARCHAR` (JSON results are `VARCHAR`).
- The `_rand` scalar functions are `VOLATILE`, so DuckDB does not constant-fold them.
- Windows (MSVC) is not supported.

### Fixed (defects of earlier sqlite3-stats and statcpp versions)

- **Rolling family** (`stat_rolling_mean` / `std` / `min` / `max` / `sum`): row *i* held the
  window *starting* at row *i* and the last rows were `NULL`, contrary to the documentation.
  Row *i* now holds the trailing window ending at row *i*, and any window containing a `NULL`
  is `NULL`.
- **`stat_rolling_min` / `stat_rolling_max`** ignored a `NULL` unless it was the first value of
  the window. They now return `NULL` for such a window, like the rest of the family.
- **`stat_moving_avg`**: one `NULL` made every later row `NULL`. It now uses the same trailing
  window and equals `stat_rolling_mean`, except after an Inf: `stat_moving_avg` sums each
  window separately (as R's `stats::filter()`), so only the windows containing the Inf are
  affected.
- **`stat_fillna_median`** filled with the lower of the two middle values (40 instead of 45 for
  an even count); it now fills with the median.
- **`stat_lag`** returned each row's own value and `NULL`ed the last *k* rows. It now returns the
  value *k* rows earlier, matching SQL's `lag()`.
- A `NULL` parameter or scalar argument was read as 0, or replaced by the default. It now
  gives `NULL`.
- Documentation: two examples queried a table without `FROM`, several examples called window
  functions without `OVER`, and nine return types in the function list were wrong
  (`stat_odds_ratio`, `stat_relative_risk`, `stat_risk_difference` return JSON text;
  `stat_binomial_coef` returns a double; the `stat_n_*` sample-size functions return integers).
- Integration runner: demos [123] and [125] queried columns that do not exist and always failed;
  they now use a small categorical table.
- **statcpp v0.5.1** fixes out-of-bounds reads, undefined behaviour and a
  hang. Results for valid arguments are unchanged. Arguments that used to give
  undefined results now raise an SQL error:
  - `stat_bin_width()` with Inf in the data. A finite range too wide for a
    double is now binned correctly;
  - `stat_n_t1()`, `stat_n_t2()`, `stat_n_prop()`, `stat_n_moe_prop()` and
    `stat_n_moe_mean()` when the sample size cannot be represented, for
    example for an effect size or margin of error close to 0;
  - `stat_poisson_quantile()` and `stat_nbinom_quantile()` with an infinite
    parameter.

  `stat_duniform_pmf()`, `stat_duniform_cdf()` and `stat_duniform_quantile()`
  now give correct results when `b - a` overflows a 64-bit integer, and
  `stat_beta_rand()` no longer hangs for extremely small shape parameters.
- **Integer results above INT64_MAX turned negative**: `stat_n_t1()`,
  `stat_n_t2()`, `stat_n_prop()`, `stat_n_moe_prop()`, `stat_n_moe_mean()`,
  `stat_poisson_quantile()`, `stat_geometric_quantile()` and
  `stat_nbinom_quantile()` cast statcpp's unsigned result straight to a signed
  64-bit integer, so a result above 9223372036854775807 came back negative
  (for example `stat_n_moe_mean(1.0, 1.767e9, 0.95)`). Such a result now raises
  an SQL error.
