# DuckDB ロード可能拡張ガイド

[English](duckdb_LOAD_EXTENSION.md)

本プロジェクトの DuckDB ロード可能拡張の実装ガイド。
拡張は C++ (`src/ext_funcs.cpp`) で、DuckDB の C++ 拡張 API を使って実装している。

---

## 目次

1. [ロード可能拡張とは](#1-ロード可能拡張とは)
2. [プロジェクト構成](#2-プロジェクト構成)
3. [ビルドと実行](#3-ビルドと実行)
4. [ロード可能拡張の仕組み](#4-ロード可能拡張の仕組み)
5. [拡張関数の実装パターン](#5-拡張関数の実装パターン)
6. [DuckDB C++ API リファレンス](#6-duckdb-c-api-リファレンス)
7. [DuckDB におけるウィンドウ関数](#7-duckdb-におけるウィンドウ関数)
8. [動的ライブラリに関する注意](#8-動的ライブラリに関する注意)

---

## 1. ロード可能拡張とは

### 概要

DuckDB のロード可能拡張は、DuckDB が実行時に `LOAD` で開く共有ライブラリである。
DuckDB を再ビルドせずに、実行中のデータベースへ新しい SQL 関数を登録できる。
ファイル名は `.duckdb_extension` で終わり、ファイル末尾のメタデータフッタが、拡張のビルド対象の
プラットフォームとバージョンを DuckDB に伝える。

DuckDB には 2 種類の拡張 API がある。

| API | ABI | ロードできる DuckDB | ウィンドウ関数 |
|---|---|---|---|
| C 拡張 API (`duckdb_extension.h`) | 安定 | 宣言した C API 以上の任意の DuckDB | 使えない |
| C++ 拡張 API (`duckdb.hpp`) | なし | ビルドに使った DuckDB と**完全に同じバージョンのみ** | `window` / `window_init` コールバック |

本プロジェクトは **C++ API** を使う。26 個のウィンドウ関数はパーティション全体で結果を計算し、
現在行の要素を返す。これにはカスタムウィンドウ用のコールバックが必要だが、安定版の C API には無い。
C API では 15 個(`stat_rank`、`stat_fillna_mean`、`stat_bh_correction` など)を実装できない。
その代償が後述のバージョン固定である。

### できること

- スカラー関数、集約関数、カスタムのウィンドウ評価を持つ集約関数
- 引数の数による関数のオーバーロード(`stat_normal_cdf(x)` と `stat_normal_cdf(x, mu, sigma)` は別のオーバーロードに解決される)
- 引数の暗黙の型変換: 引数はすべて `DOUBLE` で宣言しているため、`INTEGER`、`BIGINT`、`DECIMAL`、`FLOAT` は
  拡張に届く前に変換される
- `VOLATILE` 関数: DuckDB が定数畳み込みもキャッシュもしない(`_rand` 系で使用)

### 制約

- **バージョン固定**: C++ 拡張はビルドに使った DuckDB のバージョン(本プロジェクトでは v1.5.6)にしかロードできない。
  DuckDB はフッタのバージョンを照合し、異なれば拒否する。
- **未署名**: 拡張は DuckDB の署名を持たないため、`allow_unsigned_extensions` を有効にしたとき(`duckdb -unsigned`)
  だけロードできる。
- **Windows (MSVC)** は対象外(ビルド・検証していない)。C++ 拡張は DuckDB の C++ シンボルをホストプロセスから
  解決するため、Windows では別のリンク構成が必要になる。
- スカラー関数は集約関数・ウィンドウ関数と同じ名前を持てない。このため `stat_bonferroni` のスカラー形式は
  `stat_bonferroni_scalar` という名前にしている。

### セキュリティ上の注意

拡張のロードは、データベースプロセスの権限でネイティブコードを実行することを意味する。
自分でビルドした、または信頼できる拡張だけをロードすること。`allow_unsigned_extensions` は本拡張に限らず
すべての未署名拡張の署名検査を外すので、必要な場合だけ有効にすること。

---

## 2. プロジェクト構成

```text
DuckDB-stats/
├── CMakeLists.txt
├── src/
│   ├── ext_funcs.cpp              # 拡張本体: 259 個の SQL 関数
│   └── main.cpp                   # 結合テストランナー(266 件)
├── tests/                         # Google Test スイート(451 件)
├── cmake/
│   ├── DuckDB_C_CPP.cmake         # DuckDB をソースから取得・ビルド
│   ├── statcpp.cmake              # statcpp を取得(ヘッダーオンリー)
│   ├── extension_metadata.cmake   # LOAD 用メタデータフッタを付与
│   └── null.txt                   # フッタ生成スクリプトが使う 0x00 1 バイト
└── doc/
```

### 外部ライブラリ

#### statcpp

[statcpp](https://github.com/mitsuruk/statcpp) は C++17 のヘッダーオンリー統計ライブラリである。拡張の統計計算は
すべて statcpp に委譲しており、拡張は SQL の行を statcpp の呼び出しに橋渡しするだけである。

#### DuckDB

`cmake/DuckDB_C_CPP.cmake` が DuckDB v1.5.6 のソースを取得し、`download/DuckDB/` に 1 度だけビルドする。
拡張はそのビルドのヘッダーだけを使い、ランナーとテストは `libduckdb` をリンクする。

### 提供する SQL 関数

| カテゴリ | 個数 | 種類 |
|---|---|---|
| 基本集約関数 | 24 | 集約 |
| パラメータ付き集約関数 | 20 | 集約 |
| 2 カラム集約関数 | 27 | 集約 |
| ウィンドウ関数 | 26 | カスタムウィンドウ評価付きの集約 |
| 複合集約関数 | 41 | 集約(主に JSON を返す) |
| スカラー関数 — 検定・補助 | 38 | スカラー |
| スカラー関数 — 分布・変換 | 83 | スカラー |
| **合計** | **259** | |

全関数の一覧は [関数リファレンス](function_reference-ja.md) を参照。

---

## 3. ビルドと実行

```bash
# 構成(初回は DuckDB をソースからビルドするため時間がかかる)
cmake -B build

# 拡張とランナーをビルド
cmake --build build

# 結合テストランナーを実行(266 件)
./build/a.out

# Google Test スイートをビルドして実行
cmake -B build -DSTAT_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

生成されるファイル:

| ファイル | 説明 |
|---|---|
| `build/ext_funcs.duckdb_extension` | ロード可能拡張(メタデータフッタ付き) |
| `build/ext_funcs.duckdb_extension_raw` | フッタ付与前のリンク済みモジュール |
| `build/a.out` | 結合テストランナー |
| `build/stat_tests` | Google Test 実行ファイル(`-DSTAT_TESTS=ON`) |

### duckdb CLI からの利用

#### 前提条件

- 拡張のビルド対象と**完全に同じ**バージョン(v1.5.6)の duckdb CLI
- 未署名拡張の許可: CLI を `-unsigned` 付きで起動する

#### ロード

```sql
-- duckdb -unsigned
LOAD './build/ext_funcs.duckdb_extension';

SELECT stat_mean(x) FROM range(1, 11) t(x);
-- 5.5
```

バージョンが一致しない場合は、拡張のコードが実行される前に `LOAD` の時点で報告される。

```text
Invalid Input Error: Failed to load 'build/ext_funcs.duckdb_extension', The file was built
specifically for DuckDB version 'v1.5.6' and can only be loaded with that version of DuckDB.
(this version of DuckDB is 'v1.4.4')
```

ローカルでビルドした DuckDB に対してビルドした拡張が、公式配布の duckdb CLI v1.5.6(macOS)にロードでき、
全関数が動作することを確認済みである。

### C++ からの利用

```cpp
duckdb::DBConfig config;
config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
duckdb::DuckDB db(nullptr, &config);
duckdb::Connection con(db);
auto r = con.Query("LOAD './build/ext_funcs.duckdb_extension'");
if (r->HasError()) { /* r->GetError() */ }
```

プログラムは、拡張のビルドに使ったものと同じバージョンの DuckDB(`libduckdb`)をリンクする必要がある。

---

## 4. ロード可能拡張の仕組み

### 全体の流れ

```text
LOAD 'ext_funcs.duckdb_extension'
  │
  ├─ 1. ファイル末尾の 534 バイトのメタデータフッタを読む
  │     └─ プラットフォーム(osx_arm64)、DuckDB バージョン(v1.5.6)、ABI 種別(CPP)、
  │        署名(allow_unsigned_extensions が有効なら省略)を照合
  ├─ 2. ファイルを dlopen() する
  ├─ 3. シンボル ext_funcs_duckdb_cpp_init を探す
  └─ 4. ExtensionLoader& を渡して呼び出す。拡張が関数を登録する
```

### エントリポイントの命名規則

エントリポイントは `<拡張名>_duckdb_cpp_init` で、拡張名はファイル名から `.duckdb_extension` を除いたもの。
DuckDB のマクロで宣言し、`extern "C"` で囲む。

```cpp
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ext_funcs, loader) {
    duckdb_stats::register_all(loader);
}

}
```

`extern "C"` が無いとシンボルが C++ のマングリングを受け、`LOAD` が「did not contain the expected entrypoint
function」で失敗する。

### メタデータフッタ

フッタの無いファイルは DuckDB に拒否される。`cmake/extension_metadata.cmake` が、ビルドのたびにリンク済み
モジュールのコピーへフッタを付与する(再ビルドでフッタが二重に付かないようにするため)。

| フィールド | 値 |
|---|---|
| 1 | `4`(フッタの形式) |
| 2 | プラットフォーム(例: `osx_arm64`) |
| 3 | DuckDB のバージョン(例: `v1.5.6`)。ABI 種別 `CPP` では**完全一致**で照合される |
| 4 | 拡張のバージョン |
| 5 | ABI 種別 `CPP` |
| 署名 | 256 バイトのゼロ(未署名) |

### コンパイルフラグ

- C++17、`-Wall -Wextra -Wpedantic`
- DuckDB のヘッダーは `SYSTEM` ヘッダーとしてインクルードし、DuckDB 自身の警告をビルドに混ぜない
- macOS: `-undefined dynamic_lookup`(DuckDB のシンボルはロード時にホストから解決される)
- Linux: DuckDB を拡張に静的リンクする。hidden visibility、`--exclude-libs,ALL`、`--gc-sections`、
  `--no-undefined` を指定する(「DuckDB のシンボル」の節を参照)

---

## 5. 拡張関数の実装パターン

### 5.1. 引数と結果へのアクセス

統計計算のコードは sqlite3-stats と共通である。SQLite 版は `sqlite3_value_double()` などで引数を読み、
`sqlite3_result_*()` で結果を書く。DuckDB 版は 2 つの小さなクラスで同じ形を保っている。

| クラス | 対応するもの | 意味 |
|---|---|---|
| `ArgRow` | `sqlite3_value** argv` | `Double(i)`、`Int64(i)`、`Int(i)`、`IsNull(i)`、`Count()`。NULL 引数は `0.0` として読める |
| `ResultSlot` | `sqlite3_context*` | `SetDouble`、`SetInt64`、`SetText`、`SetNull`。NaN / Inf と空文字列は NULL になる |

関数は `FunctionNullHandling::SPECIAL_HANDLING` で登録しており、NULL 引数もコードに届き、SQLite 版と同じ
扱いになる。スカラー関数は引数のどれかが NULL なら NULL を返し、パラメータが NULL なら集約関数は NULL を、
ウィンドウ関数は全行 NULL を返す([NULL・NaN・Inf の注意事項](nan_notes-ja.md) を参照)。

### 5.2. 集約関数

集約関数はそれぞれ小さな `Spec` で記述し、`StatAggregate<Spec>` が DuckDB の `AggregateFunction` に変換する。

```cpp
struct Spec {
    struct State { /* 集めた行 */ };
    static void Step(State&, const ArgRow&);          // 入力 1 行
    static void Merge(State& target, const State&);   // 部分状態 2 つの結合
    static void Final(const State&, ResultSlot&);     // 結果の計算
};
```

DuckDB はグループごとに固定長の状態スロットを用意する。スロットはヒープ上の `State` へのポインタを持ち、
`State` は最初の行で作られ、集約のデストラクタコールバックで解放される。どの経路でも解放が保証される。

`Merge` は結合元を変更してはならない。集約をウィンドウフレーム上で評価するとき、DuckDB はセグメント木の
ノードを結合し、その後も再利用するためである。

SQLite 版のテンプレートとの対応:

| SQLite 版のテンプレート | DuckDB 版の Spec |
|---|---|
| `SingleColumnAggregate` | `SingleColumnAggregate<Func>` |
| `SingleColumnParamAggregate[Text]` | `SingleColumnParamAggregate[Text]<N, Func>` |
| `TwoColumnAggregate[Text]` | `TwoColumnAggregate[Text]<Func>` |
| `TwoColumnParamAggregate[Text]` | `TwoColumnParamAggregate[Text]<N, Func>` |
| `SingleColumnAggregateText` | `SingleColumnAggregateText<Func>` |
| `FullScanWindowFunction` | `FullScanWindowFunction<Func>`([7 章](#7-duckdb-におけるウィンドウ関数)を参照) |
| `logrank_step` / `logrank_final` | `LogrankAggregate` |

### 5.3. スカラー関数

スカラー関数の実装は SQLite 版のシグネチャの形を保つ。

```cpp
static void sf_normal_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::normal_cdf(x, mu, sigma));
}

register_scalar<sf_normal_cdf>(loader, "stat_normal_cdf", {1, 2, 3});
```

SQLite 版はこのような関数を可変長引数(`-1`)で登録していた。DuckDB 版は受け付ける引数の数ごとに
オーバーロードを登録するため、引数の数が誤っていればバインドエラーになる。戻り値の型は関数ごとに固定で、
`DOUBLE`、`BIGINT`(離散分布の分位点、サンプルサイズ)、`VARCHAR`(JSON・文字列)のいずれかである。
`register_scalar_nd` は `VOLATILE` な関数を登録する。

### 5.4. エラー

statcpp は不正な引数を `std::invalid_argument` の送出で報告する。すべてのコールバックは `invoke_guarded()` を
経由し、DuckDB 以外の例外を `duckdb::InvalidInputException` に変換する。

```text
D SELECT stat_poisson_quantile(1.5, 2.5);
Invalid Input Error: statcpp::poisson_quantile: p must be in [0, 1]
```

その文が失敗するだけで、接続は使える状態のまま残る。

---

## 6. DuckDB C++ API リファレンス

拡張が使う DuckDB の API:

| API | 用途 |
|---|---|
| `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)` | エントリポイントの宣言 |
| `ExtensionLoader::RegisterFunction(...)` | `ScalarFunctionSet` / `AggregateFunctionSet` の登録 |
| `ScalarFunction(args, ret, fn)` | スカラー関数。`fn(DataChunk&, ExpressionState&, Vector&)` |
| `ScalarFunction::SetNullHandling` / `SetVolatile` | NULL の扱いと安定性 |
| `AggregateFunction(args, ret, size, init, update, combine, finalize, null_handling, simple_update, bind, destructor)` | 集約関数 |
| `AggregateFunction::window`、`SetWindowInitCallback` | カスタムのウィンドウ評価 |
| `WindowPartitionInput` | パーティションの引数行(`ColumnDataCollection`) |
| `SubFrames` | 評価中の行のフレーム境界 |
| `Vector::ToUnifiedFormat`、`UnifiedVectorFormat::GetData` | 引数と状態ポインタの読み取り |
| `FlatVector::GetData`、`FlatVector::SetNull`、`StringVector::AddString` | 結果の書き込み |
| `duckdb::InvalidInputException` | SQL エラーの報告 |

C++ API は安定版ではなく、DuckDB のリリース間で名前やシグネチャが変わる。DuckDB を更新するときは拡張の
再ビルドが必要で、この層の修正が必要になることもある。

---

## 7. DuckDB におけるウィンドウ関数

### 問題

SQLite 版は、パーティションの全行を集めてから、結果の要素を 1 行ずつ返す。これは、全行フレーム
`ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING` のもとで SQLite が `xValue` を行ごとに 1 回ずつ
呼ぶことに依存している。

DuckDB は `OVER ()` と全行フレームを**定数集約器**で評価する。パーティションごとに集約を 1 度だけ計算し、
その値を全行に複製する。この場合、拡張のどのコールバックからも行ごとの値は返せない。

### 設計

ウィンドウ関数は DuckDB のカスタムウィンドウ用コールバックを使う。

1. `window_init` はパーティション全体(`WindowPartitionInput`)を 1 度だけ受け取る。statcpp の計算を実行し、
   結果のベクタを共有状態に保持する。
2. `window` は行ごとに呼ばれる。ただしパーティション内での行の位置は**渡されず**、フレーム境界
   (`SubFrames`)だけが渡される。そこで行を**フレームの最終行**として特定する。これは `OVER (ORDER BY ...)` の
   既定のフレームと `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` で成り立つ。

```sql
SELECT id, val, stat_rank(val) OVER (PARTITION BY grp ORDER BY id) AS r
FROM t
ORDER BY id;
```

### 帰結

| 呼び出し方 | 結果 |
|---|---|
| 一意なキーでの `OVER (ORDER BY key)` | 行ごとの正しい結果 |
| `OVER (PARTITION BY g ORDER BY key)` | 正しい。パーティションごとに計算される |
| `key` に同値がある `OVER (ORDER BY key)` | 同値の行は、同値グループの最終行の結果を共有する |
| `OVER ()`、全行フレーム、`OVER` なし | エラー: "this is a window function: call it with an ORDER BY in the OVER clause" |

`OVER` なしの呼び出しと `OVER ()` は、どちらも集約の finalize に到達し区別できないため、両方ともエラーにしている。

計算はパーティションごとに 1 度だけ行われるので、コストは statcpp の呼び出し 1 回分であり、フレームには依存しない。

---

## 8. 動的ライブラリに関する注意

### エントリポイントのシンボル

```bash
nm -gU build/ext_funcs.duckdb_extension_raw | grep init
# T _ext_funcs_duckdb_cpp_init
```

### DuckDB のシンボル

macOS では、拡張は DuckDB をリンクしない。`-undefined dynamic_lookup` でリンクし、参照する 32 個の DuckDB
シンボルはロード時にホストプロセス(duckdb CLI、または `libduckdb` をリンクしたプログラム)から解決される。
macOS の公式 duckdb CLI は DuckDB の C++ シンボルを公開しているため、これで動く。

Linux では同じ方法は使えない。Linux の公式 duckdb CLI はシンボルを 1 つも公開しておらず、`LOAD` は
`undefined symbol: _ZTIN6duckdb18BaseScalarFunctionE` などで失敗する。そこで DuckDB 自身の Linux 用拡張と同じく
(DuckDB の `extension_build_tools.cmake` の `EXTENSION_STATIC_BUILD`)、DuckDB を静的リンクする:

- `libduckdb_static.a`、`libdummy_static_extension_loader.a`、同梱のサードパーティ製ライブラリ(`libduckdb_*.a`)を
  リンクする。組み込みの拡張ローダーと jemalloc はホストが持つため除く
- hidden visibility と `--exclude-libs,ALL` で、エントリポイント以外のシンボルをすべて非公開にする。
  これにより、同梱した DuckDB が `libduckdb` をリンクしたホスト(`a.out`、`stat_tests`)と衝突しない
- `--no-undefined` で、シンボルの不足を `LOAD` 時ではなくリンク時に検出する

Linux の拡張は約 2 MB ではなく約 37 MB になる。

```bash
otool -L build/ext_funcs.duckdb_extension_raw
#   /usr/lib/libc++.1.dylib
#   /usr/lib/libSystem.B.dylib
```

シンボルがホストから来るため、ホストは同じバージョンの DuckDB であり、互換性のある設定でビルドされている
必要がある。前者はフッタの完全一致照合で DuckDB が保証し、後者は同じタグのリリースビルドであれば成り立つ。

### バージョンの更新

新しい DuckDB に移行するには、`cmake/DuckDB_C_CPP.cmake` の `DUCKDB_VERSION` を変更し、
`download/DuckDB/DuckDB_C_CPP-install` を削除して再構成し、テストを実行する。拡張をロードする duckdb CLI も
同じバージョンに更新する必要がある。
