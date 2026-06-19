cmake_minimum_required(VERSION 3.16)

project(aspira_trading VERSION 1.0.0 LANGUAGES C CXX)

# Compiler flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
# -O3: aggressive loop unrolling, vectorization, inlining
# -march=native: enable CPU-specific instructions (AVX2, BMI2, etc.)
# -flto: Link-Time Optimization — cross-TU inlining for hot functions
#         (ring_buffer, mempool, feed_handler benefit significantly)
# -DNDEBUG: disable assertions in release
#
# For C++: -fno-exceptions -fno-rtti (the codebase uses neither)
#   Reduces binary size and improves code generation for virtual-free code.
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -fno-exceptions -fno-rtti -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")

# Enable interprocedural optimization (LTO) for all configurations
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)

# Common flags
add_compile_options(-Wall -Wextra -Wpedantic)

# Optional: Profile-Guided Optimization (PGO)
# Usage:
#   1. cmake .. -DCMAKE_BUILD_TYPE=Release -DPGO_GENERATE=ON
#   2. Run the benchmark workload
#   3. cmake .. -DCMAKE_BUILD_TYPE=Release -DPGO_USE=ON
option(PGO_GENERATE "Generate PGO profile data" OFF)
option(PGO_USE "Use PGO profile data for optimization" OFF)
if(PGO_GENERATE)
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
    message(STATUS "PGO: Profile generation enabled")
elseif(PGO_USE)
    add_compile_options(-fprofile-use)
    add_link_options(-fprofile-use)
    message(STATUS "PGO: Profile-guided optimization enabled")
endif()

# Enable latency measurement by default. Add -DTRADING_NO_MEASURE_LATENCY
# to CXXFLAGS to disable for absolute minimum latency in production.
add_compile_definitions(TRADING_MEASURE_LATENCY)

# Include directories
# Use target-scoped includes (modern CMake best practice)
add_library(trading_common INTERFACE)
target_include_directories(trading_common INTERFACE ${CMAKE_SOURCE_DIR}/include)

# Source files
set(C_SOURCES
    src/ring_buffer.c
    src/memory_pool.c
    src/logger.c
    src/feed_handler.c
    src/execution_gateway.c
    src/journal.c
)

set(CXX_SOURCES
    src/order_book.cpp
    src/risk_engine.cpp
    src/event_pipeline.cpp
    src/main.cpp
)

# Main executable
add_executable(trading_engine
    ${C_SOURCES}
    ${CXX_SOURCES}
)

target_link_libraries(trading_engine PRIVATE
    trading_common
    pthread
    rt
)

# Tests
option(BUILD_TESTS "Build test executables" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_ring_buffer
        tests/test_ring_buffer.c
        src/ring_buffer.c
    )
    target_link_libraries(test_ring_buffer PRIVATE trading_common pthread rt)

    add_executable(test_memory_pool
        tests/test_memory_pool.c
        src/memory_pool.c
    )
    target_link_libraries(test_memory_pool PRIVATE trading_common pthread rt)

    add_executable(test_order_book
        tests/test_order_book.cpp
        src/order_book.cpp
    )
    target_link_libraries(test_order_book PRIVATE trading_common pthread rt)

    add_executable(test_risk_engine
        tests/test_risk_engine.cpp
        src/risk_engine.cpp
    )
    target_link_libraries(test_risk_engine PRIVATE trading_common pthread rt)

    add_test(NAME ring_buffer_test COMMAND test_ring_buffer)
    add_test(NAME memory_pool_test COMMAND test_memory_pool)
    add_test(NAME order_book_test COMMAND test_order_book)
    add_test(NAME risk_engine_test COMMAND test_risk_engine)
endif()

# Install
install(TARGETS trading_engine DESTINATION bin)
