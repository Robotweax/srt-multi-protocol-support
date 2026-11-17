find_package(Threads REQUIRED)
add_executable(srtmux_live_transmit main.cpp options.cpp media.cpp haivision/uriparser.cpp)
set_target_properties(srtmux_live_transmit PROPERTIES OUTPUT_NAME srt-live-transmit)
target_link_libraries(srtmux_live_transmit PRIVATE SrtMux::host Threads::Threads)
srtmux_warnings(srtmux_live_transmit)
install(TARGETS srtmux_live_transmit RUNTIME DESTINATION bin COMPONENT SrtMux)
install(FILES README.md DESTINATION share/srtmux COMPONENT SrtMux)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION share/srtmux COMPONENT SrtMux)
install(FILES BUNDLE.md DESTINATION . RENAME README.md COMPONENT SrtMux)
install(FILES haivision/LICENSE haivision/README.md DESTINATION share/srtmux/haivision COMPONENT SrtMux)
if(SRTMUX_BUILD_PROVIDERS)
  add_dependencies(srtmux_live_transmit srtmux_robotweax srtmux_haivision)
  install(TARGETS srtmux_robotweax srtmux_haivision
    LIBRARY DESTINATION lib/srtmux/providers COMPONENT SrtMux
    RUNTIME DESTINATION lib/srtmux/providers COMPONENT SrtMux)
  install(DIRECTORY "${PROJECT_BINARY_DIR}/providers/licenses/" DESTINATION share/srtmux/licenses/providers COMPONENT SrtMux)
  install(FILES "${PROJECT_SOURCE_DIR}/providers/haivision-1.5.7-km-close.patch"
    "${PROJECT_SOURCE_DIR}/providers/haivision-1.5.7-connect-state.patch"
    DESTINATION share/srtmux/licenses/providers COMPONENT SrtMux)
endif()
