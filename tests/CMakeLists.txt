find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME native_audit_inventory COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/check_native_audit.py")
add_test(NAME benchmark_analysis COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/benchmark_analysis_test.py")
if(SRTMUX_BUILD_APPLICATIONS)
  add_test(NAME live_transmit_cli COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/live_transmit_cli_test.py" $<TARGET_FILE:srtmux_live_transmit>)
  set_tests_properties(live_transmit_cli PROPERTIES TIMEOUT 45)
  add_test(NAME live_transmit_linkage COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/live_transmit_linkage_test.py" "${CMAKE_NM}" $<TARGET_FILE:srtmux_live_transmit>)
  if(SRTMUX_BUILD_PROVIDERS)
    add_test(NAME live_transmit_providers COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/live_transmit_provider_test.py" $<TARGET_FILE:srtmux_live_transmit>
      $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision>)
    set_tests_properties(live_transmit_providers PROPERTIES TIMEOUT 180)
  endif()
endif()

function(add_fixture name)
  add_library(${name} MODULE mock_provider.cpp)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_definitions(${name} PRIVATE SRTM_BUILD_PROVIDER ${ARGN})
  set_target_properties(${name} PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  if(APPLE)
    target_link_options(${name} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.macos")
  elseif(UNIX)
    target_link_options(${name} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.elf")
  endif()
  srtmux_warnings(${name})
endfunction()

add_fixture(provider_a FIXTURE_NAME="fixture-a" FIXTURE_VALUE=11)
add_fixture(provider_b FIXTURE_NAME="fixture-b" FIXTURE_VALUE=22)
add_fixture(provider_short FIXTURE_SHORT=1)
add_fixture(provider_null_slot FIXTURE_NULL_SLOT=1)
add_fixture(provider_wrong_abi FIXTURE_WRONG_ABI=1)
add_fixture(provider_bad_descriptor FIXTURE_BAD_DESCRIPTOR=1)
add_fixture(provider_bootstrap_failure FIXTURE_BOOTSTRAP_FAILURE=1)
add_fixture(provider_null_table FIXTURE_NULL_TABLE=1)
add_fixture(provider_future FIXTURE_FUTURE=1)

add_library(provider_no_entry MODULE no_entry.cpp)
target_compile_definitions(provider_no_entry PRIVATE SRTM_BUILD_PROVIDER)
target_include_directories(provider_no_entry PRIVATE "${PROJECT_SOURCE_DIR}/include")
srtmux_warnings(provider_no_entry)

add_executable(module_tests module_tests.cpp)
target_link_libraries(module_tests PRIVATE SrtMux::host Threads::Threads)
srtmux_warnings(module_tests)
foreach(order IN ITEMS normal reversed)
  add_test(NAME module_${order} COMMAND module_tests ${order}
    $<TARGET_FILE:provider_a> $<TARGET_FILE:provider_b>
    $<TARGET_FILE:provider_short> $<TARGET_FILE:provider_null_slot>
    $<TARGET_FILE:provider_wrong_abi> $<TARGET_FILE:provider_bad_descriptor>
    $<TARGET_FILE:provider_bootstrap_failure> $<TARGET_FILE:provider_null_table>
    $<TARGET_FILE:provider_future> $<TARGET_FILE:provider_no_entry>)
endforeach()

add_library(header_checks OBJECT header_c.c header_cpp.cpp)
target_include_directories(header_checks PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/docs/design")
set_target_properties(header_checks PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_compile_features(header_checks PRIVATE cxx_std_20)
srtmux_warnings(header_checks)

if(APPLE OR UNIX)
  add_test(NAME provider_export_surface COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.py" "${CMAKE_NM}"
    $<TARGET_FILE:provider_a> $<TARGET_FILE:provider_b>)
endif()

if(SRTMUX_BUILD_PROVIDERS)
  add_executable(real_provider_tests real_providers.cpp)
  target_link_libraries(real_provider_tests PRIVATE SrtMux::host Threads::Threads)
  srtmux_warnings(real_provider_tests)
  add_executable(transport_tests transport_tests.cpp)
  target_link_libraries(transport_tests PRIVATE SrtMux::host Threads::Threads)
  srtmux_warnings(transport_tests)
  foreach(order IN ITEMS normal reversed)
    add_test(NAME real_providers_${order} COMMAND real_provider_tests
      $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision> ${order})
    add_test(NAME transport_${order} COMMAND transport_tests
      $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision> ${order})
    set_tests_properties(transport_${order} PROPERTIES TIMEOUT 60)
    set_tests_properties(real_providers_${order} PROPERTIES TIMEOUT 180)
  endforeach()
  if(APPLE OR UNIX)
    add_test(NAME real_provider_exports COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.py" "${CMAKE_NM}"
      $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision>)
  endif()
endif()

# Functional smoke checks only: short trials intentionally cannot pass performance gates.
if(SRTMUX_BUILD_PROVIDERS AND SRTMUX_BUILD_BENCHMARKS)
  add_test(NAME benchmark_worker_failure COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmark_worker_test.py"
    $<TARGET_FILE:srtmux_direct_robotweax> $<TARGET_FILE:srtmux_direct_haivision>
    $<TARGET_FILE:srtmux_module_benchmark> $<TARGET_FILE:srtmux_facade_benchmark>
    $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision>)
  set_tests_properties(benchmark_worker_failure PROPERTIES TIMEOUT 180)
  add_test(NAME benchmark_profile_setup COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/benchmarks/compare.py" --build "${PROJECT_BINARY_DIR}"
    --profile-setup --mode file --encryption aes-128-ctr --mib 4 --runs 1
    --warmup-mib 1 --probe-calls 0 --output "${PROJECT_BINARY_DIR}/smoke-profile-setup.json")
  set_tests_properties(benchmark_profile_setup PROPERTIES TIMEOUT 180)
  add_test(NAME benchmark_file_aes128 COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/benchmarks/compare.py" --build "${PROJECT_BINARY_DIR}"
    --mode file --encryption aes-128-ctr --mib 4 --runs 1
    --warmup-mib 1 --probe-calls 0 --output "${PROJECT_BINARY_DIR}/smoke-file_aes128.json")
  set_tests_properties(benchmark_file_aes128 PROPERTIES TIMEOUT 180)
  foreach(profile IN ITEMS plain aes128 tail_direct tail_module tail_facade)
    set(options)
    if(profile STREQUAL "aes128")
      list(APPEND options --encrypted)
    elseif(profile MATCHES "^tail_(.*)$")
      list(APPEND options --faults --api "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME live_${profile} COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/live_delivery_test.py" ${options}
      $<TARGET_FILE:srtmux_live_robotweax> $<TARGET_FILE:srtmux_live_haivision>
      $<TARGET_FILE:srtmux_live_module> $<TARGET_FILE:srtmux_live_facade>
      $<TARGET_FILE:srtmux_robotweax> $<TARGET_FILE:srtmux_haivision>)
    set_tests_properties(live_${profile} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
