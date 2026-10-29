# DuckDB 用統計関数ライブラリ

DuckDB に **259 個の統計関数** を追加するロード可能な拡張機能です。

[![CI](https://github.com/mitsuruk/DuckDB-stats/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mitsuruk/DuckDB-stats/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![DuckDB](https://img.shields.io/badge/DuckDB-v1.5.6-yellow.svg)](https://duckdb.org/)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](#動作確認環境)

[English](README.md)

## 概要

DuckDB-stats は、SQL から直接呼び出せる 259 個の統計関数を提供する DuckDB のロード可能な拡張機能です。SQLite3 拡張 [sqlite3-stats](https://github.com/mitsuruk/sqlite3-stats) を DuckDB に移植したもので、関数名・引数・結果を引き継いでいます。sqlite3-stats と同じく [statcpp](https://github.com/mitsuruk/statcpp)（386 個の関数を持つ C++17 ヘッダーオンリー統計ライブラリ）をベースに構築されています。

全 259 関数は 266 件の結合テストおよび 451 件の Google Test 自動テストで検証済みです。

### 主な特徴

- **259 個の SQL 関数** — 記述統計、仮説検定、分布、リサンプリングなどをカバー
- **単一ファイル** — 拡張機能は `ext_funcs.duckdb_extension` 1 つだけ
- **標準 SQL インターフェース** — `SELECT`、`GROUP BY`、`HAVING`、サブクエリなどで使用可能
- **ウィンドウ関数対応** — 移動統計量、移動平均、順位、欠損補完、外れ値検出
- **JSON 出力** — 複合的な結果（複数値、度数表、検定結果）を JSON 文字列で返却
- **対応プラットフォーム** — macOS, Linux

### 関数カテゴリ

| カテゴリ | 個数 | 説明 |
| --- | --- | --- |
| 基本集約関数 | 24 | 単一カラム: `SELECT stat_xxx(col) FROM t` |
| パラメータ付き集約関数 | 20 | パラメータ指定: `SELECT stat_xxx(col, param) FROM t` |
| 2 カラム集約関数 | 27 | 2 カラム入力: `SELECT stat_xxx(col1, col2) FROM t` |
| ウィンドウ関数 | 26 | 全行スキャン型ウィンドウ関数: `stat_xxx(col) OVER (ORDER BY id)` |
| 複合集約関数 | 41 | JSON 返却の集約関数、2 標本検定、生存時間解析 |
| スカラー関数 — 検定・補助 | 38 | 分布関数、比率検定、多重検定補正 |
| スカラー関数 — 分布・変換 | 83 | 連続・離散分布、効果量変換、検出力分析 |

## クイックスタート

### 前提条件

- CMake 3.20 以上、および Ninja（macOS: `brew install ninja`、Ubuntu: `sudo apt install ninja-build`）
- DuckDB をソースからビルドするため 8 GB 以上のメモリ（CMake は 4 並列でコンパイルする）
- C++17 コンパイラ: Apple Clang 17 以上 または GCC 13 以上
- DuckDB v1.5.6（CMake がソースから自動ビルド）
- [statcpp](https://github.com/mitsuruk/statcpp)（CMake が自動ダウンロード）

### ビルド

```bash
cd DuckDB-stats
cmake -B build          # 初回は DuckDB をソースからビルドする（10〜30 分）
cmake --build build
```

以下が生成されます:

- `build/ext_funcs.duckdb_extension` — ロード可能な拡張機能
- `build/a.out` — テストランナー (266 テスト)

### テストの実行

```bash
./build/a.out
# [OK] All tests completed (266 tests).
```

#### Google Test スイート

[Google Test](https://github.com/google/googletest) が必要です (macOS: `brew install googletest`).

```bash
cmake -B build -DSTAT_TESTS=ON
cmake --build build

# CTest 経由で実行
ctest --test-dir build --output-on-failure

# Google Test 実行ファイルを直接実行
./build/stat_tests

# 特定のテストスイートのみ実行
./build/stat_tests --gtest_filter="BasicAggregates.*"
```

| テストファイル | 関数数 | テスト数 |
|---|---|---|
| basic_aggregates_test.cpp | 24 | 63 |
| parameterized_aggregates_test.cpp | 20 | 53 |
| two_column_aggregates_test.cpp | 27 | 58 |
| window_functions_test.cpp | 26 | 61 |
| complex_aggregates_test.cpp | 41 | 80 |
| scalar_tests_helpers_test.cpp | 38 | 43 |
| scalar_distributions_test.cpp | 83 | 93 |
| **合計** | **259** | **451** |

### 拡張機能のロード

拡張機能は署名されておらず、DuckDB の C++ 拡張 API で作られているため、未署名拡張を許可した **DuckDB v1.5.6** にだけロードできます。

#### duckdb CLI から

```sql
-- CLI は duckdb -unsigned で起動する
LOAD './build/ext_funcs.duckdb_extension';
```

macOS の公式配布の duckdb CLI は相対パスを拒否します("relative path not allowed in hardened program")。
その場合は `duckdb -unsigned -cmd "LOAD '$PWD/build/ext_funcs.duckdb_extension'"` のように絶対パスを渡します。

#### C++ から

```cpp
duckdb::DBConfig config;
config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
duckdb::DuckDB db(nullptr, &config);
duckdb::Connection con(db);
con.Query("LOAD './build/ext_funcs.duckdb_extension'");
```

## 使用例

### 記述統計

```sql
SELECT stat_mean(score), stat_stdev(score), stat_median(score)
FROM exam_results;

SELECT class, stat_mean(score), stat_cv(score)
FROM exam_results
GROUP BY class;
```

### 仮説検定

```sql
-- 1 標本 t 検定（H0: mu = 50）
SELECT stat_t_test(score, 50) FROM exam_results;

-- 2 標本 Welch の t 検定（JSON を返却）
SELECT stat_t_test_welch(group_a, group_b) FROM measurements;

-- JSON の結果から 1 項目を取り出す
SELECT json_extract(stat_t_test(score, 50), '$.p_value')::DOUBLE AS p
FROM exam_results;
```

### ウィンドウ関数

```sql
SELECT date, price,
       stat_rolling_mean(price, 7) OVER (ORDER BY date) AS ma7,
       stat_ema(price, 5)          OVER (ORDER BY date) AS ema
FROM stock_prices
ORDER BY date;
```

### 分布関数（スカラー）

```sql
-- 正規分布の分位点（95% の z 値）
SELECT stat_normal_quantile(0.975, 0, 1);  -- 1.96

-- 検出力分析: 2 標本 t 検定に必要なサンプルサイズ
SELECT stat_n_t2(0.5, 0.8, 0.05);  -- 効果量=0.5, 検出力=0.80, alpha=0.05
```

## サンプルデータベース

`tests/sample/` には、そのまま実行できる小さなデータとクエリを用意しています。拡張機能で何ができるかをすぐに確かめられます:

| ファイル | 内容 |
| --- | --- |
| `sample_data.sql` | テーブル `students`: 生徒 100 人の試験成績(`id`、`class`、`study_hours`、`score`) |
| `sample_queries.sql` | 7 つのクエリ: 記述統計、`GROUP BY`、2 標本の検定、相関と回帰、ウィンドウ関数、分布関数 |

点数は学習時間とともに上がり、クラス B の平均点が高く、特異な結果を 3 件仕込んであります。
ビルド後、プロジェクトのルートで実行します:

```bash
duckdb -unsigned \
    -cmd "LOAD '$PWD/build/ext_funcs.duckdb_extension'" \
    -c ".read tests/sample/sample_data.sql" \
    -c ".read tests/sample/sample_queries.sql"
```

主な結果(DuckDB v1.5.6):

| 問い | 関数 | 結果 |
| --- | --- | --- |
| 代表的な点数は? | `stat_mean`、`stat_median` | 平均 65.52、中央値 68.0 |
| クラス B の方が高いか? | `stat_t_test_welch` | p = 0.013(有意)、Cohen's d = -0.51 |
| 勉強は点数につながるか? | `stat_pearson_r`、`stat_simple_regression` | r = 0.638、1 時間あたり +4.07 点 |
| 外れ値はどれか? | `stat_outliers_iqr`(ウィンドウ関数) | 生徒 88 と 64(9 点と 14 点) |
| 正規分布を仮定したときの 80 点超の割合 | `stat_normal_cdf` | 0.202(実際は 0.20) |

## 共通仕様

- **NULL の扱い**: すべての集約関数・ウィンドウ関数で NULL 値は暗黙的に無視されます。
- **データ不足時**: 非 NULL 値が十分でない場合は `NULL` を返します（例: 分散の計算には n >= 2 が必要）。
- **数値精度**: IEEE 754 倍精度（64 ビット）。`NaN` および `Inf` は `NULL` にマッピングされます。
- **戻り値の型**: `DOUBLE`、`BIGINT`（離散分布の分位点、サンプルサイズ）、`VARCHAR`（JSON・文字列）のいずれか。
- **JSON 出力**: 複合集約関数の結果は JSON 文字列（`VARCHAR`）として返されます。DuckDB の `json_extract()` は JSON 型を返すため、数値は `::DOUBLE` でキャストしてください。
- **不正な引数**: 通常の SQL エラー（`Invalid Input Error: statcpp::...`）になり、接続は使用可能なまま残ります。
- **乱数関数**: `_rand` 系のスカラー関数は `VOLATILE` で、行ごとに異なる値を返します。

## sqlite3-stats との違い

| 項目 | sqlite3-stats | DuckDB-stats |
|---|---|---|
| ロード | `.load ./ext_funcs sqlite3_ext_funcs_init` | `LOAD './build/ext_funcs.duckdb_extension'`（`duckdb -unsigned`） |
| エンジンのバージョン | 拡張ロードに対応した任意の SQLite3 | DuckDB v1.5.6 のみ（C++ 拡張 API） |
| ウィンドウ関数 | 全行フレーム `OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)` | `OVER (ORDER BY id)`。`OVER ()` と `OVER` なしの呼び出しはエラー（[詳細](doc/ref/window_functions-ja.md#呼び出し方)） |
| スカラー版 Bonferroni | `stat_bonferroni(p, m)` | `stat_bonferroni_scalar(p, m)`（スカラー関数はウィンドウ関数 `stat_bonferroni(p)` と同名にできない） |
| 関数数 | 258 | 259（上記の改名による） |
| 可変長のスカラー関数 | 任意個の引数を受ける 1 つの登録 | 受け付ける引数の数ごとのオーバーロード。数が誤っていればバインドエラー |
| 戻り値の型 | REAL / INTEGER / TEXT | DOUBLE / BIGINT / VARCHAR |
| データ中の NaN | 格納できない(`NULL` になる) | `NULL` と区別される値で、statcpp に渡される([詳細](doc/nan_notes-ja.md)) |
| ウィンドウ関数の Inf の結果 | Inf のまま返す | 集約関数と同じく `NULL` を返す |
| `_rand` 系関数 | 決定的関数として登録 | `VOLATILE` |
| 対応プラットフォーム | macOS, Linux, Windows (MSVC) | macOS, Linux |

ローリング統計量・`stat_moving_avg`・`stat_lag` は、以前の sqlite3-stats では挙動がドキュメントと食い違っていたため本移植で修正しました。その後 sqlite3-stats も同様に修正されています。パラメータやスカラー関数の引数の `NULL` の規則は両者で同じです。

## 関数リファレンス

全 259 関数の詳細ドキュメント:

- [関数リファレンス（ハブ）](doc/function_reference-ja.md)
  - [基本集約関数（24）](doc/ref/basic_aggregates-ja.md)
  - [パラメータ付き集約関数（20）](doc/ref/parameterized_aggregates-ja.md)
  - [2 カラム集約関数（27）](doc/ref/two_column_aggregates-ja.md)
  - [ウィンドウ関数（26）](doc/ref/window_functions-ja.md)
  - [複合集約関数（41）](doc/ref/complex_aggregates-ja.md)
  - [スカラー関数 — 検定・補助（38）](doc/ref/scalar_tests_helpers-ja.md)
  - [スカラー関数 — 分布・変換（83）](doc/ref/scalar_distributions-ja.md)
- [NULL・NaN・Inf の注意事項](doc/nan_notes-ja.md)
- [組み込み関数との比較](doc/builtin_comparison-ja.md) — DuckDB 組み込みの統計関数との動作の違い
- [DuckDB ロード可能拡張ガイド](doc/duckdb_LOAD_EXTENSION-ja.md) — 拡張の作り方と C++ API を採用した理由

## プロジェクト構成

```text
DuckDB-stats/
├── .github/workflows/ci.yml               # CI(GitHub Actions: macOS、Ubuntu)
├── CMakeLists.txt
├── LICENSE
├── README.md                              # 英語版 README
├── README-ja.md                           # このファイル（日本語版）
├── mkdocs.yml                             # ドキュメントサイト（mkdocs serve）
├── tests/
│   ├── test_helpers.hpp                   # Google Test フィクスチャ・ヘルパー
│   ├── sample/                            # サンプルデータベースとクエリ(生徒 100 人)
│   ├── basic_aggregates_test.cpp          # 24 関数, 63 テスト
│   ├── parameterized_aggregates_test.cpp  # 20 関数, 53 テスト
│   ├── two_column_aggregates_test.cpp     # 27 関数, 58 テスト
│   ├── window_functions_test.cpp          # 26 関数, 61 テスト
│   ├── complex_aggregates_test.cpp        # 41 関数, 80 テスト
│   ├── scalar_tests_helpers_test.cpp      # 38 関数, 43 テスト
│   └── scalar_distributions_test.cpp      # 83 関数, 93 テスト
├── src/
│   ├── ext_funcs.cpp                      # 拡張機能: 259 個の SQL 関数
│   └── main.cpp                           # 結合テストランナー（266 テスト）
├── doc/
│   ├── index.md                           # ドキュメントのトップ
│   ├── function_reference.md              # 関数リファレンス（英語版）
│   ├── function_reference-ja.md           # 関数リファレンス（日本語版）
│   ├── nan_notes.md                       # NULL・NaN・Inf の注意事項（英語版）
│   ├── nan_notes-ja.md                    # NULL・NaN・Inf の注意事項（日本語版）
│   ├── duckdb_LOAD_EXTENSION.md           # 拡張機能の実装ガイド（英語版）
│   ├── duckdb_LOAD_EXTENSION-ja.md        # 拡張機能の実装ガイド（日本語版）
│   ├── builtin_comparison.md              # 組み込み関数との比較（英語版）
│   ├── builtin_comparison-ja.md           # 組み込み関数との比較（日本語版）
│   ├── CHANGELOG.md                       # 変更履歴（英語版）
│   ├── CHANGELOG-ja.md                    # 変更履歴（日本語版）
│   └── ref/                               # カテゴリ別の関数詳細（英語版・日本語版）
├── cmake/
│   ├── DuckDB_C_CPP.cmake                 # DuckDB をソースからビルド
│   ├── statcpp.cmake                      # statcpp を自動ダウンロード
│   ├── extension_metadata.cmake           # LOAD 用メタデータフッタを付与
│   └── null.txt                           # フッタ生成スクリプト用の 0x00 1 バイト
└── download/                              # 自動ダウンロードされた依存関係（git 管理外）
    ├── DuckDB/
    └── statcpp/
```

## 関連プロジェクト

DuckDB-stats は **statcpp ファミリー** の一部です。用途に応じて選択してください:

| 用途 | リポジトリ | 説明 |
| --- | --- | --- |
| C++ ライブラリ | [statcpp](https://github.com/mitsuruk/statcpp) | C++17 ヘッダーオンリー統計ライブラリ (386 関数) |
| UNIX CLI | [statcppCLI](https://github.com/mitsuruk/statcppCLI) | UNIX パイプライン向けコマンドラインツール |
| SQL (SQLite3) | [sqlite3-stats](https://github.com/mitsuruk/sqlite3-stats) | SQLite3 ロード可能な拡張機能 (258 関数) |
| SQL (DuckDB) | **DuckDB-stats** (このリポジトリ) | DuckDB ロード可能な拡張機能 (259 関数) |

**対応 statcpp バージョン**: v0.5.1

## 動作確認環境

- macOS (Apple Silicon) + Apple Clang 21.0.0、ソースからビルドした DuckDB v1.5.6
- 公式配布の duckdb CLI v1.5.6（macOS）へのロードも確認済み
- Ubuntu 24.04 (arm64、Parallels の VM と Docker) + GCC 13.2 / 13.3、ソースからビルドした DuckDB v1.5.6。公式配布の duckdb CLI v1.5.6（Linux）へのロードも確認済み
- CI(GitHub Actions): `main` への push と PR のたびに、`macos-latest`(arm64)と `ubuntu-24.04`(x86_64)で
  ビルドとテストを行い、公式の duckdb CLI v1.5.6 への拡張のロードを確認する
- Windows (MSVC) は対象外

## ライセンス

このプロジェクトは MIT ライセンスの下で公開されています。

### 依存ライブラリ

| ライブラリ | ライセンス |
| --- | --- |
| [statcpp](https://github.com/mitsuruk/statcpp) | MIT License |
| [DuckDB](https://duckdb.org/) | MIT License |

## 謝辞

このプロジェクトの開発には以下のツールと AI アシスタントを活用しています:

- **[statcpp](https://github.com/mitsuruk/statcpp)** — C++17 ヘッダーオンリー統計ライブラリ（386 関数）
- **Claude Code for VS Code** — ドキュメントの英文法,表現の誤り,コード内のコメント,変数名の英語名の訂正案,SQL テスト用のデータを作成しています

---

**免責事項**: このライブラリは数値安定性や極端なエッジケースへの対応において、商用の統計ソフトウェアと同等のレベルを保証するものではありません。研究や本番環境で結果を使用する場合は、R や Python/SciPy などの確立されたツールとのクロスバリデーションを推奨します。
