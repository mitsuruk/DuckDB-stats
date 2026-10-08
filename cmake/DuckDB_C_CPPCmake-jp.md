# DuckDB_C_CPP.cmake リファレンス

## 概要

`DuckDB_C_CPP.cmake` は、DuckDB ライブラリをソースから自動的にダウンロード、ビルド、リンクする CMake 設定ファイルです。
CMake の `file(DOWNLOAD)` と `execute_process` を使用して依存関係を管理し、`download/` ディレクトリにキャッシュすることで、不要なダウンロードやリビルドを回避します。

DuckDB は、インプロセス型の SQL OLAP データベース管理システムです。C API (`duckdb.h`) と C++ API (`duckdb.hpp`) の両方を提供し、高性能な分析データベースをアプリケーションに直接組み込むことができます。DuckDB は完全な SQL、ACID トランザクション、列指向ストレージ、ベクトル化クエリ実行をサポートしています。

## ファイル情報

| 項目 | 詳細 |
|------|------|
| ソースディレクトリ | `${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB/DuckDB_C_CPP` |
| インストールディレクトリ | `${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB/DuckDB_C_CPP-install` |
| ダウンロード URL | <https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.6.tar.gz> |
| バージョン | 1.5.6 |
| ライセンス | MIT License |

---

## インクルードガード

```cmake
include_guard(GLOBAL)
```

このファイルは `include_guard(GLOBAL)` を使用して、複数回インクルードされても一度だけ実行されることを保証します。

**必要な理由：**

- configure 時の `execute_process` の重複呼び出しを防止
- `target_link_libraries` の重複リンクを防止

---

## ディレクトリ構造

```text
DuckDB-stats/
├── cmake/
│   ├── DuckDB_C_CPP.cmake          # この設定ファイル
│   ├── DuckDB_C_CPPCmake.md        # このドキュメント（英語版）
│   └── DuckDB_C_CPPCmake-jp.md     # このドキュメント（日本語版）
├── download/
│   └── DuckDB/
│       ├── duckdb-1.5.6.tar.gz      # ソースアーカイブ（キャッシュ）
│       ├── DuckDB_C_CPP/            # DuckDB ソース（キャッシュ済み、GitHub からダウンロード）
│       │   └── _build/              # CMake ビルドディレクトリ（ソース内）
│       └── DuckDB_C_CPP-install/    # DuckDB ビルド成果物（lib/, include/）
│           ├── include/
│           │   ├── duckdb.hpp
│           │   ├── duckdb.h
│           │   └── duckdb/
│           │       └── ...
│           └── lib/
│               ├── libduckdb.dylib  (libduckdb.so on Linux)
│               └── libduckdb_static.a
├── src/
│   └── main.cpp
├── build/
└── CMakeLists.txt
```

## 使い方

### CMakeLists.txt への追加

```cmake
# CMakeLists.txt の末尾に DuckDB_C_CPP.cmake をインクルード
include("./cmake/DuckDB_C_CPP.cmake")
```

### ビルド

```bash
mkdir build && cd build
cmake ..
make
```

---

## 処理フロー

### 1. ディレクトリパスの設定

```cmake
set(DUCKDB_DOWNLOAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB)
set(DUCKDB_SOURCE_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP)
set(DUCKDB_INSTALL_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP-install)
set(DUCKDB_BUILD_DIR ${DUCKDB_SOURCE_DIR}/_build)
set(DUCKDB_VERSION "1.5.6")
set(DUCKDB_URL "https://github.com/duckdb/duckdb/archive/refs/tags/v${DUCKDB_VERSION}.tar.gz")
```

### 2. キャッシュチェックと条件付きビルド

```cmake
if(EXISTS ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a)
    message(STATUS "DuckDB already built: ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a")
else()
    # ダウンロード、設定、ビルド、インストール ...
endif()
```

キャッシュロジックは以下のように動作します：

| 条件 | アクション |
|------|-----------|
| `DuckDB_C_CPP-install/lib/libduckdb_static.a` が存在 | すべてスキップ（キャッシュされたビルドを使用） |
| `DuckDB_C_CPP/CMakeLists.txt` が存在（インストールなし） | ダウンロードをスキップ、CMake configure/build/install を実行 |
| 何も存在しない | ダウンロード、展開、CMake configure、ビルド、インストール |

