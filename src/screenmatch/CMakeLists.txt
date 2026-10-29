# murasaki-screenmatch: the process that compares regions of the screen with references. It reads frames from an
# inherited PipeWire descriptor and talks to the daemon over one inherited socketpair; it runs behind `murasaki-sandbox --profile screenmatch`. The logic is pure (no PipeWire, no I/O).
add_library(murasaki_screenmatch_logic STATIC
  bufferguard.cpp bufferguard.h
  exitcodes.h
  protocol.cpp protocol.h
  framesource.h
  plan.cpp plan.h
  pwsource.cpp pwsource.h
  session.cpp session.h)
target_include_directories(murasaki_screenmatch_logic PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(murasaki_screenmatch_logic PUBLIC murasaki_core Qt6::Core)
target_compile_features(murasaki_screenmatch_logic PUBLIC cxx_std_20)
target_compile_options(murasaki_screenmatch_logic PUBLIC -Wno-missing-field-initializers)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SCREENMATCH_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
endif()
if(SCREENMATCH_PIPEWIRE_FOUND)
  target_compile_definitions(murasaki_screenmatch_logic PUBLIC MURASAKI_HAVE_PIPEWIRE)
  target_link_libraries(murasaki_screenmatch_logic PUBLIC PkgConfig::SCREENMATCH_PIPEWIRE)
else()
  message(STATUS "murasaki-screenmatch: PipeWire not found, the helper cannot read the screen")
endif()

add_executable(murasaki-screenmatch main.cpp)
target_link_libraries(murasaki-screenmatch PRIVATE murasaki_screenmatch_logic murasaki_landlock)
target_compile_definitions(murasaki-screenmatch PRIVATE MURASAKI_VERSION="${PROJECT_VERSION}")
install(TARGETS murasaki-screenmatch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
