# DuckDB Built-in Functions vs DuckDB-stats

<!-- markdownlint-configure-file {
  "MD013": {"tables": false, "code_blocks": false},
  "MD024": {"siblings_only": true}
} -->

← [Function Reference](function_reference.md)

## Scope

- Versions checked: DuckDB v1.5.6, DuckDB-stats (statcpp v0.5.0).
  Re-check Part 1 after upgrading DuckDB.
- Part 1 compares the functions that exist on both sides and lists those that
  exist on only one side. All results were measured on 2026-10-15.
- Part 2 lists DuckDB's built-in statistical functions (no extension needed).
  Function names come from `duckdb_functions()`. General-purpose aggregates
  (`string_agg`, `bit_and`, ...) are left out.
- For the 259 `stat_*` functions, see the [Function Reference](function_reference.md).

Each difference is attributed to one of three layers.

|Cause|Meaning|To change it|
|---|---|---|
|statcpp|How statcpp computes the value|Needs a statcpp fix and a new tag. Other projects that use statcpp, such as sqlite3-stats, behave the same way|
|DuckDB-stats|The extension's wrapper: type conversion, NaN to `NULL`, window placement, API design|Can be changed in this repository alone|
|Built-in|The specification of DuckDB's built-in function|Cannot be changed. Work around it with a cast or another expression|

To load DuckDB-stats (the extension is unsigned, so start the CLI with
`-unsigned` and pass an absolute path):

```sql
-- duckdb -unsigned
LOAD '/path/to/DuckDB-stats/build/ext_funcs.duckdb_extension';
```

---

## Part 1: Comparison

Unless stated otherwise, the examples use this table.

```sql
CREATE TABLE t AS SELECT * FROM (VALUES
  (1, 2.0, 1.0), (2, 4.0, 3.0), (3, 4.0, 2.0), (4, NULL, 6.0),
  (5, 7.0, 5.0), (6, 10.0, 4.0), (7, 1.0, 8.0), (8, 4.0, 7.0)) v(id, x, y);
-- literals such as 2.0 are DECIMAL(3,1)
```

### 1.1 General differences

Keep these in mind when choosing between the two (most important first).
"Cause" names the layer the difference comes from (see the table above).

1. **Return type**: `stat_*` always returns DOUBLE. The built-in `median`,
   `quantile_cont`, `quantile_disc`, `mad`, `mode`, `min`, `max`, `lag` and
   others keep the input type. With DECIMAL input, an interpolated result is
   truncated to the input scale. For example, `median` of 1.0 and 1.1 gives
   `1.0` (`avg` returns DOUBLE and gives 1.05), and `quantile_cont(x, 0.3)`
   gives 3.5 where `stat_percentile(x, 0.3)` gives 3.6. Cast with `x::DOUBLE`
   to get the exact value from the built-in functions.
   **Cause**: Built-in. DuckDB-stats always returns DOUBLE, so it is not affected.
2. **Window frames**: `stat_rolling_*` returns `NULL` for the first n−1 rows,
   where the window is not yet full, and for any window that contains `NULL`.
   The built-in `avg(x) OVER (ROWS n-1 PRECEDING)` computes over the partial
   window and ignores `NULL`. The `stat_*` window functions also require
   `OVER (ORDER BY ...)`; `OVER ()` raises an error.
   **Cause**: DuckDB-stats. statcpp computes only complete windows, and
   DuckDB-stats sets the remaining rows to `NULL` when it places the values.
3. **NaN and Inf**: `stat_*` returns `NULL` when the result is NaN or Inf. The
   built-ins return the value as is or raise an error. For example, on a
   column containing NaN, `avg` gives `nan` and `stat_mean` gives `NULL`;
   `gamma(200)` gives `inf` and `stat_tgamma(200)` gives `NULL`.
   **Cause**: DuckDB-stats. It converts the NaN / Inf returned by statcpp to `NULL`.
4. **Argument order of two-variable functions**: `stat_*` takes `(x, y)`.
   The built-in `corr`, `covar_*` and `regr_*` take `(y, x)`.
   **Cause**: DuckDB-stats. The API is inherited from sqlite3-stats.
