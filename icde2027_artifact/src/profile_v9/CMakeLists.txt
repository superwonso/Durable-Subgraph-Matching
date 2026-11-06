cmake_minimum_required(VERSION 3.16)
project(v9_td_build_profile LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
set(S "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(ARTIFACT_ROOT "${S}/../.." ABSOLUTE)
add_library(v9_profile_hooks STATIC "${S}/benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.cpp"
  "${S}/benchmark/e3_e4_adapter_v1/E3E4AdapterCommonV1.cpp")
target_include_directories(v9_profile_hooks PUBLIC "${S}/benchmark/e3_e4_adapter_v1")
target_compile_definitions(v9_profile_hooks PUBLIC
  DSM_ARTIFACT_REPOSITORY_ROOT="${ARTIFACT_ROOT}")
target_compile_options(v9_profile_hooks PRIVATE -O3 -Wall -Wextra -Wpedantic)
add_executable(td_tree_profile_v9 "${S}/td_tree_v8_main.cpp" "${S}/original/TDTree.cpp"
  "${S}/original/Utils.cpp" "${S}/original/query_decomposition.cpp" "${S}/common/LabelProjection.cpp")
target_include_directories(td_tree_profile_v9 PRIVATE "${S}/original" "${S}/common")
target_compile_options(td_tree_profile_v9 PRIVATE -O3 -Wall -Wextra -Wpedantic)
target_link_libraries(td_tree_profile_v9 PRIVATE Threads::Threads v9_profile_hooks)
