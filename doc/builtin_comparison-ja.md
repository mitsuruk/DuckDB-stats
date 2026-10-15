# DuckDB 組み込み関数と DuckDB-stats の比較

<!-- markdownlint-configure-file {
  "MD013": {"tables": false, "code_blocks": false},
  "MD024": {"siblings_only": true}
} -->

← [関数リファレンス](function_reference-ja.md) に戻る

## 略語一覧

|略語|フルスペル|
|---|---|
|CDF|Cumulative Distribution Function(累積分布関数)|
|MAD|Median Absolute Deviation(中央絶対偏差)|
|SEM|Standard Error of the Mean(平均の標準誤差)|
|JSON|JavaScript Object Notation|

## 前提

- 検証した版: DuckDB v1.5.6, DuckDB-stats(statcpp v0.5.0). DuckDB を更新したら,第1部の差を確認し直す.
- 第1部は,両方にある機能の動作の差と,片方にしか無い機能の一覧. 2026-10-15 に実測した.
- 第2部は, DuckDB 本体の統計関連の組み込み関数(拡張不要)の一覧. 関数名は `duckdb_functions()` から取得した.
  汎用の集約関数(`string_agg`, `bit_and` 等)は除外した.
- `stat_*` 259 関数の一覧と詳細は [関数リファレンス](function_reference-ja.md) を参照.

差の原因は次の3つの層に分けて示す.

|原因|意味|変えるには|
|---|---|---|
|statcpp|statcpp の計算方法・仕様による|statcpp の修正と新しいタグが必要. sqlite3-stats など statcpp を使う他のプロジェクトも同じ挙動になる|
|DuckDB-stats|拡張のラッパー(型の変換, NaN の NULL 化, 窓の割り当て, API 設計)による|このリポジトリだけで変えられる|
|組み込み|DuckDB 本体の組み込み関数の仕様による|変えられない. キャストや別の式で回避する|

DuckDB-stats のロード方法(拡張は未署名なので `-unsigned` で起動し,絶対パスで LOAD する):

```sql
-- duckdb -unsigned
LOAD '/path/to/DuckDB-stats/build/ext_funcs.duckdb_extension';
```

---

## 第1部: 組み込み関数と DuckDB-stats の比較

DuckDB v1.5.6 の CLI に DuckDB-stats を LOAD し, 同じデータで両者を実行して比べた(2026-10-15).
表中の例は,特に断らない限り次のデータを使っている.

```sql
CREATE TABLE t AS SELECT * FROM (VALUES
  (1, 2.0, 1.0), (2, 4.0, 3.0), (3, 4.0, 2.0), (4, NULL, 6.0),
  (5, 7.0, 5.0), (6, 10.0, 4.0), (7, 1.0, 8.0), (8, 4.0, 7.0)) v(id, x, y);
-- 2.0 のようなリテラルは DECIMAL(3,1) 型になる点に注意
```

### 1.1 共通する動作の差

両方にある機能を使い分けるときは,次の差に注意する(重要度の高い順).
各項目の「原因」は,差がどの層から来るかを示す(前提の表を参照).

1. **戻り値の型**: `stat_*` は常に DOUBLE を返す. 組み込みの `median`, `quantile_cont`, `quantile_disc`,
   `mad`, `mode`, `min`, `max`, `lag` 等は入力の型を保つ.
   入力が DECIMAL だと,補間した結果が入力の小数桁に切り詰められる.
   例: `median` に 1.0 と 1.1 を渡すと `1.0` になる(`avg` は DOUBLE なので 1.05).
   `quantile_cont(x, 0.3)` は 3.5 になるが, `stat_percentile(x, 0.3)` は 3.6 を返す.
   組み込み関数で正しい値を得るには `x::DOUBLE` にキャストする.
   **原因**: 組み込み. DuckDB-stats は常に DOUBLE を返すので起きない.
2. **ウィンドウ関数の窓**: `stat_rolling_*` は,窓が埋まらない先頭 n−1 行と, NULL を含む窓では NULL を返す.
   組み込みの `avg(x) OVER (ROWS n-1 PRECEDING)` は,途中までの窓で計算し, NULL を無視する.
   また `stat_*` のウィンドウ関数は `OVER (ORDER BY ...)` が必須で, `OVER ()` はエラーになる.
   **原因**: DuckDB-stats. statcpp は完全な窓の値だけを計算し,
   DuckDB-stats がそれを各行に割り当てるときに残りの行を NULL にする.
