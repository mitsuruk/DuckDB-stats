# ウィンドウ関数（26関数）

全行スキャン型のウィンドウ関数。各行に対して1つの値を返す。`GROUP BY` は使用しない。

← [関数リファレンス](../function_reference-ja.md) に戻る

## 呼び出し方

本カテゴリの関数はすべて、**パーティション全体**に対して結果を計算し、現在行に対応する
要素を返す。`OVER` 句に `ORDER BY` を書いて呼び出す。`PARTITION BY` は通常どおり使える。

```sql
SELECT id, val,
       stat_rank(val) OVER (ORDER BY id) AS r
FROM ts_data
ORDER BY id;

SELECT grp, id, val,
       stat_fillna_mean(val) OVER (PARTITION BY grp ORDER BY id) AS filled
FROM grouped_ts
ORDER BY grp, id;
```

- `ORDER BY` は関数が見る行の順序(時系列関数では時間順)を決める。キーは**一意**にすること。
  現在行はフレームの最終行として特定されるため、既定のフレームではキーが同値の行は
  同値グループの最終行の結果を共有する。
- `OVER ()`、全行フレーム(`ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING`)、
  および `OVER` なしの呼び出しはエラーになる。DuckDB はこれらをパーティションごとに
  1 度だけ評価するため、行ごとの結果を返せない。

- パラメータに `NULL` を渡すと、SQL の `lag(v, NULL)` と同じく全行 `NULL` になる。
  `NULL`・`NaN`・`Inf` が関数の種類ごとにどう影響するかは
  [NULL・NaN・Inf の注意事項](../nan_notes-ja.md) の 5 章 を参照。

> **sqlite3-stats との違い**: SQLite 版では上記の全行フレームの指定が必須だった。
> DuckDB 版では `OVER (ORDER BY id)` と書く。

---

## ローリング統計量

> **注意**: 本カテゴリの関数はすべて**ウィンドウ関数**（全行スキャン型）として実装されている。各行に対して1つの値を返す。`GROUP BY` は使用しない。

### stat_rolling_mean

**ローリング平均**（移動平均）を計算する。指定したウィンドウサイズの直近 n 件の平均値を各行に返す。

**構文**: `stat_rolling_mean(column, window_size)`

| パラメータ | 説明 |
|---|---|
| `window_size` | ウィンドウサイズ（整数） |

> 行 *i* は行 *i*-window_size+1 〜 *i* (現在行で終わる窓) を対象とする。ウィンドウ未満の先頭行は
> `NULL` を返し、窓に `NULL` を含む行も `NULL` を返す。ローリング統計量と `stat_moving_avg` に共通。

```sql
-- 3日間のローリング平均
SELECT date, price,
       stat_rolling_mean(price, 3) OVER (ORDER BY date) AS rolling_avg
FROM stock_prices
ORDER BY date;

-- 7日間移動平均で平滑化
SELECT timestamp, value,
       stat_rolling_mean(value, 7) OVER (ORDER BY timestamp) AS smoothed
FROM sensor_data
ORDER BY timestamp;
```

---

### stat_rolling_std

**ローリング標準偏差**を計算する。

**構文**: `stat_rolling_std(column, window_size)`

```sql
SELECT date, price,
       stat_rolling_std(price, 5) OVER (ORDER BY date) AS rolling_volatility
FROM stock_prices
ORDER BY date;
```

---

### stat_rolling_min

**ローリング最小値**を計算する。

**構文**: `stat_rolling_min(column, window_size)`

```sql
SELECT date, price,
       stat_rolling_min(price, 20) OVER (ORDER BY date) AS min_20d
FROM stock_prices
ORDER BY date;
```

---

### stat_rolling_max

**ローリング最大値**を計算する。

**構文**: `stat_rolling_max(column, window_size)`

```sql
SELECT date, price,
       stat_rolling_max(price, 20) OVER (ORDER BY date) AS max_20d
FROM stock_prices
ORDER BY date;
```

---

### stat_rolling_sum

**ローリング合計**を計算する。

**構文**: `stat_rolling_sum(column, window_size)`

