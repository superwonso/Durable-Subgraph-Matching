cmake_minimum_required(VERSION 3.16)
project(v8_baseline_adapters LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
set(S "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(ARTIFACT_ROOT "${S}/../.." ABSOLUTE)
add_library(v8_runtime_hooks STATIC
    "${S}/benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.cpp"
    "${S}/benchmark/e3_e4_adapter_v1/E3E4AdapterCommonV1.cpp")
target_include_directories(v8_runtime_hooks PUBLIC "${S}/benchmark/e3_e4_adapter_v1")
target_compile_definitions(v8_runtime_hooks PUBLIC
    DSM_ARTIFACT_REPOSITORY_ROOT="${ARTIFACT_ROOT}")
target_compile_options(v8_runtime_hooks PRIVATE -O3 -Wall -Wextra -Wpedantic)
add_executable(td_tree_baseline_v8 "${S}/td_tree_v8_main.cpp"
    "${S}/original/TDTree.cpp" "${S}/original/Utils.cpp"
    "${S}/original/query_decomposition.cpp" "${S}/common/LabelProjection.cpp")
add_executable(tinla_baseline_v8 "${S}/tinla_v8_main.cpp"
    "${S}/paper_baselines/tinla/tinla.cpp" "${S}/common/LabelProjection.cpp")
foreach(target td_tree_baseline_v8 tinla_baseline_v8)
    target_include_directories(${target} PRIVATE "${S}/original" "${S}/common")
    target_compile_options(${target} PRIVATE -O3 -Wall -Wextra -Wpedantic)
    target_link_libraries(${target} PRIVATE Threads::Threads v8_runtime_hooks)
endforeach()