5. **Out-of-domain input**: `stat_*` stops with `Invalid Input Error`. For
   example, `stat_percentile(x, 30)` fails with "p must be in [0, 1]". The
   built-in `quantile_cont` accepts `[-1, 1]` and treats a negative value as a
   position counted from the top.
   **Cause**: statcpp. DuckDB-stats turns the exception thrown by statcpp
   into an error.
6. **Random numbers**: `setseed()` affects only `random()`; `stat_*_rand` is not
   reproducible.
   **Cause**: DuckDB-stats. statcpp has `set_seed()`, but no SQL function
   calls it.
7. **Last-digit differences**: Kurtosis and similar values differ by about
   1e-14 (the same value for practical purposes).
   **Cause**: statcpp / Built-in. The two use different algorithms.

### 1.2 Functions on both sides (51 functions)

"Same" means the example data gave the same value, apart from the type
difference above. A "—" cause means there is no difference, or only a
difference in scope.

#### Descriptive statistics (aggregates)

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_mean`|`avg` / `mean`|Same. For NaN, see 1.1 item 3|—|
|`stat_median`|`median`|Same. The built-in truncates DECIMAL input (1.1 item 1)|Built-in|
|`stat_mode`|`mode`|**Differs when several values tie.** `stat_mode` returns the smallest. `mode` returns the first one encountered, so it depends on row order. For [5, 3, 5, 3, 9], `stat_mode` = 3 and `mode` = 5|statcpp|
|`stat_var` / `stat_population_variance`|`var_pop`|Same. `stat_var` is the population variance, unlike the built-in `variance` (sample)|DuckDB-stats (naming)|
|`stat_sample_variance`|`var_samp` / `variance`|Same|—|
|`stat_stdev` / `stat_population_stddev`|`stddev_pop`|Same. `stat_stdev` is the population standard deviation, unlike the built-in `stddev` (sample)|DuckDB-stats (naming)|
|`stat_sample_stddev`|`stddev_samp` / `stddev`|Same|—|
|`stat_skewness`|`skewness`|Same (sample skewness). Example: 0.9168|—|
|`stat_kurtosis`|`kurtosis`|Same (sample excess kurtosis). Example: 0.6074|—|
|`stat_population_kurtosis`|`kurtosis_pop`|Same. Example: −0.4969|—|
|`stat_se`|`sem`|**Different values.** `stat_se` is s/√n (the usual SEM, 1.1518 in the example). `sem` is the population standard deviation / √n (1.0663)|Built-in|
|`stat_mad`|`mad`|Same (raw median absolute deviation, no scale factor)|—|
|`stat_mad_scaled`|`1.4826 * mad(x)`|Same. No dedicated built-in function|—|
|`stat_range`|`max(x) - min(x)`|Same. No dedicated built-in function|—|

#### Quantiles

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_percentile(x, p)`|`quantile_cont(x, p)`|Same interpolation (R type 7). The built-in truncates DECIMAL input (3.6 vs 3.5). For the range of p, see 1.1 item 5|Built-in|
|`stat_quartile`|`quantile_cont(x, [0.25, 0.5, 0.75])`|Same values. `stat_quartile` returns JSON (`{"q1":3,"q2":4,"q3":5.5}`), the built-in returns a LIST|DuckDB-stats|
|`stat_iqr`|`quantile_cont(x, 0.75) - quantile_cont(x, 0.25)`|Same|—|
|`stat_five_number_summary`|`min`, `quantile_cont(x, [0.25, 0.5, 0.75])`, `max`|Same values, returned together as one JSON object|DuckDB-stats|

