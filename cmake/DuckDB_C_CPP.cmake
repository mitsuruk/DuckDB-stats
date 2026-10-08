# =============================================================================
# DuckDB C/C++ CMake configuration
#
# This file configures DuckDB library for the project.
# DuckDB is an in-process SQL OLAP database management system.
# It provides both a C API (duckdb.h) and a C++ API (duckdb.hpp)
# for embedding an analytical database directly into applications.
#
# Download directory: ${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB_C_CPP
# Install directory:  ${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB_C_CPP-install
#
# - If DuckDB_C_CPP-install/lib/libduckdb_static.a already exists, skip download and build.
# - If download/DuckDB_C_CPP/CMakeLists.txt already exists, skip download (reuse cache).
# - Otherwise, download from GitHub, configure with CMake, build, and install.
#
# License: MIT License (this cmake file)
# Note: DuckDB library itself is licensed under the MIT License.
# =============================================================================

include_guard(GLOBAL)

message(STATUS "===============================================================")
message(STATUS "DuckDB C/C++ configuration:")

# Path to download/install directories
set(DUCKDB_DOWNLOAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/download/DuckDB)
set(DUCKDB_SOURCE_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP)
set(DUCKDB_INSTALL_DIR ${DUCKDB_DOWNLOAD_DIR}/DuckDB_C_CPP-install)
set(DUCKDB_BUILD_DIR ${DUCKDB_SOURCE_DIR}/_build)
set(DUCKDB_VERSION "1.5.6")
set(DUCKDB_URL "https://github.com/duckdb/duckdb/archive/refs/tags/v${DUCKDB_VERSION}.tar.gz")

message(STATUS "DUCKDB_SOURCE_DIR  = ${DUCKDB_SOURCE_DIR}")
message(STATUS "DUCKDB_INSTALL_DIR = ${DUCKDB_INSTALL_DIR}")

# =============================================================================
# DuckDB Library: Download, Build, and Install (cached in download/ directory)
# =============================================================================
if(EXISTS ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a)
    message(STATUS "DuckDB already built: ${DUCKDB_INSTALL_DIR}/lib/libduckdb_static.a")
else()
    # --- Download (skip if source already cached) ---
    if(EXISTS ${DUCKDB_SOURCE_DIR}/CMakeLists.txt)
        message(STATUS "DuckDB source already cached: ${DUCKDB_SOURCE_DIR}")
    else()
        set(DUCKDB_ARCHIVE ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION}.tar.gz)
        set(DUCKDB_URLS
            "https://github.com/duckdb/duckdb/archive/refs/tags/v${DUCKDB_VERSION}.tar.gz"
        )

        set(DUCKDB_DOWNLOADED FALSE)
        foreach(URL ${DUCKDB_URLS})
            message(STATUS "Downloading DuckDB ${DUCKDB_VERSION} from ${URL} ...")
            file(DOWNLOAD
                ${URL}
                ${DUCKDB_ARCHIVE}
                SHOW_PROGRESS
                TIMEOUT 300
                INACTIVITY_TIMEOUT 60
                STATUS DOWNLOAD_STATUS
            )
            list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RESULT)
            if(DOWNLOAD_RESULT EQUAL 0)
                set(DUCKDB_DOWNLOADED TRUE)
                break()
            else()
                list(GET DOWNLOAD_STATUS 1 DOWNLOAD_ERROR)
                message(WARNING "Download from ${URL} failed: ${DOWNLOAD_ERROR}")
                file(REMOVE ${DUCKDB_ARCHIVE})
            endif()
        endforeach()

        if(NOT DUCKDB_DOWNLOADED)
            message(FATAL_ERROR
                "DuckDB download failed.\n"
                "You can manually download and place the file:\n"
                "  curl -L -o download/duckdb-${DUCKDB_VERSION}.tar.gz ${DUCKDB_URL}\n"
                "Then re-run cmake."
            )
        endif()

        message(STATUS "Extracting DuckDB ${DUCKDB_VERSION} ...")
        file(ARCHIVE_EXTRACT
            INPUT ${DUCKDB_ARCHIVE}
            DESTINATION ${DUCKDB_DOWNLOAD_DIR}
        )
        # Rename extracted directory (duckdb-1.4.4 -> DuckDB_C_CPP)
        file(RENAME ${DUCKDB_DOWNLOAD_DIR}/duckdb-${DUCKDB_VERSION} ${DUCKDB_SOURCE_DIR})

        message(STATUS "DuckDB source cached: ${DUCKDB_SOURCE_DIR}")
    endif()

    # --- Configure (CMake) ---
    message(STATUS "Configuring DuckDB with CMake ...")
    file(MAKE_DIRECTORY ${DUCKDB_BUILD_DIR})
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
        RESULT_VARIABLE DUCKDB_CONFIGURE_RESULT
    )
    if(NOT DUCKDB_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "DuckDB CMake configure failed")
    endif()

    # --- Build ---
    message(STATUS "Building DuckDB (this may take a while) ...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build . --config Release -j4
        WORKING_DIRECTORY ${DUCKDB_BUILD_DIR}
        RESULT_VARIABLE DUCKDB_BUILD_RESULT
    )
    if(NOT DUCKDB_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "DuckDB build failed")
    endif()

    # --- Install ---
    message(STATUS "Installing DuckDB to ${DUCKDB_INSTALL_DIR} ...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --install . --config Release
        WORKING_DIRECTORY ${DUCKDB_BUILD_DIR}
        RESULT_VARIABLE DUCKDB_INSTALL_RESULT
    )
    if(NOT DUCKDB_INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "DuckDB install failed")
    endif()

    message(STATUS "DuckDB ${DUCKDB_VERSION} built and installed successfully")
endif()

# =============================================================================
# DuckDB Library Configuration
# =============================================================================
# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${DUCKDB_INSTALL_DIR}/include)

# Link the static library with all internal dependencies.
# DuckDB's static library depends on its bundled third-party libraries
# (fmt, pg_query, re2, miniz, utf8proc, hyperloglog, fsst, etc.)
# which must also be linked.
# file(GLOB DUCKDB_ALL_LIBS "${DUCKDB_INSTALL_DIR}/lib/*.a")
# Pick the single DuckDB shared library, cross-platform:
# CMAKE_SHARED_LIBRARY_SUFFIX is .dylib on macOS and .so on Linux.
file(GLOB DUCKDB_ALL_LIBS "${DUCKDB_INSTALL_DIR}/lib/libduckdb${CMAKE_SHARED_LIBRARY_SUFFIX}")


target_link_libraries(${PROJECT_NAME} PRIVATE
    ${DUCKDB_ALL_LIBS}
    Threads::Threads
)
find_package(Threads REQUIRED)

message(STATUS "DuckDB linked to ${PROJECT_NAME}")
message(STATUS "===============================================================")