```sql
-- 7日間の累計売上
SELECT date, sales,
       stat_rolling_sum(sales, 7) OVER (ORDER BY date) AS weekly_total
FROM daily_sales
ORDER BY date;
```

---

## 移動平均

### stat_moving_avg

**単純移動平均** (SMA) を計算する。`NULL` を含む窓も含めて `stat_rolling_mean` と同等。
異なるのは `Inf` の後だけで、`stat_moving_avg` は R の `stats::filter()` と同じく窓ごとに合計するのに対し、
`stat_rolling_mean` は R の `zoo::rollmean()` と同じく累積和を使うため、`Inf` 以降の窓はすべて `NULL` を返す。

**構文**: `stat_moving_avg(column, window_size)`

```sql
SELECT date, close_price,
       stat_moving_avg(close_price, 20) OVER (ORDER BY date) AS sma_20
FROM stocks
ORDER BY date;
```

---

### stat_ema

**指数移動平均** (EMA) を計算する。最近のデータにより大きな重みを置く移動平均。

$$\text{EMA}_t = \alpha \cdot x_t + (1-\alpha) \cdot \text{EMA}_{t-1}, \quad \alpha = \frac{2}{\text{span}+1}$$

**構文**: `stat_ema(column, span)`

| パラメータ | 説明 |
|---|---|
| `span` | スパン（整数）。pandas の `ewm(span=N)` と同じ慣例 |

```sql
-- 12日間 EMA（α = 2/13 ≈ 0.154）
SELECT date, close_price,
       stat_ema(close_price, 12) OVER (ORDER BY date) AS ema_12
FROM stocks
ORDER BY date;

-- SMA と EMA の比較
SELECT date, close_price,
       stat_moving_avg(close_price, 20) OVER (ORDER BY date) AS sma_20,
       stat_ema(close_price, 20) OVER (ORDER BY date) AS ema_20
FROM stocks
ORDER BY date;
```

---

## ランク

### stat_rank

**順位変換**を行う。各行の値に対してその順位を返す。同順位は平均順位。NULL 行は NULL を返す。

**構文**: `stat_rank(column)`

```sql
-- 成績の順位付け
SELECT name, score,
       stat_rank(score) OVER (ORDER BY name) AS rank
FROM students
ORDER BY name;
```

---

## 欠損値補完

### stat_fillna_mean

NULL 値を**非 NULL 値の平均**で補完する。

**構文**: `stat_fillna_mean(column)`

```sql
SELECT id, value,
       stat_fillna_mean(value) OVER (ORDER BY id) AS filled
FROM measurements
ORDER BY id;
```

---

### stat_fillna_median

NULL 値を**非 NULL 値の中央値**で補完する。

**構文**: `stat_fillna_median(column)`

```sql
SELECT id, value,
       stat_fillna_median(value) OVER (ORDER BY id) AS filled
FROM measurements
ORDER BY id;
```

---

### stat_fillna_ffill

NULL 値を**直前の非 NULL 値**で補完する（前方補完）。先頭が NULL の場合は NULL のまま。

**構文**: `stat_fillna_ffill(column)`

```sql
-- 時系列データの前方補完
SELECT timestamp, sensor_value,
       stat_fillna_ffill(sensor_value) OVER (ORDER BY timestamp) AS filled
FROM sensor_data
ORDER BY timestamp;
```

---

### stat_fillna_bfill

NULL 値を**直後の非 NULL 値**で補完する（後方補完）。末尾が NULL の場合は NULL のまま。

**構文**: `stat_fillna_bfill(column)`

```sql
SELECT timestamp, sensor_value,
       stat_fillna_bfill(sensor_value) OVER (ORDER BY timestamp) AS filled
FROM sensor_data
ORDER BY timestamp;
```

---

### stat_fillna_interp

NULL 値を**線形補間**で補完する。前後の非 NULL 値の間を線形に内挿する。

**構文**: `stat_fillna_interp(column)`

```sql
-- 時系列の欠損値を線形補間
SELECT timestamp, temperature,
       stat_fillna_interp(temperature) OVER (ORDER BY timestamp) AS interpolated
FROM hourly_data
ORDER BY timestamp;
```

