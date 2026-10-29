# Test tools of tools/test-overlay-pointer-nested.sh (not installed): a layer-shell surface that behaves like a pet window
# and is driven by a FIFO (pointer-probe), and a client of org_kde_kwin_fake_input / zwlr_virtual_pointer_v1 that moves the
# pointer of a private nested KWin, sway or Hyprland (pointer-fakeinput).
enable_language(C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner)

# Stands in for the overlay's pointer D-Bus object: logs the reports of kwin-pet-cursor.js.
find_package(Qt6 REQUIRED COMPONENTS DBus)
add_executable(murasaki-pointer-sink sink.cpp)
target_link_libraries(murasaki-pointer-sink PRIVATE Qt6::Core Qt6::DBus)

add_executable(murasaki-pointer-probe probe.cpp)
target_link_libraries(murasaki-pointer-probe PRIVATE Qt6::Gui LayerShellQt::Interface)

if(WAYLAND_SCANNER)
  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${_gen})
  add_custom_command(
    OUTPUT ${_gen}/fake-input-client-protocol.h ${_gen}/fake-input-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${CMAKE_CURRENT_SOURCE_DIR}/fake-input.xml ${_gen}/fake-input-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${CMAKE_CURRENT_SOURCE_DIR}/fake-input.xml ${_gen}/fake-input-protocol.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fake-input.xml)
  add_custom_command(
    OUTPUT ${_gen}/wlr-virtual-pointer-client-protocol.h ${_gen}/wlr-virtual-pointer-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${CMAKE_CURRENT_SOURCE_DIR}/wlr-virtual-pointer.xml ${_gen}/wlr-virtual-pointer-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${CMAKE_CURRENT_SOURCE_DIR}/wlr-virtual-pointer.xml ${_gen}/wlr-virtual-pointer-protocol.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wlr-virtual-pointer.xml)
  add_executable(murasaki-pointer-fakeinput fakeinput.c ${_gen}/fake-input-protocol.c ${_gen}/fake-input-client-protocol.h
                 ${_gen}/wlr-virtual-pointer-protocol.c ${_gen}/wlr-virtual-pointer-client-protocol.h)
  target_include_directories(murasaki-pointer-fakeinput PRIVATE ${_gen})
  target_link_libraries(murasaki-pointer-fakeinput PRIVATE PkgConfig::WAYLAND_CLIENT)
endif()
