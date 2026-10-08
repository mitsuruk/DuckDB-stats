# =============================================================================
# Loadable extension metadata footer
#
# A DuckDB extension is a plain shared library with a 534-byte footer appended.
# DuckDB reads that footer BEFORE dlopen'ing the library and refuses to load it
# if the fields do not match the host. Without the footer, the .dylib is not an
# extension at all and `LOAD` fails with "not a valid DuckDB extension file".
#
# Layout (see duckdb/scripts/append_metadata.cmake, which this file mirrors):
#
#   [ section id 0                                          =   1 byte  ]
#   [ LEB128 section length (531) + name length (16)         =   3 bytes ]
#   [ section name "duckdb_signature"                        =  16 bytes ]
#   [ LEB128 payload length (512)                            =   2 bytes ]
#   [ 8 metadata fields x 32 bytes, stored in REVERSE order  = 256 bytes ]
#   [ signature placeholder, 0x00 filled                     = 256 bytes ]
#                                                              ---------
#                                                              534 bytes
#
# Fields we populate:
#
#   FIELD1 = "4"                metadata format version
#   FIELD2 = platform           e.g. osx_arm64 - must match the host exactly
#   FIELD3 = version            SEE BELOW
#   FIELD4 = extension version  free-form, ours
#   FIELD5 = ABI type           "CPP" for a C++ API extension
#
# FIELD3 is overloaded and this is the single easiest thing to get wrong:
#
#   ABI_TYPE=CPP or C_STRUCT_UNSTABLE -> the DuckDB version, matched EXACTLY
#   ABI_TYPE=C_STRUCT                 -> the C API version, treated as a MINIMUM
#
# (verified in duckdb/src/main/extension.cpp, ExtensionABIType handling)
#
# This project uses the C++ API (it needs the custom window aggregate
# callbacks, which the C API does not expose), so FIELD3 holds the exact
# DuckDB version the extension was compiled against, e.g. v1.5.6. The binary
# loads ONLY into that DuckDB version.
#
# The header is a WebAssembly custom section purely so that the very same
# footer works for the wasm builds; native loaders just read from the end.
#
# The signature area stays zero-filled: this extension is unsigned, so it needs
# `duckdb -unsigned` or SET allow_unsigned_extensions=true to load.
#
# Usage (script mode, invoked from add_custom_command):
#   cmake -DEXTENSION=<file> -DPLATFORM=<str> -DVERSION_FIELD=<str> \
#         -DEXTENSION_VERSION=<str> -DABI_TYPE=CPP -DNULL_FILE=<file> \
#         -P cmake/extension_metadata.cmake
#
# License: MIT License (this cmake file).
# Note: derived from duckdb/scripts/append_metadata.cmake (MIT License).
# =============================================================================

cmake_minimum_required(VERSION 3.20)

set(EXTENSION "" CACHE PATH "Path of the extension binary to append metadata to")
set(NULL_FILE "" CACHE PATH "Path to a file containing exactly one 0x00 byte")
set(PLATFORM "" CACHE STRING "Metadata field 2: duckdb platform, e.g. osx_arm64")
set(VERSION_FIELD "" CACHE STRING "Metadata field 3: DuckDB version for CPP")
set(EXTENSION_VERSION "" CACHE STRING "Metadata field 4: this extension's version")
set(ABI_TYPE "CPP" CACHE STRING "Metadata field 5: extension ABI type")

if(NOT EXISTS "${NULL_FILE}")
    message(FATAL_ERROR "NULL_FILE not found: ${NULL_FILE}")
endif()
if(NOT EXISTS "${EXTENSION}")
    message(FATAL_ERROR "EXTENSION not found: ${EXTENSION}")
endif()
if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM must be set (e.g. osx_arm64)")
endif()

# CMake cannot express a literal NUL in a string, so it is read from a file
# that contains exactly one 0x00 byte.
file(READ "${NULL_FILE}" EMPTY_BYTE)
string(LENGTH "${EMPTY_BYTE}" EMPTY_BYTE_LEN)
if(NOT EMPTY_BYTE_LEN EQUAL 1)
    message(FATAL_ERROR "NULL_FILE must contain exactly 1 byte, got ${EMPTY_BYTE_LEN}")
endif()

string(REPEAT "${EMPTY_BYTE}" 32 EMPTY_32)
string(REPEAT "${EMPTY_BYTE}" 256 EMPTY_256)

# --- WebAssembly custom-section header ------------------------------------
# section id 0 = custom section
string(APPEND CUSTOM_SECTION "${EMPTY_BYTE}")

# LEB128 section length 531 = 1 + 16 + 2 + 8*32 + 256, then name length 16
string(ASCII 147 4 16 CUSTOM_SECTION_2)
string(APPEND CUSTOM_SECTION "${CUSTOM_SECTION_2}")

# section name, exactly 16 bytes
string(APPEND CUSTOM_SECTION "duckdb_signature")

# LEB128 payload length 512 = 8*32 metadata + 256 signature
string(ASCII 128 4 CUSTOM_SECTION_3)
string(APPEND CUSTOM_SECTION "${CUSTOM_SECTION_3}")

# --- Metadata fields, each padded to exactly 32 bytes ----------------------
string(SUBSTRING "4${EMPTY_32}" 0 32 METADATA1)
string(SUBSTRING "${PLATFORM}${EMPTY_32}" 0 32 METADATA2)
string(SUBSTRING "${VERSION_FIELD}${EMPTY_32}" 0 32 METADATA3)
string(SUBSTRING "${EXTENSION_VERSION}${EMPTY_32}" 0 32 METADATA4)
string(SUBSTRING "${ABI_TYPE}${EMPTY_32}" 0 32 METADATA5)
string(SUBSTRING "${EMPTY_32}" 0 32 METADATA6)
string(SUBSTRING "${EMPTY_32}" 0 32 METADATA7)
string(SUBSTRING "${EMPTY_32}" 0 32 METADATA8)

# Fields are stored in reverse order, so field 1 ends up closest to the
# signature block and the loader can walk backwards from the end of the file.
string(APPEND CUSTOM_SECTION "${METADATA8}")
string(APPEND CUSTOM_SECTION "${METADATA7}")
string(APPEND CUSTOM_SECTION "${METADATA6}")
string(APPEND CUSTOM_SECTION "${METADATA5}")
string(APPEND CUSTOM_SECTION "${METADATA4}")
string(APPEND CUSTOM_SECTION "${METADATA3}")
string(APPEND CUSTOM_SECTION "${METADATA2}")
string(APPEND CUSTOM_SECTION "${METADATA1}")

# --- Signature placeholder (unsigned extension) ---------------------------
string(APPEND CUSTOM_SECTION "${EMPTY_256}")

file(APPEND "${EXTENSION}" "${CUSTOM_SECTION}")

message(STATUS "Appended extension metadata: platform=${PLATFORM} abi=${ABI_TYPE} version=${VERSION_FIELD}")
