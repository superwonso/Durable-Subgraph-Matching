cmake_minimum_required(VERSION 3.16)
project(ours_gopher LANGUAGES CXX)
option(GOPHER_STRICT_WARNINGS "Treat strict GCC/Clang warnings as errors" OFF)
option(
    GOPHER_ERASE_SEARCH_STATISTICS
    "Build the fail-closed statistics-erased overhead control"
    OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(GOPHER_STRICT_WARNINGS AND NOT MSVC)
    add_compile_options(-Wconversion -Wshadow -Werror)
endif()

find_package(Threads REQUIRED)

set(GOPHER_PROFILE_KERNEL_SOURCES
    DagExecutor.cpp
    DirectEvaluator.cpp
    GopherMatcher.cpp)
set(GOPHER_COMMON_SOURCES
    AdaptiveDiagnostics.cpp
    WindowAlgebra.cpp
    RawBitset.cpp
    WindowStore.cpp
    WStoreFile.cpp
    ../ours/v3_canonical_json.cpp
    WindowProgram.cpp
    JIndexExecution.cpp
    BIndexExecution.cpp
    DirectP4Evaluator.cpp
    GopherMatcherV5.cpp
    JIndexSimulation.cpp
    CandidateDag.cpp
    CandidateDagPasses.cpp
    OrderSimulator.cpp
    LabelProjection.cpp
    SearchStatistics.cpp
    query_decomposition.cpp
    temporal_cost_planner.cpp
    TDTree.cpp
    Utils.cpp)
set(GOPHER_CORE_SOURCES
    ${GOPHER_COMMON_SOURCES}
    ${GOPHER_PROFILE_KERNEL_SOURCES})

file(GLOB GOPHER_RECEIPT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
set(GOPHER_RECEIPT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
foreach(source IN LISTS GOPHER_CORE_SOURCES)
    list(APPEND GOPHER_RECEIPT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()
list(APPEND GOPHER_RECEIPT_FILES ${GOPHER_RECEIPT_HEADERS})
list(REMOVE_DUPLICATES GOPHER_RECEIPT_FILES)
list(SORT GOPHER_RECEIPT_FILES)
set(GOPHER_SOURCE_MATERIAL "")
foreach(source IN LISTS GOPHER_RECEIPT_FILES)
    file(SHA256 "${source}" source_sha256)
    file(RELATIVE_PATH source_relative
        "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
    string(APPEND GOPHER_SOURCE_MATERIAL
        "${source_relative}:${source_sha256}\n")
endforeach()
string(SHA256 GOPHER_SOURCE_IDENTITY "${GOPHER_SOURCE_MATERIAL}")

# This identity deliberately excludes GOPHER_ERASE_SEARCH_STATISTICS: the
# overhead oracle accepts exactly that one profile difference and requires
# all ordinary compiler/build controls to match.
set(GOPHER_COMMON_FLAGS_MATERIAL
    "compiler=${CMAKE_CXX_COMPILER}|id=${CMAKE_CXX_COMPILER_ID}|version=${CMAKE_CXX_COMPILER_VERSION}|standard=17|extensions=off|build_type=${CMAKE_BUILD_TYPE}|cxx_flags=${CMAKE_CXX_FLAGS}|strict=${GOPHER_STRICT_WARNINGS}")
string(SHA256 GOPHER_COMMON_FLAGS_IDENTITY
    "${GOPHER_COMMON_FLAGS_MATERIAL}")

function(gopher_configure_compile_target target)
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE
        DSM_GOPHER_SOURCE_IDENTITY="${GOPHER_SOURCE_IDENTITY}"
        DSM_GOPHER_COMMON_FLAGS_IDENTITY="${GOPHER_COMMON_FLAGS_IDENTITY}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The timed statistics-OFF kernel is compiled in its own archive.  Neither the
# product/erased profile switch nor any profile-only source is visible to this
# target; separately configured product and erased builds therefore use the
# same source, flags, and definitions for these objects.
add_library(
    ours_gopher_off_kernel STATIC
    ${GOPHER_PROFILE_KERNEL_SOURCES})
gopher_configure_compile_target(ours_gopher_off_kernel)
target_compile_definitions(
    ours_gopher_off_kernel PRIVATE DSM_GOPHER_OFF_TRANSLATION_UNIT=1)
target_link_libraries(ours_gopher_off_kernel PUBLIC Threads::Threads)

add_library(
    ours_gopher_core STATIC
    ${GOPHER_COMMON_SOURCES}
    ${GOPHER_PROFILE_KERNEL_SOURCES})
target_include_directories(ours_gopher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    ours_gopher_core PUBLIC Threads::Threads ours_gopher_off_kernel)
target_compile_definitions(ours_gopher_core PUBLIC
    DSM_GOPHER_SOURCE_IDENTITY="${GOPHER_SOURCE_IDENTITY}"
    DSM_GOPHER_COMMON_FLAGS_IDENTITY="${GOPHER_COMMON_FLAGS_IDENTITY}")
if(GOPHER_ERASE_SEARCH_STATISTICS)
    target_compile_definitions(
        ours_gopher_core PRIVATE DSM_ERASE_SEARCH_STATISTICS=1)
endif()
gopher_configure_compile_target(ours_gopher_core)

add_executable(td_tree_gopher main.cpp)
target_link_libraries(td_tree_gopher PRIVATE ours_gopher_core)
set_target_properties(td_tree_gopher PROPERTIES OUTPUT_NAME td_tree_bitset_v8)

add_executable(build_wstore build_wstore.cpp)
target_link_libraries(build_wstore PRIVATE ours_gopher_core)
set_target_properties(build_wstore PROPERTIES OUTPUT_NAME build_raw_bitset_store_v8)

include(CTest)
if(GOPHER_ERASE_SEARCH_STATISTICS AND BUILD_TESTING)
    message(FATAL_ERROR
        "The erased overhead-control build is executable-only; configure "
        "with -DBUILD_TESTING=OFF")
endif()
if(BUILD_TESTING)
    # Explicit frozen v8 correctness campaign; never silently run in CTest.
    add_executable(test_v8_bitset tests/test_v8_bitset.cpp)
    target_link_libraries(test_v8_bitset PRIVATE ours_gopher_core)
    add_executable(test_wstore_file tests/test_wstore_file.cpp)
    target_link_libraries(test_wstore_file PRIVATE ours_gopher_core)
    add_executable(test_v6_direct tests/test_v6_direct.cpp)
    target_link_libraries(test_v6_direct PRIVATE ours_gopher_core)
    # The v7 evidence runner invokes this three-arm corpus once explicitly.
    # Do not register it in generic CTest and duplicate the frozen campaign.
    add_executable(test_v7_direct tests/test_v7_direct.cpp)
    target_link_libraries(test_v7_direct PRIVATE ours_gopher_core)
    # The full v5 campaign is invoked once by its evidence runner, not by
    # generic CTest (which would silently duplicate 285,600 native checks).
    add_executable(test_v5_matcher tests/test_v5_matcher.cpp)
    target_link_libraries(test_v5_matcher PRIVATE ours_gopher_core)
    # Conditional production-P8 campaign; explicitly invoked once by its
    # evidence runner, never duplicated by generic CTest.
    add_executable(test_v5_p8 tests/test_v5_p8.cpp)
    target_link_libraries(test_v5_p8 PRIVATE ours_gopher_core)
    add_executable(audit_time_domain tests/audit_time_domain.cpp)
    target_link_libraries(audit_time_domain PRIVATE ours_gopher_core)
    # Standalone algebra oracle: no graph loader/matcher or performance run.
    add_executable(test_window_algebra
        tests/test_window_algebra.cpp WindowAlgebra.cpp)
    target_include_directories(test_window_algebra PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME window_algebra_properties
        COMMAND test_window_algebra
            ${CMAKE_CURRENT_BINARY_DIR}/window_algebra_property_evidence)
    add_executable(test_adaptive_physical tests/test_adaptive_physical.cpp)
    target_link_libraries(test_adaptive_physical PRIVATE ours_gopher_core)
    add_executable(test_adaptive_stage_a tests/test_adaptive_stage_a.cpp)
    target_link_libraries(test_adaptive_stage_a PRIVATE ours_gopher_core)
    add_executable(test_adaptive_diagnostics tests/test_adaptive_diagnostics.cpp)
    target_include_directories(test_adaptive_diagnostics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_adaptive_diagnostics PRIVATE Threads::Threads)
    add_test(NAME adaptive_diagnostics_units COMMAND test_adaptive_diagnostics)
    add_executable(test_operator_passes tests/test_operator_passes.cpp)
    target_link_libraries(test_operator_passes PRIVATE ours_gopher_core)
    add_executable(test_ours
        tests/test_ours.cpp
        query_decomposition.cpp
        TDTree.cpp
        Utils.cpp)
    target_link_libraries(test_ours PRIVATE Threads::Threads)
    add_test(NAME ours_regression COMMAND test_ours)

    add_executable(test_gopher tests/test_gopher.cpp)
    target_link_libraries(test_gopher PRIVATE ours_gopher_core)
    add_test(NAME gopher_exactness COMMAND test_gopher)

    add_executable(test_direct_evaluator tests/test_direct_evaluator.cpp)
    target_link_libraries(test_direct_evaluator PRIVATE ours_gopher_core)
    add_test(NAME gopher_direct_evaluator COMMAND test_direct_evaluator)

    add_executable(test_runtime_key_modes tests/test_runtime_key_modes.cpp)
    target_link_libraries(test_runtime_key_modes PRIVATE ours_gopher_core)
    add_test(NAME gopher_runtime_key_modes COMMAND test_runtime_key_modes)

    add_executable(
        test_statistics_specialization
        tests/test_statistics_specialization.cpp)
    target_link_libraries(
        test_statistics_specialization
        PRIVATE ours_gopher_core)
    add_test(
        NAME gopher_statistics_specialization
        COMMAND test_statistics_specialization)
    if(CMAKE_OBJDUMP AND NOT MSVC)
        add_test(
            NAME gopher_statistics_specialization_objdump
            COMMAND
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_statistics_specialization_objdump.sh
                ${CMAKE_OBJDUMP}
                $<TARGET_FILE:test_statistics_specialization>
                $<TARGET_FILE:ours_gopher_off_kernel>)
    endif()

    add_executable(test_label_projection tests/test_label_projection.cpp)
    target_link_libraries(test_label_projection PRIVATE ours_gopher_core)
    add_test(
        NAME gopher_label_projection
        COMMAND
            test_label_projection
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

    add_executable(
        test_experiment_contract
        tests/test_experiment_contract.cpp)
    target_include_directories(
        test_experiment_contract
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(
        NAME gopher_experiment_contract
        COMMAND test_experiment_contract)

    add_executable(test_temporal_cost_planner
        ../ours/tests/test_temporal_frontier_planner.cpp
        ../ours/temporal_frontier_planner.cpp)
    target_include_directories(test_temporal_cost_planner PRIVATE ../ours)
    add_test(NAME temporal_frontier_planner COMMAND test_temporal_cost_planner)

    add_executable(
        test_parallel_prefix
        tests/test_parallel_prefix.cpp)
    target_link_libraries(
        test_parallel_prefix
        PRIVATE ours_gopher_core)
    add_test(
        NAME gopher_parallel_prefix
        COMMAND test_parallel_prefix)

    add_test(
        NAME gopher_cli
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.sh
            $<TARGET_FILE:td_tree_gopher>)
    add_test(
        NAME gopher_experiment_cli
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_experiment_cli.sh
            $<TARGET_FILE:td_tree_gopher>)
endif()