### 3. ダウンロード（必要な場合）

```cmake
file(DOWNLOAD
    ${DUCKDB_URL}
    ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION}.tar.gz
    SHOW_PROGRESS
    STATUS DOWNLOAD_STATUS
)
file(ARCHIVE_EXTRACT
    INPUT ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION}.tar.gz
    DESTINATION ${DUCKDB_DOWNLOAD_DIR}
)
file(RENAME ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION} ${DUCKDB_SOURCE_DIR})
```

- GitHub Releases からダウンロード
- `duckdb-1.5.6/` を `DuckDB_C_CPP/` にリネームしてクリーンなパスに

### 4. 設定、ビルド、インストール（CMake）

```cmake
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DCMAKE_INSTALL_PREFIX=${DUCKDB_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHELL=FALSE
            -DBUILD_UNITTESTS=FALSE
            -DBUILD_BENCHMARKS=FALSE
            -DBUILD_COMPLETE_EXTENSION_SET=FALSE
            -DDISABLE_BUILTIN_EXTENSIONS=FALSE
            -DENABLE_EXTENSION_AUTOLOADING=FALSE
            -DENABLE_EXTENSION_AUTOINSTALL=FALSE
            -DSKIP_EXTENSIONS=parquet
            -DBUILD_EXTENSIONS=json
            -DOVERRIDE_GIT_DESCRIBE=v${DUCKDB_VERSION}-0-g0000000000
            -G Ninja
            ${DUCKDB_SOURCE_DIR}
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR}
)
execute_process(COMMAND ${CMAKE_COMMAND} --build . --config Release -j4
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR})
execute_process(COMMAND ${CMAKE_COMMAND} --install . --config Release
    WORKING_DIRECTORY ${DUCKDB_BUILD_DIR})
```

- `-DBUILD_SHELL=FALSE`: CLI シェルのビルドを無効化
- `-DBUILD_UNITTESTS=FALSE`: テストバイナリのビルドを無効化
- `-DBUILD_BENCHMARKS=FALSE`: ベンチマークスイートのビルドを無効化
- `-DBUILD_COMPLETE_EXTENSION_SET=FALSE`: 全エクステンションのビルドを無効化
- `-DDISABLE_BUILTIN_EXTENSIONS=FALSE`: 選択したエクステンションを `libduckdb` にリンクする
  （`TRUE` にすると DuckDB は `core_functions` だけをリンクし、`BUILD_EXTENSIONS` を無視する）
- `-DENABLE_EXTENSION_AUTOLOADING=FALSE`: エクステンションの自動ロードを無効化
- `-DENABLE_EXTENSION_AUTOINSTALL=FALSE`: エクステンションの自動インストールを無効化
- `-DSKIP_EXTENSIONS=parquet`: Parquet エクステンションを除外
- `-DBUILD_EXTENSIONS=json`: JSON エクステンション（テストで使う `json_extract()` など）をリンクする。
  結果として `json` と `core_functions` がリンクされる
- `-DOVERRIDE_GIT_DESCRIBE=...`: ライブラリが `v1.5.6` を名乗るようにする（拡張のフッタはこの値と一致する必要がある）
- `-G Ninja`: 高速ビルドのため Ninja ジェネレータを使用
- `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`: 位置独立コードを生成
- すべてのステップは CMake の configure 時に実行され、ビルド時ではない

### 5. ライブラリのリンク

```cmake
target_include_directories(${PROJECT_NAME} PRIVATE ${DUCKDB_INSTALL_DIR}/include)

file(GLOB DUCKDB_ALL_LIBS "${DUCKDB_INSTALL_DIR}/lib/libduckdb${CMAKE_SHARED_LIBRARY_SUFFIX}")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${DUCKDB_ALL_LIBS}
    Threads::Threads
)
```

`${PROJECT_NAME}`（ランナー `a.out`）は共有ライブラリ `libduckdb.dylib` / `libduckdb.so` をリンクします。

DuckDB-stats での使われ方:

| ターゲット | DuckDB ヘッダー | DuckDB ライブラリ |
|---|---|---|
| `ext_funcs`（拡張本体） | `SYSTEM` インクルード | **リンクしない**: DuckDB のシンボルは `LOAD` 時にホストから解決される（macOS では `-undefined dynamic_lookup`） |
| `a.out` | 本ファイル経由 | `libduckdb`（共有） |
| `stat_tests` | `SYSTEM` インクルード | `${DUCKDB_ALL_LIBS}` |

拡張は DuckDB の C++ API を使うため、`DUCKDB_VERSION` と同じバージョンの DuckDB にしかロードできません。
`LOAD` 用のメタデータフッタにこのバージョンが記録されます（`cmake/extension_metadata.cmake` を参照）。

---

## DuckDB の主要機能

| 機能 | 説明 |
|------|------|
| インプロセス | ホストプロセス内で動作し、別途サーバーが不要 |
| SQL サポート | JOIN、集約、ウィンドウ関数、CTE を含む完全な SQL サポート |
| ACID トランザクション | 直列化可能な分離レベルによる完全なトランザクションサポート |
| 列指向ストレージ | 分析クエリに最適化された列指向ストレージエンジン |
| ベクトル化実行 | 高性能のためデータをベクター/バッチ単位で処理 |
| C/C++ API | C (`duckdb.h`) と C++ (`duckdb.hpp`) の両方の API が利用可能 |
| プリペアドステートメント | 安全で効率的な繰り返し実行のためのパラメータ化クエリ |
| データインポート/エクスポート | CSV、Parquet、JSON ファイルの読み書き |
| インメモリ＆永続化 | インメモリとファイルベースの両方のデータベースをサポート |
| スレッドセーフ | 並列演算子によるマルチスレッドクエリ実行 |

---

## C++ での使用例

### 基本: インメモリデータベースとクエリ

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    // インメモリデータベースを作成
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    // テーブル作成とデータ挿入
    con.Query("CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER)");
    con.Query("INSERT INTO users VALUES (1, 'Alice', 30)");
    con.Query("INSERT INTO users VALUES (2, 'Bob', 25)");
    con.Query("INSERT INTO users VALUES (3, 'Charlie', 35)");

    // SELECT クエリ
    auto result = con.Query("SELECT * FROM users ORDER BY id");
    result->Print();

    return 0;
}
```

### 集約クエリ

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE sales (product VARCHAR, amount DOUBLE, quantity INTEGER)");
    con.Query("INSERT INTO sales VALUES ('Apple', 1.50, 100)");
    con.Query("INSERT INTO sales VALUES ('Banana', 0.75, 200)");
    con.Query("INSERT INTO sales VALUES ('Apple', 1.50, 150)");
    con.Query("INSERT INTO sales VALUES ('Cherry', 3.00, 50)");

    // GROUP BY と集約
    auto result = con.Query(
        "SELECT product, SUM(amount * quantity) AS revenue, SUM(quantity) AS total_qty "
        "FROM sales GROUP BY product ORDER BY revenue DESC"
    );
    result->Print();

    return 0;
}
```

### プリペアドステートメント

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE employees (id INTEGER, name VARCHAR, salary DOUBLE)");
    con.Query("INSERT INTO employees VALUES (1, 'Alice', 75000)");
    con.Query("INSERT INTO employees VALUES (2, 'Bob', 65000)");
    con.Query("INSERT INTO employees VALUES (3, 'Charlie', 85000)");

    // パラメータ付きプリペアドステートメント
    auto prepared = con.Prepare("SELECT * FROM employees WHERE salary >= $1");
    auto result = prepared->Execute(70000);
    result->Print();

    return 0;
}
```

### 永続化データベース（ファイルベース）

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    // 永続化データベースファイルを作成または開く
    duckdb::DuckDB db("my_database.duckdb");
    duckdb::Connection con(db);

    con.Query("CREATE TABLE IF NOT EXISTS logs (ts TIMESTAMP, message VARCHAR)");
    con.Query("INSERT INTO logs VALUES (NOW(), 'Application started')");

    auto result = con.Query("SELECT * FROM logs ORDER BY ts DESC LIMIT 10");
    result->Print();

    return 0;
}
```