#### Two variables

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_population_covariance(x, y)`|`covar_pop(y, x)`|Same. Mind the argument order|DuckDB-stats|
|`stat_covariance(x, y)`|`covar_samp(y, x)`|Same|—|
|`stat_pearson_r(x, y)`|`corr(y, x)`|Same|—|
|`stat_simple_regression(x, y)`|`regr_slope`, `regr_intercept`, `regr_r2`|Slope, intercept and R² are the same. `stat_simple_regression` also returns standard errors, t values, p values, the F statistic and adjusted R² as JSON|—|
|`stat_euclidean_dist(a, b)`|`list_distance(list(a), list(b))`|Same. The built-in works on LISTs, so collect the column with `list()` first|—|
|`stat_cosine_sim(a, b)`|`list_cosine_similarity(list(a), list(b))`|Same (as above)|—|
|`stat_cosine_dist(a, b)`|`list_cosine_distance(list(a), list(b))`|Same (as above)|—|

**Similar names, different meaning**: `stat_r_squared(actual, predicted)`
measures the fit of predictions, 1 − SSres/SStot. `regr_r2(y, x)` is the R² of a
simple regression (the squared correlation). On the example data they give
−1.638 and 0.0078.

#### Window functions

These examples use `x` = [3, 1, NULL, 4, 1, 5, NULL] ordered by `id`.

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_rolling_mean(x, n)` / `stat_moving_avg(x, n)`|`avg(x) OVER (ORDER BY id ROWS n-1 PRECEDING)`|Same on rows whose window is full and has no `NULL`. On the other rows `stat_*` returns `NULL` (1.1 item 2)|DuckDB-stats|
|`stat_rolling_sum`|`sum(x) OVER (...)`|As above|DuckDB-stats|
|`stat_rolling_std`|`stddev_samp(x) OVER (...)`|As above (both are sample standard deviations)|DuckDB-stats|
|`stat_rolling_min` / `stat_rolling_max`|`min` / `max(x) OVER (...)`|As above|DuckDB-stats|
|`stat_rank(x)`|`rank() OVER (ORDER BY x)`|**Ties differ.** `stat_rank` gives the average rank (1, 1 → 1.5, 1.5); `rank()` gives the minimum rank (1, 1). A `NULL` row stays `NULL` with `stat_rank` but gets the last rank with `rank()`|statcpp|
|`stat_lag(x, k)`|`lag(x, k) OVER (ORDER BY id)`|Same (type only)|—|
|`stat_diff(x, 1)`|`x - lag(x) OVER (ORDER BY id)`|Same. `stat_diff` also computes higher-order differences|—|
|`stat_fillna_ffill`|`last_value(x IGNORE NULLS) OVER (ORDER BY id)`|Same|—|
|`stat_fillna_bfill`|`first_value(x IGNORE NULLS) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)`|Same|—|
|`stat_fillna_mean`|`coalesce(x, avg(x) OVER ())`|Same|—|
|`stat_fillna_median`|`coalesce(x, median(x::DOUBLE) OVER ())`|Same|—|
|`stat_fillna_interp`|`fill(x) OVER (ORDER BY id)`|**Different basis.** `stat_fillna_interp` interpolates by row position; `fill` interpolates by the value of the ORDER BY key. With id = 1, 2, 10 and x = 1, NULL, 10, the former gives 5.5 and the latter 2.0. A trailing `NULL` stays `NULL` with `stat_*` but is extrapolated by `fill`|statcpp|
|`stat_label_encode`|`dense_rank() OVER (ORDER BY x) - 1`|**Different numbering.** `stat_label_encode` numbers values in order of first appearance (3, 1, 4, 5 → 0, 1, 2, 3); `dense_rank` numbers them in sorted order (1, 3, 4, 5 → 0, 1, 2, 3). `NULL` stays `NULL` with `stat_*` but gets the last number with `dense_rank`|statcpp|
|`stat_bin_freq(x, n)`|`ntile(n) OVER (ORDER BY x)`|**Not equivalent.** `stat_bin_freq` is 0-based and puts equal values in the same bin. `ntile` is 1-based and splits rows by count, so equal values can land in different bins. `ntile` also numbers `NULL` rows|statcpp|
|`stat_bin_width(x, n)`|`equi_width_bins(min, max, n, false)` and `histogram`|Partial only. The built-ins generate boundaries and count values, but do not return a bin number per row|—|

