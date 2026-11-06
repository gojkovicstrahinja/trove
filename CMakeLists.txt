cmake_minimum_required(VERSION 3.20)
project(trove VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TROVE_BUILD_TESTS "Build the test suite" ON)
option(TROVE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_library(trove_core STATIC
  src/util.cpp
  src/bloom.cpp
  src/wal.cpp
  src/sstable.cpp
)
target_include_directories(trove_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(trove_core PUBLIC /W4 /permissive-)
  if(TROVE_WARNINGS_AS_ERRORS)
    target_compile_options(trove_core PUBLIC /WX)
  endif()
else()
  target_compile_options(trove_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow)
  if(TROVE_WARNINGS_AS_ERRORS)
    target_compile_options(trove_core PUBLIC -Werror)
  endif()
endif()

if(TROVE_BUILD_TESTS)
  enable_testing()
  add_executable(trove_tests
    tests/test_main.cpp
    tests/test_units.cpp
    tests/test_wal.cpp
    tests/test_sstable.cpp
  )
  target_link_libraries(trove_tests PRIVATE trove_core)
  add_test(NAME trove_tests COMMAND trove_tests)
endif()
