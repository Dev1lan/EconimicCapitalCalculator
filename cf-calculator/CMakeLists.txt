cmake_minimum_required(VERSION 3.20)
project(cashflow_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CF_BUILD_TESTS "Build unit tests for CashFlowEngine" ON)

option(CF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(cf_engine STATIC
    src/date_utils.cpp
    src/cf_calculator.cpp
)

target_include_directories(cf_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(cf_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(cf_engine PRIVATE -Wall -Wextra -Wpedantic)
    if(CF_ENABLE_SANITIZERS)
        target_compile_options(cf_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(cf_engine PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(CF_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(cf_tests
        tests/test_cf.cpp
    )

    target_link_libraries(cf_tests
        PRIVATE
            cf_engine
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(cf_tests)
endif()
