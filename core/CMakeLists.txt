add_library(cfcore STATIC
  src/cashflow.cpp
  src/csv_io.cpp
  src/date.cpp
  src/engine.cpp
  src/finance.cpp
  src/schedule.cpp
  src/validate.cpp
)
target_include_directories(cfcore PUBLIC include PRIVATE src)

# Настройки вычислений наследуются всеми, кто линкуется с ядром, включая тесты.
add_library(cfcore_options INTERFACE)
if(MSVC)
  target_compile_options(cfcore_options INTERFACE /W4 /fp:precise)
else()
  # -ffp-contract=off: компилятор не объединяет a*b+c в одну операцию,
  # иначе результат на ARM и x86 будет отличаться.
  # -fno-fast-math: без него пропадает компенсация в суммировании Ньюмана.
  target_compile_options(cfcore_options INTERFACE
    -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
  if(CFCORE_SANITIZE)
    target_compile_options(cfcore_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cfcore_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()
target_link_libraries(cfcore PUBLIC cfcore_options)

# Сначала ищем установленный GoogleTest, иначе скачиваем.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(cfcore_cli cli/main.cpp)
set_target_properties(cfcore_cli PROPERTIES OUTPUT_NAME cfcore)
target_link_libraries(cfcore_cli PRIVATE cfcore)

include(GoogleTest)
add_executable(cfcore_tests
  tests/test_cashflow.cpp
  tests/test_contracts.cpp
  tests/test_csv.cpp
  tests/test_date.cpp
  tests/test_engine.cpp
  tests/test_finance.cpp
  tests/test_golden.cpp
  tests/test_validate.cpp
)
target_link_libraries(cfcore_tests PRIVATE cfcore GTest::gtest_main)
target_compile_definitions(cfcore_tests PRIVATE
  CFCORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  CFCORE_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/data/golden")
gtest_discover_tests(cfcore_tests)