#### Frequencies

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_frequency_count`|`histogram`|Same counts. `stat_*` returns JSON whose keys are not sorted by value; `histogram` returns a MAP sorted by value|statcpp|
|`stat_frequency_table`|computed from `histogram` and `count(*)`|Returns counts and relative frequencies together as JSON|—|

#### Scalars

|`stat_*`|Built-in counterpart|Difference|Cause|
|---|---|---|---|
|`stat_lgamma`|`lgamma`|Same for positive x. **Returns `NULL` for negative x where Γ(x) < 0 (−1 < x < 0, −3 < x < −2, ...).** At x = −0.5, `lgamma` = 1.2655 and `stat_lgamma` = `NULL`. At zero and negative integers the built-in returns `inf` or an error; `stat_*` raises an error|statcpp|
|`stat_tgamma`|`gamma`|Same for positive x. **Returns `NULL` where Γ(x) is negative.** At x = −0.5, `gamma` = −3.5449 and `stat_tgamma` = `NULL`. On overflow the built-in returns `inf`, `stat_*` returns `NULL`|statcpp|
|`stat_log_factorial(n)`|`lgamma(n + 1)`|Same|—|
|`stat_binomial_coef(n, k)`|`factorial(n) / (factorial(k) * factorial(n - k))`|The built-in is exact integer arithmetic but overflows for large n (error at n = 60). `stat_*` approximates in DOUBLE (C(10,3) = 120.00000000000009)|statcpp|
|`stat_uniform_rand()`|`random()`|Both are uniform on [0, 1). `stat_uniform_rand(a, b)` accepts a range. `setseed` affects only `random`|DuckDB-stats|

Note: `stat_sample(x, n)` and `stat_sample_replace(x, n)` correspond to the SQL
`USING SAMPLE` clause rather than to a function. `stat_*` returns the values
as a JSON array; `USING SAMPLE` returns rows.

#### About `stat_lgamma` / `stat_tgamma`

statcpp's `lgamma_impl` uses the reflection formula
`log(π / sin(πx)) − lgamma(1 − x)`. Where sin(πx) is negative, this takes the
log of a negative number and yields NaN, which DuckDB-stats converts to `NULL`.
`tgamma` is computed as `exp(lgamma)`, so it cannot return a negative value.
This is a limitation of statcpp itself (`special_functions.hpp`), not of
DuckDB-stats. statcpp leaves it unfixed and documents it as a known limitation
in its API reference and Doxygen comments. For negative non-integer arguments,
use the built-in `gamma` / `lgamma`.

### 1.3 Built-in only

Functions with no `stat_*` counterpart.

|Category|Functions|
|---|---|
|Sum, product, count|`sum`, `kahan_sum`, `favg`, `product`, `count`, `count_if`, `min`, `max`|
|Information|`entropy`|
|Quantiles|`quantile_disc` (returns an actual value), `approx_quantile`, `reservoir_quantile`|
|Regression helpers|`regr_count`, `regr_avgx`, `regr_avgy`, `regr_sxx`, `regr_syy`, `regr_sxy`|
|Frequencies, approximations|`histogram_exact`, `approx_count_distinct`, `approx_top_k`, `arg_max`, `arg_min`|
|Window|`row_number`, `percent_rank`, `cume_dist`, `lead`, `nth_value`|
|LIST forms|LIST versions such as `list_avg`, `list_median`, `list_stddev_samp`, and `list_aggregate`. They aggregate a LIST stored in one row; `stat_*` accepts only columns (sets of rows)|
|Math|`factorial`, `setseed`|

Built-in aggregates can also be combined with `OVER (ROWS BETWEEN ...)` for any
frame. The `stat_*` window functions have a fixed frame.

### 1.4 DuckDB-stats only

All `stat_*` functions except the 51 above. DuckDB has no built-in tests,
confidence intervals, probability distributions, effect sizes or power
analysis, so use DuckDB-stats for those. See the
[Function Reference](function_reference.md) for details.

|Category|Functions (208)|
|---|---|
|Basic aggregates (7)|`stat_cv`, `stat_geometric_mean`, `stat_geometric_stddev`, `stat_harmonic_mean`, `stat_hodges_lehmann`, `stat_mad_mean`, `stat_population_skewness`|
|Parameterized aggregates (18)|`stat_acf_lag`, `stat_biweight_midvar`, `stat_bootstrap_mean`, `stat_bootstrap_median`, `stat_bootstrap_stddev`, `stat_chisq_gof_uniform`, `stat_ci_mean`, `stat_ci_mean_z`, `stat_ci_var`, `stat_cohens_d`, `stat_hedges_g`, `stat_ks_test`, `stat_moe_mean`, `stat_shapiro_wilk`, `stat_t_test`, `stat_trimmed_mean`, `stat_wilcoxon`, `stat_z_test`|
|Two-column aggregates (20)|`stat_adjusted_r_squared`, `stat_chebyshev_dist`, `stat_chisq_gof`, `stat_kendall_tau`, `stat_mae`, `stat_manhattan_dist`, `stat_mape`, `stat_minkowski_dist`, `stat_mse`, `stat_r_squared`, `stat_rmse`, `stat_spearman_r`, `stat_t_test_paired`, `stat_weighted_covariance`, `stat_weighted_harmonic_mean`, `stat_weighted_mean`, `stat_weighted_median`, `stat_weighted_percentile`, `stat_weighted_stddev`, `stat_weighted_variance`|
|Window (9)|`stat_bh_correction`, `stat_bonferroni`, `stat_ema`, `stat_holm_correction`, `stat_outliers_iqr`, `stat_outliers_mzscore`, `stat_outliers_zscore`, `stat_seasonal_diff`, `stat_winsorize`|
|Complex aggregates (38)|`stat_acf`, `stat_anova1`, `stat_bartlett`, `stat_bonferroni_posthoc`, `stat_bootstrap`, `stat_bootstrap_bca`, `stat_bootstrap_sample`, `stat_chisq_independence`, `stat_ci_mean_diff`, `stat_ci_mean_diff_welch`, `stat_cohens_d2`, `stat_cohens_f`, `stat_contingency_table`, `stat_cumulative_frequency`, `stat_cumulative_relative_frequency`, `stat_dunnett_posthoc`, `stat_f_test`, `stat_glass_delta`, `stat_hedges_g2`, `stat_kaplan_meier`, `stat_kruskal_wallis`, `stat_levene`, `stat_logrank`, `stat_mann_whitney`, `stat_modes`, `stat_nelson_aalen`, `stat_pacf`, `stat_permutation_corr`, `stat_permutation_paired`, `stat_permutation_test2`, `stat_relative_frequency`, `stat_sample`, `stat_sample_replace`, `stat_scheffe_posthoc`, `stat_stratified_sample`, `stat_t_test2`, `stat_t_test_welch`, `stat_tukey_hsd`|
|Scalars: tests and helpers (38)|`stat_aic`, `stat_aicc`, `stat_betainc`, `stat_betaincinv`, `stat_bic`, `stat_bonferroni_scalar`, `stat_boxcox`, `stat_chisq_cdf`, `stat_chisq_pdf`, `stat_chisq_quantile`, `stat_chisq_rand`, `stat_ci_prop`, `stat_ci_prop_diff`, `stat_ci_prop_wilson`, `stat_f_cdf`, `stat_f_pdf`, `stat_f_quantile`, `stat_f_rand`, `stat_fisher_exact`, `stat_gammainc_lower`, `stat_gammainc_lower_inv`, `stat_gammainc_upper`, `stat_nnt`, `stat_norm_cdf`, `stat_norm_quantile`, `stat_normal_cdf`, `stat_normal_pdf`, `stat_normal_quantile`, `stat_normal_rand`, `stat_odds_ratio`, `stat_relative_risk`, `stat_risk_difference`, `stat_t_cdf`, `stat_t_pdf`, `stat_t_quantile`, `stat_t_rand`, `stat_z_test_prop`, `stat_z_test_prop2`|
|Scalars: distributions and conversions (78)|`stat_bernoulli_cdf`, `stat_bernoulli_pmf`, `stat_bernoulli_quantile`, `stat_bernoulli_rand`, `stat_beta_cdf`, `stat_beta_func`, `stat_beta_pdf`, `stat_beta_quantile`, `stat_beta_rand`, `stat_binomial_cdf`, `stat_binomial_pmf`, `stat_binomial_quantile`, `stat_binomial_rand`, `stat_cohens_h`, `stat_d_to_r`, `stat_duniform_cdf`, `stat_duniform_pmf`, `stat_duniform_quantile`, `stat_duniform_rand`, `stat_erf`, `stat_erfc`, `stat_eta_squared_ef`, `stat_exponential_cdf`, `stat_exponential_pdf`, `stat_exponential_quantile`, `stat_exponential_rand`, `stat_gamma_cdf`, `stat_gamma_pdf`, `stat_gamma_quantile`, `stat_gamma_rand`, `stat_geometric_cdf`, `stat_geometric_pmf`, `stat_geometric_quantile`, `stat_geometric_rand`, `stat_hedges_j`, `stat_hypergeom_cdf`, `stat_hypergeom_pmf`, `stat_hypergeom_quantile`, `stat_hypergeom_rand`, `stat_interpret_d`, `stat_interpret_eta2`, `stat_interpret_r`, `stat_lbeta`, `stat_log_binomial_coef`, `stat_logarithmic_mean`, `stat_lognormal_cdf`, `stat_lognormal_pdf`, `stat_lognormal_quantile`, `stat_lognormal_rand`, `stat_moe_prop`, `stat_moe_prop_worst`, `stat_n_moe_mean`, `stat_n_moe_prop`, `stat_n_prop`, `stat_n_t1`, `stat_n_t2`, `stat_nbinom_cdf`, `stat_nbinom_pmf`, `stat_nbinom_quantile`, `stat_nbinom_rand`, `stat_omega_squared_ef`, `stat_partial_eta_sq`, `stat_poisson_cdf`, `stat_poisson_pmf`, `stat_poisson_quantile`, `stat_poisson_rand`, `stat_power_prop`, `stat_power_t1`, `stat_power_t2`, `stat_r_to_d`, `stat_t_to_r`, `stat_uniform_cdf`, `stat_uniform_pdf`, `stat_uniform_quantile`, `stat_weibull_cdf`, `stat_weibull_pdf`, `stat_weibull_quantile`, `stat_weibull_rand`|

---

## Part 2: DuckDB built-in functions

Notes (verified by running them):

- `sem(x)` uses the **population** standard deviation / √n, which is smaller
  than the usual SEM (s / √n). For the usual SEM, use
  `stddev_samp(x) / sqrt(count(x))`.
- `mad(x)` returns the raw MAD, without the normal consistency factor (1.4826).
- The official description of `regr_sxy` says "population covariance", but it
  actually returns the sum of cross-deviations Sxy.
- `entropy(x)` is the base-2 entropy of the value frequencies.

### Location and sums

|Function|Aliases|Description|
|---|---|---|
|`avg(x)`|`mean`|Mean|
|`favg(x)`||Mean with Kahan summation|
|`median(x)`||Median (mean of the two middle values for an even count)|
|`mode(x)`||Mode|
|`sum(x)`||Sum|
|`kahan_sum(x)`|`fsum`, `sumkahan`|Sum with Kahan summation|
|`product(x)`||Product|
|`min(x)` / `max(x)`||Minimum / maximum. `min(x, n)` returns the lowest n as a LIST|
|`count(x)` / `count(*)`||Count|
|`count_if(b)`|`countif`|Number of TRUE values|

### Dispersion

|Function|Aliases|Description|
|---|---|---|
|`var_samp(x)`|`variance`|Sample variance (unbiased, n−1)|
|`var_pop(x)`||Population variance (n)|
|`stddev_samp(x)`|`stddev`|Sample standard deviation|
|`stddev_pop(x)`||Population standard deviation|
|`sem(x)`||Standard error of the mean (population-based, see notes)|
|`mad(x)`||Median absolute deviation (no scale factor)|

### Shape and information

|Function|Description|
|---|---|
|`skewness(x)`|Skewness (bias-corrected)|
|`kurtosis(x)`|Excess kurtosis (Fisher, bias-corrected)|
|`kurtosis_pop(x)`|Excess kurtosis (no bias correction)|
|`entropy(x)`|Base-2 entropy|

### Quantiles

|Function|Aliases|Description|
|---|---|---|
|`quantile_cont(x, p)`||Interpolated quantile. Pass a LIST as p for several at once|
|`quantile_disc(x, p)`|`quantile`|Quantile without interpolation (an actual value)|
|`approx_quantile(x, p)`||Approximate quantile using T-Digest|
|`reservoir_quantile(x, p [, n])`||Approximate quantile using reservoir sampling (default n = 8192)|

### Two variables and simple regression

All take arguments in the order `(y, x)`.

|Function|Description|
|---|---|
|`corr(y, x)`|Pearson correlation coefficient|
|`covar_samp(y, x)`|Sample covariance|
|`covar_pop(y, x)`|Population covariance|
|`regr_slope(y, x)`|Slope of the regression line|
|`regr_intercept(y, x)`|Intercept of the regression line|
|`regr_r2(y, x)`|Coefficient of determination R²|
|`regr_count(y, x)`|Number of pairs where both values are non-NULL|
|`regr_avgx(y, x)` / `regr_avgy(y, x)`|Mean of x / y (non-NULL pairs only)|
|`regr_sxx(y, x)` / `regr_syy(y, x)`|Sums of squared deviations Sxx / Syy|
|`regr_sxy(y, x)`|Sum of cross-deviations Sxy|

### Frequencies and approximate aggregates

|Function|Kind|Description|
|---|---|---|
|`histogram(x [, bins])`|Aggregate|MAP of counts per value (or per bin)|
|`histogram_exact(x, bins)`|Aggregate|Counts of values that exactly match the given bins|
|`equi_width_bins(min, max, n, nice)`|Scalar|Generates equal-width bin boundaries. Use with `histogram`|
|`approx_count_distinct(x)`|Aggregate|Approximate distinct count using HyperLogLog|
|`approx_top_k(x, k)`|Aggregate|Approximate k most frequent values|
|`arg_max(arg, val)` / `arg_min(arg, val)`|Aggregate|arg at the row with the largest / smallest val. Aliases `max_by` / `min_by`|

### Window functions

Use with `OVER (...)`.

|Function|Description|
|---|---|
|`row_number()`|Row number|
|`rank()` / `dense_rank()`|Rank (with / without gaps after ties)|
|`percent_rank()`|Relative rank (rank−1)/(n−1)|
|`cume_dist()`|Cumulative distribution (rows up to the current one)/n|
|`ntile(n)`|Bucket number after splitting into n buckets|
|`lag(x [, k, default])` / `lead(...)`|Value k rows before / after|
|`first_value(x)` / `last_value(x)` / `nth_value(x, n)`|First / last / n-th value in the frame|
|`fill(x)`|Fills `NULL` by linear interpolation over the ORDER BY key (extrapolates at the ends)|

The aggregates above (`avg`, `stddev_samp`, `quantile_cont`, ...) also work as
moving statistics with `OVER (ROWS BETWEEN ...)`.

### LIST forms (scalar)

Apply an aggregate to a LIST in one row. The main ones:

`list_avg`, `list_median`, `list_mode`, `list_var_samp`, `list_var_pop`,
`list_stddev_samp`, `list_stddev_pop`, `list_sem`, `list_mad`, `list_skewness`,
`list_kurtosis`, `list_kurtosis_pop`, `list_entropy`, `list_histogram`,
`list_sum`, `list_product`, `list_min`, `list_max`, `list_count`,
`list_approx_count_distinct`.

Any aggregate can be called through `list_aggregate(list, 'name', args...)`.
Example: `list_aggregate([1, 2, 3], 'quantile_cont', 0.5)` → `2.0`.

### Other math scalars

|Function|Description|
|---|---|
|`gamma(x)` / `lgamma(x)`|Gamma function / log-gamma function|
|`factorial(n)`|Factorial (the `n!` operator also works)|
|`random()` / `setseed(s)`|Uniform random number on [0, 1) / set the seed|

DuckDB has no built-in distribution functions (such as the normal CDF), tests
or confidence intervals. DuckDB-stats provides them (see 1.4).
