# The host side of user shortcuts (applications the user picks for a wallpaper's `usershortcut` slots): the .desktop reader,
# the policy of when a wallpaper may start one, how one is started, and the catalogue model of the GUI. Qt Core only, so
# it is built and tested wherever the rest of the logic is. docs/design/specs/2026-10-21-user-shortcuts-design.md
add_library(murasaki_shortcut STATIC
  desktopentry.cpp desktopentry.h
  launcher.cpp launcher.h
  launchassigned.cpp launchassigned.h
  projectkeys.cpp projectkeys.h
  shortcutcatalog.cpp shortcutcatalog.h
  shortcutpolicy.cpp shortcutpolicy.h)
target_include_directories(murasaki_shortcut PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(murasaki_shortcut PUBLIC murasaki_appclass Qt6::Core)
set_target_properties(murasaki_shortcut PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The host half: icons (Qt Gui) and the bridge's consumer side. Where the GUI stack exists, like the media host.
find_package(Qt6 6.8 QUIET COMPONENTS Gui DBus)
if(Qt6Gui_FOUND AND Qt6DBus_FOUND)
  add_library(murasaki_shortcut_host STATIC
    daemonlauncher.cpp daemonlauncher.h
    iconrender.cpp iconrender.h
    shortcutitem.cpp shortcutitem.h)
  target_include_directories(murasaki_shortcut_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(murasaki_shortcut_host PUBLIC murasaki_shortcut Qt6::Gui Qt6::DBus murasaki_bridge_client)
  # vector icons of the theme (most apps of Breeze ship only those): read by QtSvg when it is there, otherwise such an application keeps the
  # scene's default icon
  find_package(Qt6 6.8 QUIET COMPONENTS Svg)
  if(Qt6Svg_FOUND)
    target_link_libraries(murasaki_shortcut_host PUBLIC Qt6::Svg)
    target_compile_definitions(murasaki_shortcut_host PUBLIC MURASAKI_SHORTCUT_SVG=1)
  endif()
  set_target_properties(murasaki_shortcut_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