3. **NaN と Inf**: `stat_*` は結果が NaN や Inf になると NULL を返す.
   組み込みはそのまま返すかエラーにする.
   例: NaN を含む列に対して `avg` は `nan`, `stat_mean` は NULL.
   `gamma(200)` は `inf`, `stat_tgamma(200)` は NULL.
   **原因**: DuckDB-stats. statcpp が返した NaN / Inf を NULL に変換している.
4. **2変数関数の引数の順序**: `stat_*` は `(x, y)`.
   組み込みの `corr`, `covar_*`, `regr_*` は `(y, x)`.
   **原因**: DuckDB-stats. sqlite3-stats から引き継いだ API である.
5. **定義域外の入力**: `stat_*` は `Invalid Input Error` で止まる.
   例: `stat_percentile(x, 30)` は "p must be in [0, 1]" になる.
   組み込みの `quantile_cont` は `[-1, 1]` を受け付け,負の値は上側からの位置として扱う.
   **原因**: statcpp. statcpp が投げた例外を DuckDB-stats がエラーに変換している.
6. **乱数**: `setseed()` は `random()` にしか効かず, `stat_*_rand` は再現できない.
   **原因**: DuckDB-stats. statcpp には `set_seed()` があるが, SQL から呼ぶ関数を用意していない.
7. **浮動小数の末尾の差**: 尖度などで 1e-14 程度の差がある(実用上は同じ値).
   **原因**: statcpp / 組み込み. 両者の計算アルゴリズムが異なる.

### 1.2 両方にある機能(51 関数)

「同じ」は,上記の型の差を除き, 例のデータで同じ値になったことを示す.
「原因」の — は,差が無いか,機能の範囲の違いだけであることを示す.

#### 記述統計(集約)

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_mean`|`avg` / `mean`|同じ. NaN の扱いは 1.1 の 3|—|
|`stat_median`|`median`|同じ. DECIMAL 入力では組み込みが丸める(1.1 の 1)|組み込み|
|`stat_mode`|`mode`|**同数の最頻値が複数あると異なる**. `stat_mode` は最小値を返す. `mode` は最初に出現した値を返すので,行の順序に依存する. 例: [5, 3, 5, 3, 9] で `stat_mode`=3, `mode`=5|statcpp|
|`stat_var` / `stat_population_variance`|`var_pop`|同じ. `stat_var` は母分散なので,組み込みの `variance`(標本分散)とは異なる|DuckDB-stats(命名)|
|`stat_sample_variance`|`var_samp` / `variance`|同じ|—|
|`stat_stdev` / `stat_population_stddev`|`stddev_pop`|同じ. `stat_stdev` は母標準偏差なので,組み込みの `stddev`(標本)とは異なる|DuckDB-stats(命名)|
|`stat_sample_stddev`|`stddev_samp` / `stddev`|同じ|—|
|`stat_skewness`|`skewness`|同じ(標本歪度). 例: 0.9168|—|
|`stat_kurtosis`|`kurtosis`|同じ(標本超過尖度). 例: 0.6074|—|
|`stat_population_kurtosis`|`kurtosis_pop`|同じ. 例: −0.4969|—|
|`stat_se`|`sem`|**値が異なる**. `stat_se` は s/√n(標本 SEM, 例 1.1518). `sem` は母標準偏差/√n(例 1.0663). 一般に使われるのは `stat_se`|組み込み|
|`stat_mad`|`mad`|同じ(係数なしの中央絶対偏差)|—|
|`stat_mad_scaled`|`1.4826 * mad(x)`|同じ. 組み込みには専用関数が無い|—|
|`stat_range`|`max(x) - min(x)`|同じ. 組み込みには専用関数が無い|—|

#### 分位点

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_percentile(x, p)`|`quantile_cont(x, p)`|補間方法は同じ(R の type 7). DECIMAL 入力では組み込みが丸める(例 3.6 と 3.5). p の範囲は 1.1 の 5|組み込み|
|`stat_quartile`|`quantile_cont(x, [0.25, 0.5, 0.75])`|値は同じ. `stat_quartile` は JSON(`{"q1":3,"q2":4,"q3":5.5}`), 組み込みは LIST を返す|DuckDB-stats|
|`stat_iqr`|`quantile_cont(x, 0.75) - quantile_cont(x, 0.25)`|同じ|—|
|`stat_five_number_summary`|`min`, `quantile_cont(x, [0.25, 0.5, 0.75])`, `max`|値は同じ. JSON で1つにまとめて返す点が異なる|DuckDB-stats|

