# Qt-Core-only logic, always built and tested on CI.
add_library(murasaki_session_logic STATIC
  audiospectrum.cpp audiospectrum.h
  buspropertywatcher.cpp buspropertywatcher.h
  compositorprofile.cpp compositorprofile.h
  cursorrouter.cpp cursorrouter.h
  i3ipc.cpp i3ipc.h
  outputhealth.cpp outputhealth.h
  pausepolicy.cpp pausepolicy.h
  playbackpolicy.cpp playbackpolicy.h
  sessionstate.cpp sessionstate.h
  spansync.cpp spansync.h
  toplevelstate.cpp toplevelstate.h)
target_include_directories(murasaki_session_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_session_logic PUBLIC murasaki_appclass Qt6::Core Qt6::DBus)

if(MURASAKI_SURFACE)
  add_library(murasaki_session_monitors STATIC
    hyprlandmonitor.cpp hyprlandmonitor.h
    idlesource.h
    kwinmonitor.cpp kwinmonitor.h
    sessionmonitor.h
    swaymonitor.cpp swaymonitor.h)
  target_link_libraries(murasaki_session_monitors PUBLIC murasaki_session_logic Qt6::DBus Qt6::Network)

  # wlr-foreign-toplevel-management: which windows are fullscreen or maximized on which output, and their app ids, on
  # any compositor that offers it (labwc, river, wayfire, niri, ...). Needs libwayland-client and wayland-scanner.
  find_program(MURASAKI_WAYLAND_SCANNER wayland-scanner)
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
  if(MURASAKI_WAYLAND_SCANNER AND WAYLAND_CLIENT_FOUND)
    enable_language(C) # the scanner's protocol code
    set(_wlr_xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
    set(_wlr_gen ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${_wlr_gen})
    add_custom_command(OUTPUT ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
      COMMAND ${MURASAKI_WAYLAND_SCANNER} client-header ${_wlr_xml}
              ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
      DEPENDS ${_wlr_xml})
    add_custom_command(OUTPUT ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
      COMMAND ${MURASAKI_WAYLAND_SCANNER} private-code ${_wlr_xml}
              ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
      DEPENDS ${_wlr_xml})
    add_library(murasaki_wlr_protocol STATIC
      ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
      ${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-protocol.c)
    target_include_directories(murasaki_wlr_protocol PUBLIC ${_wlr_gen})
    target_link_libraries(murasaki_wlr_protocol PUBLIC PkgConfig::WAYLAND_CLIENT)
    set_target_properties(murasaki_wlr_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_source_files_properties(${_wlr_gen}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
                                PROPERTIES COMPILE_OPTIONS "-w") # generated code: not ours to warn about
    target_sources(murasaki_session_monitors PRIVATE foreigntoplevelmonitor.cpp foreigntoplevelmonitor.h)
    target_link_libraries(murasaki_session_monitors PUBLIC murasaki_wlr_protocol)
    target_compile_definitions(murasaki_session_monitors PUBLIC MURASAKI_HAVE_FOREIGN_TOPLEVEL)
    # ext-idle-notify-v1: whether the user is away, on compositors that never set logind's IdleHint (Plasma). Optional like the monitor above.
    set(_idle_xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/ext-idle-notify-v1.xml)
    add_custom_command(OUTPUT ${_wlr_gen}/ext-idle-notify-v1-client-protocol.h
      COMMAND ${MURASAKI_WAYLAND_SCANNER} client-header ${_idle_xml} ${_wlr_gen}/ext-idle-notify-v1-client-protocol.h
      DEPENDS ${_idle_xml})
    add_custom_command(OUTPUT ${_wlr_gen}/ext-idle-notify-v1-protocol.c
      COMMAND ${MURASAKI_WAYLAND_SCANNER} private-code ${_idle_xml} ${_wlr_gen}/ext-idle-notify-v1-protocol.c
      DEPENDS ${_idle_xml})
    add_library(murasaki_idle_protocol STATIC ${_wlr_gen}/ext-idle-notify-v1-client-protocol.h ${_wlr_gen}/ext-idle-notify-v1-protocol.c)
    target_include_directories(murasaki_idle_protocol PUBLIC ${_wlr_gen})
    target_link_libraries(murasaki_idle_protocol PUBLIC PkgConfig::WAYLAND_CLIENT)
    set_target_properties(murasaki_idle_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_source_files_properties(${_wlr_gen}/ext-idle-notify-v1-protocol.c PROPERTIES COMPILE_OPTIONS "-w")
    target_sources(murasaki_session_monitors PRIVATE waylandidlemonitor.cpp waylandidlemonitor.h)
    target_link_libraries(murasaki_session_monitors PUBLIC murasaki_idle_protocol)
    target_compile_definitions(murasaki_session_monitors PUBLIC MURASAKI_HAVE_IDLE_NOTIFY)
  else()
    message(STATUS "libwayland-client or wayland-scanner not found: no wlr-foreign-toplevel-management monitor, no idle time")
  endif()
  qt_add_resources(murasaki_session_monitors kwin_monitor_script PREFIX /murasaki FILES kwin-monitor.js kwin-cursor.js)

  # Everything but main(): the controller, its sinks and the D-Bus facade, so the tests can drive them.
  add_library(murasaki_surface_core STATIC
    audiocapture.cpp audiocapture.h
    heaptuning.cpp heaptuning.h
    hostoutput.cpp hostoutput.h
    outputsink.h
    outputsurface.cpp outputsurface.h
    surfacecontroller.cpp surfacecontroller.h
    surfaceservice.cpp surfaceservice.h
    wallpaperroot.cpp wallpaperroot.h)
  target_include_directories(murasaki_surface_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(murasaki_surface_core PUBLIC
    murasaki_session_monitors murasaki_render murasaki_core murasaki_bridge_producer
    Qt6::DBus Qt6::Quick Qt6::WebEngineQuick)
  if(TARGET murasaki_x11)
    target_link_libraries(murasaki_surface_core PUBLIC murasaki_x11)
    target_compile_definitions(murasaki_surface_core PUBLIC MURASAKI_HAVE_X11)
  endif()
  if(PIPEWIRE_FOUND)
    target_link_libraries(murasaki_surface_core PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(murasaki_surface_core PRIVATE MURASAKI_HAVE_PIPEWIRE)
  else()
    message(STATUS "libpipewire-0.3 not found: audio-reactive wallpapers will get silence")
  endif()
  if(LayerShellQt_FOUND)
    target_link_libraries(murasaki_surface_core PUBLIC LayerShellQt::Interface)
    if(LayerShellQt_VERSION VERSION_GREATER_EQUAL 6.6)
      set(_murasaki_ls_screen 1)
    else()
      set(_murasaki_ls_screen 0)
    endif()
    target_compile_definitions(murasaki_surface_core PUBLIC
      MURASAKI_HAVE_LAYERSHELL MURASAKI_LAYERSHELL_HAS_SCREEN=${_murasaki_ls_screen})
  else()
    message(STATUS "LayerShellQt not found: murasaki-surface will only run with --windowed")
  endif()

  qt_add_executable(murasaki-surface main.cpp)
  target_link_libraries(murasaki-surface PRIVATE murasaki_surface_core murasaki_renderplugin)
  install(TARGETS murasaki-surface)
endif()
