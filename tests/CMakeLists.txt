find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

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