### ウィンドウ関数

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    con.Query("CREATE TABLE scores (student VARCHAR, subject VARCHAR, score INTEGER)");
    con.Query("INSERT INTO scores VALUES ('Alice', 'Math', 90)");
    con.Query("INSERT INTO scores VALUES ('Alice', 'Science', 85)");
    con.Query("INSERT INTO scores VALUES ('Bob', 'Math', 78)");
    con.Query("INSERT INTO scores VALUES ('Bob', 'Science', 92)");
    con.Query("INSERT INTO scores VALUES ('Charlie', 'Math', 95)");
    con.Query("INSERT INTO scores VALUES ('Charlie', 'Science', 88)");

    // ウィンドウ関数: 各科目内でのランク
    auto result = con.Query(
        "SELECT student, subject, score, "
        "RANK() OVER (PARTITION BY subject ORDER BY score DESC) AS rank "
        "FROM scores ORDER BY subject, rank"
    );
    result->Print();

    return 0;
}
```

### 複数接続とトランザクション

```cpp
#include <iostream>
#include "duckdb.hpp"

int main() {
    duckdb::DuckDB db(nullptr);

    // 接続1: テーブル作成とデータ挿入
    duckdb::Connection con1(db);
    con1.Query("CREATE TABLE accounts (id INTEGER, balance DOUBLE)");
    con1.Query("INSERT INTO accounts VALUES (1, 1000.0)");
    con1.Query("INSERT INTO accounts VALUES (2, 2000.0)");

    // 接続2: BEGIN/COMMIT によるトランザクション
    duckdb::Connection con2(db);
    con2.Query("BEGIN TRANSACTION");
    con2.Query("UPDATE accounts SET balance = balance - 500 WHERE id = 1");
    con2.Query("UPDATE accounts SET balance = balance + 500 WHERE id = 2");
    con2.Query("COMMIT");

    auto result = con1.Query("SELECT * FROM accounts ORDER BY id");
    result->Print();

    return 0;
}
```

---

## DuckDB C++ API 主要クラス

| クラス | 説明 |
|--------|------|
| `duckdb::DuckDB` | データベースインスタンス。`nullptr` でインメモリ、ファイルパスで永続化 |
| `duckdb::Connection` | クエリ実行用のデータベース接続 |
| `duckdb::QueryResult` | クエリの結果。`Print()` で表示、または行のイテレーション |
| `duckdb::PreparedStatement` | コンパイル済みのパラメータ化クエリ |
| `duckdb::Appender` | 高スループットの一括データ挿入インターフェース |
| `duckdb::Value` | 型情報付きの単一値を表現 |
| `duckdb::DataChunk` | クエリ結果に使用されるベクトル化データバッチ |

---

## よく使うメソッド

| メソッド | 説明 |
|----------|------|
| `DuckDB(path)` | データベースインスタンス作成（`nullptr` = インメモリ、`"file.db"` = 永続化） |
| `Connection(db)` | データベースへの接続を作成 |
| `con.Query(sql)` | SQL クエリを実行して結果を返す |
| `con.Prepare(sql)` | パラメータ化 SQL ステートメントを準備 |
| `prepared->Execute(args...)` | 引数付きでプリペアドステートメントを実行 |
| `result->Print()` | クエリ結果を標準出力に表示 |
| `result->Fetch()` | 結果から次の DataChunk を取得 |
| `result->GetValue(col, row)` | 結果から特定の値を取得 |
| `Appender(con, table)` | テーブル用の一括アペンダーを作成 |
| `appender.AppendRow(args...)` | 値の行を追加 |
| `appender.Close()` | フラッシュしてアペンダーを閉じる |

---

## DuckDB SQL 型マッピング

| SQL 型 | C++ 型 | 説明 |
|--------|--------|------|
| `BOOLEAN` | `bool` | 真偽値 |
| `TINYINT` | `int8_t` | 8ビット符号付き整数 |
| `SMALLINT` | `int16_t` | 16ビット符号付き整数 |
| `INTEGER` | `int32_t` | 32ビット符号付き整数 |
| `BIGINT` | `int64_t` | 64ビット符号付き整数 |
| `FLOAT` | `float` | 32ビット浮動小数点数 |
| `DOUBLE` | `double` | 64ビット浮動小数点数 |
| `VARCHAR` | `std::string` | 可変長文字列 |
| `DATE` | `duckdb::date_t` | カレンダー日付 |
| `TIMESTAMP` | `duckdb::timestamp_t` | 日時 |
| `BLOB` | `std::string` | バイナリラージオブジェクト |
| `DECIMAL` | `duckdb::hugeint_t` | 固定小数点数 |

---

## CMake ビルドオプションリファレンス

| オプション | デフォルト | 説明 |
|-----------|----------|------|
| `BUILD_SHELL` | TRUE | DuckDB CLI シェルをビルド |
| `BUILD_UNITTESTS` | TRUE | C++ ユニットテストをビルド |
| `BUILD_BENCHMARKS` | FALSE | ベンチマークスイートをビルド |
| `BUILD_COMPLETE_EXTENSION_SET` | TRUE | 全エクステンションをビルド |
| `DISABLE_BUILTIN_EXTENSIONS` | FALSE | 組み込みエクステンションを無効化 |
| `ENABLE_EXTENSION_AUTOLOADING` | FALSE | 実行時にエクステンションを自動ロード |
| `ENABLE_EXTENSION_AUTOINSTALL` | FALSE | エクステンションを自動インストール |
| `BUILD_EXTENSIONS` | (様々) | ビルドするエクステンションのセミコロン区切りリスト |
| `DISABLE_THREADS` | FALSE | マルチスレッドを無効化 |
| `OSX_BUILD_UNIVERSAL` | FALSE | macOS でユニバーサルバイナリをビルド |
| `SMALLER_BINARY` | FALSE | バイナリサイズの最適化 |

---

## トラブルシューティング

### ダウンロードに失敗する

GitHub に接続できない場合は、手動で tarball をダウンロードして配置できます：

```bash
curl -L -o download/DuckDB/duckdb-1.5.6.tar.gz https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.6.tar.gz
```

その後 `cmake ..` を再実行すると、キャッシュされた tarball から展開が行われます。

### configure に失敗する

CMake 3.20+ と Ninja が利用可能であることを確認してください：

```bash
cmake --version
ninja --version
```

macOS では、Xcode Command Line Tools がインストールされていることを確認：

```bash
xcode-select --install
```

Ninja が利用できない場合はインストール：

```bash
brew install ninja
```

### DuckDB をスクラッチからリビルド

フルリビルドを強制するには、インストールディレクトリとソースディレクトリを削除：

```bash
rm -rf download/DuckDB/DuckDB_C_CPP-install download/DuckDB/DuckDB_C_CPP
cd build && cmake ..
```

### リンクエラー: DuckDB シンボルへの未定義参照

`download/DuckDB/DuckDB_C_CPP-install/lib/` に `libduckdb_static.a` が存在することを確認してください。存在しない場合は、インストールディレクトリを削除して cmake を再実行します。

また、システムライブラリ（`pthread`、`dl`、`m`）がリンクされていることを確認してください。静的ライブラリにはこれらが必要です。

### `duckdb.hpp` が見つからない

`target_include_directories` が正しいインストールディレクトリを指していることを確認してください。インストールされたヘッダーは `download/DuckDB/DuckDB_C_CPP-install/include/` にあります。

### ビルドに時間がかかる

DuckDB は大規模なプロジェクトです。ソースからの初回ビルドには、ハードウェアによって 10〜30 分かかる場合があります。以降のビルドでは `download/DuckDB/DuckDB_C_CPP-install/` のキャッシュされた成果物が使用されます。

ビルドを高速化するには：

- Ninja ジェネレータを使用（`-G Ninja` で設定済み）
- 並列ジョブ数を増やす: cmake ファイル内の `-j4` を `-j$(nproc)` に変更
- エクステンションを無効化（設定済み）

---

## 参考資料

- [DuckDB GitHub リポジトリ](https://github.com/duckdb/duckdb)
- [DuckDB ドキュメント](https://duckdb.org/docs/)
- [DuckDB C++ API リファレンス](https://duckdb.org/docs/stable/api/cpp/overview)
- [DuckDB C API リファレンス](https://duckdb.org/docs/stable/api/c/overview)
- [DuckDB ソースからのビルド](https://duckdb.org/docs/stable/dev/building/overview)
- [CMake execute_process ドキュメント](https://cmake.org/cmake/help/latest/command/execute_process.html)
- [CMake file(DOWNLOAD) ドキュメント](https://cmake.org/cmake/help/latest/command/file.html#download)