---

## エンコーディング・ビニング

### stat_label_encode

値を**ラベルエンコーディング**する。ユニーク値をソートし、0 から順に整数を割り当てる。

**構文**: `stat_label_encode(column)`

```sql
-- カテゴリ値を数値化
SELECT id, color,
       stat_label_encode(color) OVER (ORDER BY id) AS encoded
FROM products
ORDER BY id;
-- color=blue → 0, green → 1, red → 2（ソート順）
```

---

### stat_bin_width

**等幅ビニング**を行う。値域を等幅の n 個のビンに分割し、各値が属するビン番号（0〜n-1）を返す。

**構文**: `stat_bin_width(column, n_bins)`

| パラメータ | 説明 |
|---|---|
| `n_bins` | ビン数（整数） |

```sql
-- 年齢を5つのビンに分類
SELECT name, age,
       stat_bin_width(age, 5) OVER (ORDER BY name) AS age_group
FROM customers
ORDER BY name;
```

---

### stat_bin_freq

**等頻度ビニング**（分位点ビニング）を行う。各ビンに同程度の件数が入るように n 個のビンに分割する。

**構文**: `stat_bin_freq(column, n_bins)`

| パラメータ | 説明 |
|---|---|
| `n_bins` | ビン数（整数） |

```sql
-- 4分位に基づくビニング
SELECT name, income,
       stat_bin_freq(income, 4) OVER (ORDER BY name) AS income_quartile
FROM households
ORDER BY name;
```

---

## 時系列変換

### stat_lag

**ラグ**（遅延）を適用する。k 行前の値を返す。先頭 k 行は NULL。

**構文**: `stat_lag(column, k)`

| パラメータ | 説明 |
|---|---|
| `k` | ラグ数（整数、正の値） |

```sql
-- 1期前の値を取得
SELECT date, price,
       stat_lag(price, 1) OVER (ORDER BY date) AS prev_price,
       price - stat_lag(price, 1) OVER (ORDER BY date) AS price_change
FROM stock_prices
ORDER BY date;
```

---

### stat_diff

**差分**を計算する。order 次の差分を適用する。先頭 order 行は NULL。

**構文**: `stat_diff(column, order)`

| パラメータ | 説明 |
|---|---|
| `order` | 差分の次数（整数） |

```sql
-- 1次差分（前日比）
SELECT date, price,
       stat_diff(price, 1) OVER (ORDER BY date) AS daily_change
FROM stock_prices
ORDER BY date;

-- 2次差分（加速度）
SELECT date, value,
       stat_diff(value, 2) OVER (ORDER BY date) AS acceleration
FROM time_series
ORDER BY date;
```

---

### stat_seasonal_diff

**季節差分**を計算する。period 期前の値との差を返す。先頭 period 行は NULL。

**構文**: `stat_seasonal_diff(column, period)`

| パラメータ | 説明 |
|---|---|
| `period` | 季節周期（整数） |

```sql
-- 前年同月比（月次データ、周期12）
SELECT month, sales,
       stat_seasonal_diff(sales, 12) OVER (ORDER BY month) AS yoy_change
FROM monthly_sales
ORDER BY month;

-- 前週同曜日比（日次データ、周期7）
SELECT date, visitors,
       stat_seasonal_diff(visitors, 7) OVER (ORDER BY date) AS wow_change
FROM daily_traffic
ORDER BY date;
```

---

## 外れ値検出

### stat_outliers_iqr

**IQR 法**で外れ値を検出する。Q1 - 1.5*IQR 未満または Q3 + 1.5*IQR 超の値を外れ値（1.0）、それ以外を正常値（0.0）として返す。NULL 行は NULL。

**構文**: `stat_outliers_iqr(column)`

```sql
-- 外れ値フラグの付与
SELECT id, value,
       stat_outliers_iqr(value) OVER (ORDER BY id) AS is_outlier
FROM measurements
ORDER BY id;

-- 外れ値を除外した平均
SELECT stat_mean(value) AS mean_without_outliers
FROM (
    SELECT value,
           stat_outliers_iqr(value) OVER (ORDER BY id) AS is_outlier
    FROM measurements
)
WHERE is_outlier = 0.0;
```

