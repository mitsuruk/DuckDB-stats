# 変更履歴

DuckDB-stats の変更履歴を記録する。

本プロジェクトは [セマンティック バージョニング](https://semver.org/lang/ja/) に従う。

## [Unreleased]

### 追加

- **sqlite3-stats の DuckDB への初回移植。** DuckDB の C++ 拡張 API で作るロード可能拡張
  (`ext_funcs.duckdb_extension`、DuckDB v1.5.6)として提供する。
  sqlite3-stats の全 258 関数を備える。統計処理のコードはそのまま引き継ぎ、
  R の既定の欠損値処理に従う statcpp v0.5.1 に処理を委ねる。
- ウィンドウ関数は DuckDB のカスタムウィンドウコールバックで実装した。結果はパーティションごとに 1 度計算し、
  行ごとに返す。`OVER (ORDER BY ...)` で呼び出し、`PARTITION BY` にも対応する。
- **`NULL` の SQL の慣例**(sqlite3-stats と同じ):
  - 集約関数は `NULL` の行を無視する(2 カラム関数とグループ列付き関数は、いずれかの列が `NULL` の行を無視する)。
    行がない場合や結果が NaN / Inf の場合は `NULL` を返す。
  - スカラー関数は、いずれかの引数が `NULL` なら `NULL` を返す。省略可能な引数を省略した場合は既定値を使う。
  - パラメータが `NULL` なら、集約関数は `NULL` を返し、
    ウィンドウ関数は SQL の `lag(v, NULL)` と同じく全行 `NULL` を返す。
- **データ中の NaN** は、DuckDB では `NULL` と区別される値である。NaN は statcpp に渡され、R の `NA` と同じく扱われる。
  記述統計と相関は `NULL` を返し、検定とモデルは NaN の行を除いて計算し、分位点や順序に基づく関数は
  "data contains NaN" のエラーになる。
- `doc/nan_notes.md` / `doc/nan_notes-ja.md`: `NULL`、NaN、Inf の扱いを関数の種類ごとにまとめた。
- Google Test スイート(451 件)と結合テストランナー `a.out`(デモ 266 件)。スイートには sqlite3-stats のテスト、
  ローリング統計量・`stat_moving_avg`・`stat_lag` の値のテスト、上記の `NULL`・NaN・Inf の規則のテストを含む。
- Linux では、DuckDB 自身の Linux 用拡張と同じく、DuckDB を拡張に静的リンクする。
  Linux の公式 duckdb CLI はシンボルを公開しないため。Ubuntu 24.04 の公式 CLI v1.5.6 にロードできる。
- `tests/sample/`: そのまま実行できる 100 件のサンプルデータベースと 7 つのクエリ。

### 変更(sqlite3-stats との違い)

- **`stat_bonferroni(p, m)` を `stat_bonferroni_scalar(p, m)` に改名した。** DuckDB ではスカラー関数に
  ウィンドウ関数 `stat_bonferroni(p)` と同じ名前を付けられないため。関数数は 259 になる。
- ウィンドウ関数は全行フレームではなく `OVER (ORDER BY id)` で呼び出す。`OVER ()`、全行フレーム、
  `OVER` なしの呼び出しは、誤った値を返すのではなくエラーになる。
- ウィンドウ関数の結果が Inf の場合は、集約関数と同じく `NULL` を返す(sqlite3-stats は Inf を返す)。
- 可変個の引数を受けるスカラー関数は、受け付ける引数の数ごとのオーバーロードとして登録した。
  引数の数が誤っていればバインドエラーになる。
- 戻り値の型は `DOUBLE`、`BIGINT`、`VARCHAR`(JSON の結果は `VARCHAR`)。
- `_rand` 系のスカラー関数は `VOLATILE` とし、DuckDB が定数畳み込みしないようにした。
- Windows (MSVC) には対応しない。

### 修正(以前の sqlite3-stats と statcpp の不具合)

- **ローリング統計量**(`stat_rolling_mean` / `std` / `min` / `max` / `sum`):
  行 *i* に行 *i* から*始まる*窓の値が入り、
  末尾の行が `NULL` になっていた(ドキュメントと矛盾)。行 *i* には行 *i* で終わる窓の値が入るようにし、
  `NULL` を含む窓は `NULL` にした。
- **`stat_rolling_min` / `stat_rolling_max`** は、`NULL` が窓の先頭にない限り無視していた。
  他のローリング統計量と同じく、そのような窓は `NULL` を返すようにした。
- **`stat_moving_avg`**: `NULL` が 1 つあると、それ以降の全行が `NULL` になっていた。同じ末尾揃えの窓を使い、
  `stat_rolling_mean` と同じ値を返すようにした。異なるのは Inf の後だけで、`stat_moving_avg` は
  R の `stats::filter()` と同じく窓ごとに合計するため、影響は Inf を含む窓だけに限られる。
- **`stat_fillna_median`** は、データ数が偶数のとき中央の 2 値の小さい方で埋めていた(45 ではなく 40)。
  中央値で埋めるようにした。
- **`stat_lag`** は各行自身の値を返し、末尾の *k* 行を `NULL` にしていた。SQL の `lag()` と同じく
  *k* 行前の値を返すようにした。
- `NULL` のパラメータやスカラー関数の引数を 0 として読むか、既定値に置き換えていた。`NULL` を返すようにした。
- ドキュメント: SQL 例 2 件で `FROM` が抜けていた。いくつかの例でウィンドウ関数を `OVER` なしで呼んでいた。
  関数一覧の戻り値型 9 件が誤っていた
  (`stat_odds_ratio`、`stat_relative_risk`、`stat_risk_difference` は JSON テキスト、
  `stat_binomial_coef` は実数、`stat_n_*` のサンプルサイズ関数は整数を返す)。
- 結合テストランナー: デモ [123] と [125] は存在しない列を参照していて、常に失敗していた。
  小さなカテゴリデータのテーブルを使うようにした。
- **statcpp v0.5.1** で、範囲外読み出し、未定義動作、無限ループを修正した。
  正しい引数に対する結果は変わらない。結果が未定義だった次の引数は、SQL のエラーになる。
  - `stat_bin_width()` のデータに Inf がある場合。
    double の範囲を超える有限の幅は正しく分割するようにした。
  - `stat_n_t1()`、`stat_n_t2()`、`stat_n_prop()`、`stat_n_moe_prop()`、
    `stat_n_moe_mean()` で、標本数を表現できない場合(効果量や許容誤差が 0 に近い場合など)。
  - `stat_poisson_quantile()` と `stat_nbinom_quantile()` のパラメータが無限大の場合。

  `stat_duniform_pmf()`、`stat_duniform_cdf()`、`stat_duniform_quantile()` は、
  `b - a` が 64 ビット整数をあふれる場合も正しい結果を返す。
  `stat_beta_rand()` は、形状パラメータが極めて小さい場合もハングしない。