#### 2変数

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_population_covariance(x, y)`|`covar_pop(y, x)`|同じ. 引数の順序に注意|DuckDB-stats|
|`stat_covariance(x, y)`|`covar_samp(y, x)`|同じ|—|
|`stat_pearson_r(x, y)`|`corr(y, x)`|同じ|—|
|`stat_simple_regression(x, y)`|`regr_slope`, `regr_intercept`, `regr_r2`|傾き,切片, R² は同じ. `stat_simple_regression` はこのほかに標準誤差, t 値, p 値, F 値, 自由度調整済み R² を JSON で返す|—|
|`stat_euclidean_dist(a, b)`|`list_distance(list(a), list(b))`|同じ. 組み込みは LIST 同士の関数なので, `list()` で集めてから渡す|—|
|`stat_cosine_sim(a, b)`|`list_cosine_similarity(list(a), list(b))`|同じ(同上)|—|
|`stat_cosine_dist(a, b)`|`list_cosine_distance(list(a), list(b))`|同じ(同上)|—|

**名前は似ているが別物**: `stat_r_squared(actual, predicted)` は予測値の当てはまり 1 − SSres/SStot を計算する.
`regr_r2(y, x)` は単回帰の R²(相関係数の2乗)である. 例のデータでは −1.638 と 0.0078 になった.

#### ウィンドウ関数

次の例は `x` = [3, 1, NULL, 4, 1, 5, NULL] を `id` 順に並べたもの.

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_rolling_mean(x, n)` / `stat_moving_avg(x, n)`|`avg(x) OVER (ORDER BY id ROWS n-1 PRECEDING)`|窓が埋まっていて NULL も無い行では同じ. それ以外の行では `stat_*` が NULL を返す(1.1 の 2)|DuckDB-stats|
|`stat_rolling_sum`|`sum(x) OVER (...)`|同上|DuckDB-stats|
|`stat_rolling_std`|`stddev_samp(x) OVER (...)`|同上(どちらも標本標準偏差)|DuckDB-stats|
|`stat_rolling_min` / `stat_rolling_max`|`min` / `max(x) OVER (...)`|同上|DuckDB-stats|
|`stat_rank(x)`|`rank() OVER (ORDER BY x)`|**同順位の扱いが異なる**. `stat_rank` は平均順位(1, 1 → 1.5, 1.5), `rank()` は最小順位(1, 1). NULL の行は `stat_rank` では NULL, `rank()` では末尾の順位になる|statcpp|
|`stat_lag(x, k)`|`lag(x, k) OVER (ORDER BY id)`|同じ(型の差のみ)|—|
|`stat_diff(x, 1)`|`x - lag(x) OVER (ORDER BY id)`|同じ. `stat_diff` は2次以上の差分も計算できる|—|
|`stat_fillna_ffill`|`last_value(x IGNORE NULLS) OVER (ORDER BY id)`|同じ|—|
|`stat_fillna_bfill`|`first_value(x IGNORE NULLS) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)`|同じ|—|
|`stat_fillna_mean`|`coalesce(x, avg(x) OVER ())`|同じ|—|
|`stat_fillna_median`|`coalesce(x, median(x::DOUBLE) OVER ())`|同じ|—|
|`stat_fillna_interp`|`fill(x) OVER (ORDER BY id)`|**補間の基準が異なる**. `stat_fillna_interp` は行の位置で補間し, `fill` は ORDER BY のキーの値で補間する. 例: id = 1, 2, 10 で x = 1, NULL, 10 のとき, 前者は 5.5, 後者は 2.0. また末尾の NULL は, `stat_*` では NULL のまま残り, `fill` では外挿で埋まる|statcpp|
|`stat_label_encode`|`dense_rank() OVER (ORDER BY x) - 1`|**番号の付け方が異なる**. `stat_label_encode` は出現順(3, 1, 4, 5 → 0, 1, 2, 3), `dense_rank` は値の順(1, 3, 4, 5 → 0, 1, 2, 3). NULL は `stat_*` では NULL, `dense_rank` では最後の番号になる|statcpp|
|`stat_bin_freq(x, n)`|`ntile(n) OVER (ORDER BY x)`|**一致しない**. `stat_bin_freq` は 0 始まりで,同じ値は同じビンに入る. `ntile` は 1 始まりで,同じ値でも行数で機械的に分けるため別のビンになりうる. NULL も `ntile` では番号が付く|statcpp|
|`stat_bin_width(x, n)`|`equi_width_bins(min, max, n, false)` と `histogram`|部分的にしか対応しない. 組み込みは境界の生成と度数の集計だけで,行ごとのビン番号は返さない|—|

