# Where the user units go: share/systemd/user by default (systemd also reads it); a distribution package uses
# lib/systemd/user.
set(MURASAKI_SYSTEMD_USER_UNIT_DIR ${CMAKE_INSTALL_DATADIR}/systemd/user CACHE STRING
    "Install directory of the systemd user units, relative to the prefix")

# The units and the D-Bus service files name the installed programs by absolute path. They are generated when
# installing, from the prefix of that install (`cmake --install build --prefix DIR` and DESTDIR both work), not
# when configuring: a file written at configure time would keep pointing at the configured prefix.
function(murasaki_install_generated template destination)
  string(REGEX REPLACE "\\.in$" "" _name ${template})
  # Directories this creates are 0755 and the file 0644 whatever the umask of the installing user (a package must not
  # ship group-writable files); a directory that already exists is left as it is.
  install(CODE "
    set(CMAKE_INSTALL_FULL_BINDIR \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    set(_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    string(REPLACE \"/\" \";\" _parts \"${destination}\")
    foreach(_part IN LISTS _parts)
      string(APPEND _dir \"/\${_part}\")
      if(NOT IS_DIRECTORY \"\${_dir}\")
        file(MAKE_DIRECTORY \"\${_dir}\")
        file(CHMOD \"\${_dir}\" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
      endif()
    endforeach()
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/${template}\" \"\${_dir}/${_name}\" @ONLY)
    file(CHMOD \"\${_dir}/${_name}\" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  ")
endfunction()

murasaki_install_generated(io.github.uriel2121ger_art.Murasaki.service.in ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
murasaki_install_generated(murasakid.service.in ${MURASAKI_SYSTEMD_USER_UNIT_DIR})
if(MURASAKI_SURFACE)
  murasaki_install_generated(murasaki-surface.service.in ${MURASAKI_SYSTEMD_USER_UNIT_DIR})
endif()

# The desktop pets: a user unit the daemon starts with the first pet, D-Bus activation for the same name, and a
# hidden desktop entry that gives the process an application id (the global shortcut portals want one).
if(TARGET murasaki-overlay)
  murasaki_install_generated(murasaki-overlay.service.in ${MURASAKI_SYSTEMD_USER_UNIT_DIR})
  murasaki_install_generated(io.github.uriel2121ger_art.Murasaki.Overlay.service.in ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
  install(FILES io.github.uriel2121ger_art.Murasaki.Overlay.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

# Desktop integration for the GUI (the application id matches QGuiApplication::desktopFileName).
if(MURASAKI_APP)
  install(FILES io.github.uriel2121ger_art.Murasaki.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES icons/hicolor/scalable/apps/io.github.uriel2121ger_art.Murasaki.svg
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  install(FILES io.github.uriel2121ger_art.Murasaki.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

# The Plasma wallpaper package. It is self-contained: the Murasaki.Bridge module is copied into contents/ui and
# imported by relative path, so plasmashell needs no QML_IMPORT_PATH. main.qml learns the absolute path of
# murasaki-surface here, so plasmashell (which has no PATH it can count on) starts exactly this install's host.
if(TARGET murasaki_bridge_qml AND MURASAKI_SURFACE)
  set(MURASAKI_PLASMA_ID io.github.uriel2121ger_art.Murasaki)
  set(_package_src ${CMAKE_CURRENT_SOURCE_DIR}/plasma/${MURASAKI_PLASMA_ID})
  set(_package_dest ${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/${MURASAKI_PLASMA_ID})
  install(FILES ${_package_src}/contents/config/main.xml DESTINATION ${_package_dest}/contents/config)
  install(FILES ${_package_src}/contents/ui/MurasakiView.qml ${_package_src}/contents/ui/config.qml
          DESTINATION ${_package_dest}/contents/ui)
  get_property(_module_files GLOBAL PROPERTY MURASAKI_BRIDGE_MODULE_FILES)
  install(FILES ${_module_files} DESTINATION ${_package_dest}/contents/ui/Murasaki/Bridge)
  # Three generated files are written when installing, from the real prefix (so that `cmake --install --prefix DIR`
  # works, and so does DESTDIR): metadata.json (the project version) and the two that name an installed program.
  install(CODE "
    set(MURASAKI_SURFACE_PROGRAM \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/murasaki-surface\")
    set(MURASAKI_APP_PROGRAM \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/murasaki\")
    set(PROJECT_VERSION \"${PROJECT_VERSION}\")
    set(_root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_package_dest}\")
    configure_file(\"${_package_src}/metadata.json.in\" \"\${_root}/metadata.json\" @ONLY)
    set(_ui \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_package_dest}/contents/ui\")
    configure_file(\"${_package_src}/contents/ui/main.qml.in\" \"\${_ui}/main.qml\" @ONLY)
    configure_file(\"${_package_src}/contents/ui/ConfigStatus.qml.in\" \"\${_ui}/ConfigStatus.qml\" @ONLY)
    # Whatever the umask of the installing user: readable by everybody, writable by the owner.
    file(CHMOD \"\${_root}/metadata.json\" \"\${_ui}/main.qml\" \"\${_ui}/ConfigStatus.qml\"
         PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  ")
endif()

# The GNOME Shell extension (docs/gnome.md): enable it with gnome-extensions. The tests and the README are not installed.
install(DIRECTORY gnome-shell/murasaki@uriel2121ger-art.github.io/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/murasaki@uriel2121ger-art.github.io
        PATTERN "test" EXCLUDE PATTERN "README.md" EXCLUDE)