---

### stat_outliers_zscore

**Z スコア法**で外れ値を検出する。|z| > 3 の値を外れ値（1.0）として返す。

**構文**: `stat_outliers_zscore(column)`

```sql
SELECT id, value,
       stat_outliers_zscore(value) OVER (ORDER BY id) AS is_outlier
FROM measurements
ORDER BY id;
```

---

### stat_outliers_mzscore

**修正 Z スコア法**で外れ値を検出する。中央値と MAD を使用するロバストな方法。|修正Zスコア| > 3.5 の値を外れ値（1.0）として返す。

**構文**: `stat_outliers_mzscore(column)`

```sql
-- ロバストな外れ値検出
SELECT id, value,
       stat_outliers_mzscore(value) OVER (ORDER BY id) AS is_outlier
FROM measurements
ORDER BY id;

-- 3手法の比較
SELECT id, value,
       stat_outliers_iqr(value) OVER (ORDER BY id) AS iqr,
       stat_outliers_zscore(value) OVER (ORDER BY id) AS zscore,
       stat_outliers_mzscore(value) OVER (ORDER BY id) AS mzscore
FROM measurements
ORDER BY id;
```

---

## ロバスト処理

### stat_winsorize

**ウィンザライズ**（裾刈り）を行う。指定パーセンタイルで上下の極値をクリップする。

**構文**: `stat_winsorize(column, percentile)`

| パラメータ | 説明 |
|---|---|
| `percentile` | クリップするパーセンタイル（整数、例: 5 → 5%ile と 95%ile でクリップ） |

```sql
-- 5パーセンタイルでウィンザライズ
SELECT id, value,
       stat_winsorize(value, 5) OVER (ORDER BY id) AS winsorized
FROM measurements
ORDER BY id;

-- ウィンザライズ後の統計量
SELECT stat_mean(w) AS winsorized_mean
FROM (
    SELECT stat_winsorize(value, 10) OVER (ORDER BY id) AS w FROM measurements
);
```

---

## 多重比較補正

| 関数 | 構文 | 説明 |
|---|---|---|
| `stat_bonferroni` | `stat_bonferroni(p)` | Bonferroni 補正 |
| `stat_bh_correction` | `stat_bh_correction(p)` | Benjamini-Hochberg 補正 |
| `stat_holm_correction` | `stat_holm_correction(p)` | Holm 補正 |

p 値の列を受け取り、多重比較補正を施した p 値を**各行に**返す。
R の `p.adjust(p, method = "bonferroni" / "BH" / "holm")` と一致する。

これらがスカラー関数ではなくウィンドウ関数である理由は、BH 補正と Holm 補正が
**p 値集合全体を参照して単調性を強制する**ためである。BH は p 値降順に累積 min を、
Holm は昇順に累積 max を取る。1 行分の情報しか見えないスカラー関数では表現できない。

> **重要**: [呼び出し方](#呼び出し方) のとおり `OVER (ORDER BY <一意なキー>)` で呼び出すこと。
> `OVER ()` はエラーになる。

**構文**:

```sql
stat_bh_correction(p) OVER (
    ORDER BY id
)
```

**NULL の扱い**: NULL 行は補正の母数から除外され、出力でも NULL のまま残る。

```sql
-- 複数の検定結果をまとめて FDR 制御する
SELECT gene, p_value,
       stat_bh_correction(p_value) OVER (
           ORDER BY id
       ) AS q_value
FROM gene_tests
ORDER BY p_value;

-- 補正後に有意なものだけ抽出する
SELECT * FROM (
    SELECT gene, p_value,
           stat_holm_correction(p_value) OVER (
               ORDER BY id
           ) AS p_adj
    FROM gene_tests
) WHERE p_adj < 0.05;
```

**検定数が既知で単一の p 値だけを補正したい場合**は、スカラー版
[`stat_bonferroni_scalar(p, m)`](scalar_tests_helpers-ja.md#多重検定補正) を使う。

---