#### 度数

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_frequency_count`|`histogram`|度数は同じ. `stat_*` は JSON で返し,キーの順序は値の順にならない. `histogram` は値の順に並んだ MAP を返す|statcpp|
|`stat_frequency_table`|`histogram` と `count(*)` から計算|度数と相対度数をまとめて JSON で返す|—|

#### スカラー

|`stat_*`|組み込みの対応|差|原因|
|---|---|---|---|
|`stat_lgamma`|`lgamma`|正の x では同じ. **x が負で Γ(x) < 0 となる区間(−1 < x < 0, −3 < x < −2, ...)では `stat_lgamma` が NULL を返す**. 例: x = −0.5 で `lgamma` = 1.2655, `stat_lgamma` = NULL. 0 と負の整数では,組み込みは `inf` かエラー, `stat_*` はエラー|statcpp|
|`stat_tgamma`|`gamma`|正の x では同じ. **Γ(x) が負になる x では `stat_tgamma` が NULL を返す**. 例: x = −0.5 で `gamma` = −3.5449, `stat_tgamma` = NULL. オーバーフローすると組み込みは `inf`, `stat_*` は NULL|statcpp|
|`stat_log_factorial(n)`|`lgamma(n + 1)`|同じ|—|
|`stat_binomial_coef(n, k)`|`factorial(n) / (factorial(k) * factorial(n - k))`|組み込みは整数で正確に計算するが, n が大きいとオーバーフローする(n = 60 でエラー). `stat_*` は DOUBLE で近似する(例: C(10,3) = 120.00000000000009)|statcpp|
|`stat_uniform_rand()`|`random()`|どちらも [0, 1) の一様乱数. `stat_uniform_rand(a, b)` は範囲を指定できる. `setseed` が効くのは `random` だけ|DuckDB-stats|

参考: `stat_sample(x, n)` と `stat_sample_replace(x, n)` に対応するのは,
関数ではなく SQL の `USING SAMPLE` 句である.
`stat_*` は値を JSON 配列で返し, `USING SAMPLE` は行を返す.

#### `stat_lgamma` / `stat_tgamma` の不具合について

statcpp の `lgamma_impl` は,反射公式 `log(π / sin(πx)) − lgamma(1 − x)` を使っている.
sin(πx) が負になる区間では負の数の log を取ることになり, NaN になる.
その NaN を DuckDB-stats が NULL に変換している.
`tgamma` は `exp(lgamma)` で計算しているので,負の値を返せない.
これは statcpp 本体の不具合(`special_functions.hpp`)で, DuckDB-stats 側の不具合ではない.
statcpp では修正せず, 既知の制限として API リファレンスと Doxygen コメントに記載している.
負の非整数で使う場合は,組み込みの `gamma` / `lgamma` を使う.

### 1.3 組み込みにしか無い機能

対応する `stat_*` が無いもの.

|分類|関数|
|---|---|
|合計・積・件数|`sum`, `kahan_sum`, `favg`, `product`, `count`, `count_if`, `min`, `max`|
|情報量|`entropy`|
|分位点|`quantile_disc`(実在する値を返す), `approx_quantile`, `reservoir_quantile`|
|回帰の補助|`regr_count`, `regr_avgx`, `regr_avgy`, `regr_sxx`, `regr_syy`, `regr_sxy`|
|度数・近似|`histogram_exact`, `approx_count_distinct`, `approx_top_k`, `arg_max`, `arg_min`|
|ウィンドウ|`row_number`, `percent_rank`, `cume_dist`, `lead`, `nth_value`|
|LIST 版|`list_avg`, `list_median`, `list_stddev_samp` などの LIST 版と `list_aggregate`. 1 行に入った LIST 列を集約できる. `stat_*` は列(行の集まり)しか受け付けない|
|数学|`factorial`, `setseed`|

組み込みの集約関数は `OVER (ROWS BETWEEN ...)` と組み合わせて自由な窓で使えることも利点である.
`stat_*` のウィンドウ関数は窓の形が固定されている.

### 1.4 DuckDB-stats にしか無い機能

上記の 51 関数を除いた残りがこれにあたる. 組み込みには検定,信頼区間,確率分布,効果量,検出力分析が一切無いので,
それらが必要な場合は DuckDB-stats を使う. 各関数の詳細は [関数リファレンス](function_reference-ja.md) を参照.

|分類|関数(208)|
|---|---|
|基本集約(7)|`stat_cv`, `stat_geometric_mean`, `stat_geometric_stddev`, `stat_harmonic_mean`, `stat_hodges_lehmann`, `stat_mad_mean`, `stat_population_skewness`|
|パラメータ付き集約(18)|`stat_acf_lag`, `stat_biweight_midvar`, `stat_bootstrap_mean`, `stat_bootstrap_median`, `stat_bootstrap_stddev`, `stat_chisq_gof_uniform`, `stat_ci_mean`, `stat_ci_mean_z`, `stat_ci_var`, `stat_cohens_d`, `stat_hedges_g`, `stat_ks_test`, `stat_moe_mean`, `stat_shapiro_wilk`, `stat_t_test`, `stat_trimmed_mean`, `stat_wilcoxon`, `stat_z_test`|
|2カラム集約(20)|`stat_adjusted_r_squared`, `stat_chebyshev_dist`, `stat_chisq_gof`, `stat_kendall_tau`, `stat_mae`, `stat_manhattan_dist`, `stat_mape`, `stat_minkowski_dist`, `stat_mse`, `stat_r_squared`, `stat_rmse`, `stat_spearman_r`, `stat_t_test_paired`, `stat_weighted_covariance`, `stat_weighted_harmonic_mean`, `stat_weighted_mean`, `stat_weighted_median`, `stat_weighted_percentile`, `stat_weighted_stddev`, `stat_weighted_variance`|
|ウィンドウ(9)|`stat_bh_correction`, `stat_bonferroni`, `stat_ema`, `stat_holm_correction`, `stat_outliers_iqr`, `stat_outliers_mzscore`, `stat_outliers_zscore`, `stat_seasonal_diff`, `stat_winsorize`|
|複合集約(38)|`stat_acf`, `stat_anova1`, `stat_bartlett`, `stat_bonferroni_posthoc`, `stat_bootstrap`, `stat_bootstrap_bca`, `stat_bootstrap_sample`, `stat_chisq_independence`, `stat_ci_mean_diff`, `stat_ci_mean_diff_welch`, `stat_cohens_d2`, `stat_cohens_f`, `stat_contingency_table`, `stat_cumulative_frequency`, `stat_cumulative_relative_frequency`, `stat_dunnett_posthoc`, `stat_f_test`, `stat_glass_delta`, `stat_hedges_g2`, `stat_kaplan_meier`, `stat_kruskal_wallis`, `stat_levene`, `stat_logrank`, `stat_mann_whitney`, `stat_modes`, `stat_nelson_aalen`, `stat_pacf`, `stat_permutation_corr`, `stat_permutation_paired`, `stat_permutation_test2`, `stat_relative_frequency`, `stat_sample`, `stat_sample_replace`, `stat_scheffe_posthoc`, `stat_stratified_sample`, `stat_t_test2`, `stat_t_test_welch`, `stat_tukey_hsd`|
|スカラー(検定補助)(38)|`stat_aic`, `stat_aicc`, `stat_betainc`, `stat_betaincinv`, `stat_bic`, `stat_bonferroni_scalar`, `stat_boxcox`, `stat_chisq_cdf`, `stat_chisq_pdf`, `stat_chisq_quantile`, `stat_chisq_rand`, `stat_ci_prop`, `stat_ci_prop_diff`, `stat_ci_prop_wilson`, `stat_f_cdf`, `stat_f_pdf`, `stat_f_quantile`, `stat_f_rand`, `stat_fisher_exact`, `stat_gammainc_lower`, `stat_gammainc_lower_inv`, `stat_gammainc_upper`, `stat_nnt`, `stat_norm_cdf`, `stat_norm_quantile`, `stat_normal_cdf`, `stat_normal_pdf`, `stat_normal_quantile`, `stat_normal_rand`, `stat_odds_ratio`, `stat_relative_risk`, `stat_risk_difference`, `stat_t_cdf`, `stat_t_pdf`, `stat_t_quantile`, `stat_t_rand`, `stat_z_test_prop`, `stat_z_test_prop2`|
|スカラー(分布・変換)(78)|`stat_bernoulli_cdf`, `stat_bernoulli_pmf`, `stat_bernoulli_quantile`, `stat_bernoulli_rand`, `stat_beta_cdf`, `stat_beta_func`, `stat_beta_pdf`, `stat_beta_quantile`, `stat_beta_rand`, `stat_binomial_cdf`, `stat_binomial_pmf`, `stat_binomial_quantile`, `stat_binomial_rand`, `stat_cohens_h`, `stat_d_to_r`, `stat_duniform_cdf`, `stat_duniform_pmf`, `stat_duniform_quantile`, `stat_duniform_rand`, `stat_erf`, `stat_erfc`, `stat_eta_squared_ef`, `stat_exponential_cdf`, `stat_exponential_pdf`, `stat_exponential_quantile`, `stat_exponential_rand`, `stat_gamma_cdf`, `stat_gamma_pdf`, `stat_gamma_quantile`, `stat_gamma_rand`, `stat_geometric_cdf`, `stat_geometric_pmf`, `stat_geometric_quantile`, `stat_geometric_rand`, `stat_hedges_j`, `stat_hypergeom_cdf`, `stat_hypergeom_pmf`, `stat_hypergeom_quantile`, `stat_hypergeom_rand`, `stat_interpret_d`, `stat_interpret_eta2`, `stat_interpret_r`, `stat_lbeta`, `stat_log_binomial_coef`, `stat_logarithmic_mean`, `stat_lognormal_cdf`, `stat_lognormal_pdf`, `stat_lognormal_quantile`, `stat_lognormal_rand`, `stat_moe_prop`, `stat_moe_prop_worst`, `stat_n_moe_mean`, `stat_n_moe_prop`, `stat_n_prop`, `stat_n_t1`, `stat_n_t2`, `stat_nbinom_cdf`, `stat_nbinom_pmf`, `stat_nbinom_quantile`, `stat_nbinom_rand`, `stat_omega_squared_ef`, `stat_partial_eta_sq`, `stat_poisson_cdf`, `stat_poisson_pmf`, `stat_poisson_quantile`, `stat_poisson_rand`, `stat_power_prop`, `stat_power_t1`, `stat_power_t2`, `stat_r_to_d`, `stat_t_to_r`, `stat_uniform_cdf`, `stat_uniform_pdf`, `stat_uniform_quantile`, `stat_weibull_cdf`, `stat_weibull_pdf`, `stat_weibull_quantile`, `stat_weibull_rand`|

---

## 第2部: DuckDB 組み込み関数一覧

注意点(実機で確認済み):

- `sem(x)` は**母標準偏差** / √n で計算する. 一般的な標本 SEM(s / √n)より小さい値になる.
  標本 SEM が必要なら `stddev_samp(x) / sqrt(count(x))` を使う.
- `mad(x)` は正規分布の一致係数(1.4826)を掛けない生の MAD を返す.
- `regr_sxy` の公式説明は "population covariance" だが,実際は偏差積和 Sxy を返す.
- `entropy(x)` は値の出現頻度に対する底2のエントロピー.

### 代表値・合計

|関数|別名|説明|
|---|---|---|
|`avg(x)`|`mean`|平均|
|`favg(x)`||Kahan 加算による高精度平均|
|`median(x)`||中央値(偶数個は中央2値の平均)|
|`mode(x)`||最頻値|
|`sum(x)`||合計|
|`kahan_sum(x)`|`fsum`, `sumkahan`|Kahan 加算による高精度合計|
|`product(x)`||積|
|`min(x)` / `max(x)`||最小 / 最大. `min(x, n)` で下位 n 件の LIST|
|`count(x)` / `count(*)`||件数|
|`count_if(b)`|`countif`|TRUE の件数|

### 散布度

|関数|別名|説明|
|---|---|---|
|`var_samp(x)`|`variance`|標本分散(不偏, n−1)|
|`var_pop(x)`||母分散(n)|
|`stddev_samp(x)`|`stddev`|標本標準偏差|
|`stddev_pop(x)`||母標準偏差|
|`sem(x)`||平均の標準誤差(母標準偏差ベース, 上記注意)|
|`mad(x)`||中央絶対偏差(係数なし)|

### 分布の形状・情報量

|関数|説明|
|---|---|
|`skewness(x)`|歪度(バイアス補正あり)|
|`kurtosis(x)`|超過尖度(Fisher, バイアス補正あり)|
|`kurtosis_pop(x)`|超過尖度(バイアス補正なし)|
|`entropy(x)`|底2のエントロピー|

### 分位点

|関数|別名|説明|
|---|---|---|
|`quantile_cont(x, p)`||補間ありの分位点. p に LIST を渡すと複数同時|
|`quantile_disc(x, p)`|`quantile`|補間なし(実在値)の分位点|
|`approx_quantile(x, p)`||T-Digest による近似分位点|
|`reservoir_quantile(x, p [, n])`||リザーバサンプリングによる近似分位点(既定 n=8192)|

### 2変数・単回帰

すべて引数の順序は `(y, x)`.

|関数|説明|
|---|---|
|`corr(y, x)`|Pearson 相関係数|
|`covar_samp(y, x)`|標本共分散|
|`covar_pop(y, x)`|母共分散|
|`regr_slope(y, x)`|回帰直線の傾き|
|`regr_intercept(y, x)`|回帰直線の切片|
|`regr_r2(y, x)`|決定係数 R²|
|`regr_count(y, x)`|両方非 NULL の組の数|
|`regr_avgx(y, x)` / `regr_avgy(y, x)`|x / y の平均(両方非 NULL の組のみ)|
|`regr_sxx(y, x)` / `regr_syy(y, x)`|偏差平方和 Sxx / Syy|
|`regr_sxy(y, x)`|偏差積和 Sxy|

### 度数・近似集計

|関数|種別|説明|
|---|---|---|
|`histogram(x [, bins])`|集約|値ごと(またはビンごと)の度数 MAP|
|`histogram_exact(x, bins)`|集約|指定ビンに完全一致する値の度数|
|`equi_width_bins(min, max, n, nice)`|スカラー|等幅ビンの境界を生成. `histogram` と併用|
|`approx_count_distinct(x)`|集約|HyperLogLog による異なり数の近似|
|`approx_top_k(x, k)`|集約|出現頻度上位 k 件の近似|
|`arg_max(arg, val)` / `arg_min(arg, val)`|集約|val が最大 / 最小の行の arg. 別名 `max_by` / `min_by`|

### ウィンドウ関数

`OVER (...)` 付きで使う.

|関数|説明|
|---|---|
|`row_number()`|行番号|
|`rank()` / `dense_rank()`|順位(同順位で飛ばす / 飛ばさない)|
|`percent_rank()`|相対順位 (rank−1)/(n−1)|
|`cume_dist()`|累積分布 (自分以下の行数)/n|
|`ntile(n)`|n 個のバケットに分割した番号|
|`lag(x [, k, default])` / `lead(...)`|k 行前 / 後の値|
|`first_value(x)` / `last_value(x)` / `nth_value(x, n)`|フレーム内の先頭 / 末尾 / n 番目|
|`fill(x)`|NULL を ORDER BY キーに対する線形補間(端は外挿)で埋める|

上記の集約関数(`avg`, `stddev_samp`, `quantile_cont` 等)も `OVER (ROWS BETWEEN ...)` で移動統計量として使える.

### LIST 版(スカラー)

1行の LIST 列に対して集約を適用する. 主なもの:

`list_avg`, `list_median`, `list_mode`, `list_var_samp`, `list_var_pop`,
`list_stddev_samp`, `list_stddev_pop`, `list_sem`, `list_mad`, `list_skewness`,
`list_kurtosis`, `list_kurtosis_pop`, `list_entropy`, `list_histogram`,
`list_sum`, `list_product`, `list_min`, `list_max`, `list_count`,
`list_approx_count_distinct`.

任意の集約関数は `list_aggregate(list, 'name', args...)` で呼べる.
例: `list_aggregate([1, 2, 3], 'quantile_cont', 0.5)` → `2.0`.

### その他の数学スカラー

|関数|説明|
|---|---|
|`gamma(x)` / `lgamma(x)`|ガンマ関数 / 対数ガンマ関数|
|`factorial(n)`|階乗(`n!` 演算子も可)|
|`random()` / `setseed(s)`|[0, 1) 一様乱数 / シード設定|

組み込みには分布関数(正規 CDF 等), 検定, 信頼区間は無い. それらは DuckDB-stats で補う(1.4 を参照).
