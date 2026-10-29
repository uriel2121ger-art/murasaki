# The X11 presenter: EWMH window hints for the wallpaper and pet windows, and the monitor of an X11 session. Needs only
# xcb (libxcb1-dev) next to Qt's GUI module; the state building (x11state) needs neither.
add_library(murasaki_x11_logic STATIC x11state.cpp x11state.h)
target_include_directories(murasaki_x11_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_x11_logic PUBLIC murasaki_session_logic Qt6::Core)

if(TARGET PkgConfig::XCB AND MURASAKI_SURFACE)
  add_library(murasaki_x11 STATIC
    x11hints.cpp x11hints.h
    x11monitor.cpp x11monitor.h
    x11util.cpp x11util.h)
  target_include_directories(murasaki_x11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  # SessionMonitor (and its moc) lives in the surface's monitors library.
  target_link_libraries(murasaki_x11 PUBLIC murasaki_x11_logic murasaki_session_monitors Qt6::Gui PkgConfig::XCB)
  # The pets' input shape (setInputShape) goes through Xlib's SHAPE calls; without them it reports false and the window falls back to setMask().
  pkg_check_modules(XEXT_SHAPE QUIET IMPORTED_TARGET xext x11)
  if(XEXT_SHAPE_FOUND)
    target_compile_definitions(murasaki_x11 PUBLIC MURASAKI_HAVE_X11_SHAPE)
    target_link_libraries(murasaki_x11 PUBLIC PkgConfig::XEXT_SHAPE)
  endif()
endif()
