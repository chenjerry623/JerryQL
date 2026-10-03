cmake_minimum_required(VERSION 3.16)
project(JerryQL LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(JERRYQL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JERRYQL_WERROR "Treat compiler warnings as errors" OFF)

add_library(jerryql_core
  src/ast.cpp
  src/catalog.cpp
  src/database.cpp
  src/executor.cpp
  src/expr_eval.cpp
  src/lexer.cpp
  src/memory_store.cpp
  src/parser.cpp
  src/planner.cpp
  src/shell.cpp
  src/storage/btree.cpp
  src/storage/btree_store.cpp
  src/storage/buffer_pool.cpp
  src/storage/file.cpp
  src/storage/pager.cpp
  src/storage/row_codec.cpp
  src/storage/wal.cpp
  src/value.cpp
)
target_include_directories(jerryql_core PUBLIC src)
target_compile_options(jerryql_core PUBLIC -Wall -Wextra -Wpedantic)
if(JERRYQL_WERROR)
  target_compile_options(jerryql_core PUBLIC -Werror)
endif()
if(JERRYQL_SANITIZE)
  target_compile_options(jerryql_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer
                                             -fno-sanitize-recover=all)
  target_link_options(jerryql_core PUBLIC -fsanitize=address,undefined)
endif()

if(EMSCRIPTEN)
  # Browser build: the engine plus a tiny C API, loaded by web/index.html.
  # SQL errors are C++ exceptions, which Emscripten disables by default.
  target_compile_options(jerryql_core PUBLIC -fexceptions)
  add_executable(jerryql_web web/wasm_api.cpp)
  target_link_libraries(jerryql_web PRIVATE jerryql_core)
  set_target_properties(jerryql_web PROPERTIES OUTPUT_NAME jerryql)
  target_link_options(jerryql_web PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=createJerryQL
    -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=_jerryql_run,_jerryql_reset,_jerryql_free,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString
    -sSINGLE_FILE=1
    -sSINGLE_FILE_BINARY_ENCODE=0
    -fexceptions
  )
  return()
endif()

add_executable(jerryql src/main.cpp)
target_link_libraries(jerryql PRIVATE jerryql_core)

add_executable(jerryql_bench bench/bench_storage.cpp)
target_link_libraries(jerryql_bench PRIVATE jerryql_core)

find_package(SQLite3)
if(SQLite3_FOUND)
  add_executable(jerryql_vs_sqlite bench/bench_vs_sqlite.cpp)
  target_link_libraries(jerryql_vs_sqlite PRIVATE jerryql_core SQLite::SQLite3)
endif()

add_executable(jerryql_crashtest tools/crash_test.cpp tools/bank_workload.cpp)
target_link_libraries(jerryql_crashtest PRIVATE jerryql_core)

enable_testing()

add_executable(jerryql_tests
  tests/test_main.cpp
  tests/test_lexer.cpp
  tests/test_parser.cpp
  tests/test_planner.cpp
  tests/test_engine.cpp
  tests/test_btree.cpp
  tests/test_wal.cpp
)
target_link_libraries(jerryql_tests PRIVATE jerryql_core)
add_test(NAME unit_tests COMMAND jerryql_tests)

# Golden-file tests: run each tests/sql/*.sql through the shell with --echo
# and compare against the matching .expected file.
file(GLOB GOLDEN_SQL CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/sql/*.sql)
foreach(sql_file ${GOLDEN_SQL})
  get_filename_component(name ${sql_file} NAME_WE)
  add_test(NAME golden_${name}
    COMMAND ${CMAKE_COMMAND}
      -DJERRYQL=$<TARGET_FILE:jerryql>
      -DSQL=${sql_file}
      -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/sql/${name}.expected
      -P ${CMAKE_SOURCE_DIR}/tests/run_golden.cmake)
endforeach()
