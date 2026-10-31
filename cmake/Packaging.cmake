# Debian package with CPack (no debhelper). Build it from a Release configuration with prefix /usr, see
# docs/design/specs/2026-10-17-murasaki-packaging-design.md:
#   cmake -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
#         -DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user && cmake --build build/pkg && (cd build/pkg && cpack -G DEB)


# Debian policy wants a changelog; it is compressed here (-n: no timestamp, reproducible).
find_program(MURASAKI_GZIP gzip)
if(MURASAKI_GZIP)
  execute_process(COMMAND ${MURASAKI_GZIP} -n -9 -c ${PROJECT_SOURCE_DIR}/packaging/deb/changelog
                  OUTPUT_FILE ${CMAKE_BINARY_DIR}/changelog.gz)
  install(FILES ${CMAKE_BINARY_DIR}/changelog.gz DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/murasaki)
endif()
# Directories 0755 whatever the umask of the build.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS ${CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS})

install(FILES ${PROJECT_SOURCE_DIR}/packaging/deb/copyright DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/murasaki)
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSING.md ${PROJECT_SOURCE_DIR}/docs/third-party.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/murasaki)


set(CPACK_PACKAGE_NAME murasaki)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "uriel2121ger-art <uriel2121ger-art@users.noreply.github.com>")
set(CPACK_PACKAGE_VENDOR "uriel2121ger-art")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/uriel2121ger-art/murasaki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Animated wallpapers and desktop pets")
# Wrapped by hand: CPack copies the lines as they are, and Debian wants them under 80 columns.
string(CONCAT CPACK_PACKAGE_DESCRIPTION
    "Murasaki plays video, web and scene wallpapers and shows animated desktop\n"
    "pets. It reads the items of your own Wallpaper Engine and Anima Engine\n"
    "installations and ships none. Wallpapers are set per monitor and per virtual\n"
    "desktop and pause while a maximized or fullscreen window covers them. Web\n"
    "wallpapers have no network unless allowed per item; scene engines run in a\n"
    "sandbox. Includes a KDE Plasma wallpaper plugin and a GNOME Shell extension.")
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR DEB)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL Unspecified)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION kde)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "")

# Libraries come from dpkg-shlibdeps. QML modules are imported at run time and show up in no ELF header: listed here (the ones
# the QML files import: QtQuick, Controls, Layouts, Dialogs, WebEngine, Kirigami; Controls pulls in Templates and Window).
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_NAME murasaki)
# With component packaging CPack takes the long description from the component, not from CPACK_PACKAGE_DESCRIPTION.
set(CPACK_DEBIAN_UNSPECIFIED_DESCRIPTION ${CPACK_PACKAGE_DESCRIPTION})
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_DEPENDS
    "qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-dialogs, qml6-module-qtwebengine, qml6-module-org-kde-kirigami, libvulkan1, dbus-user-session | dbus-x11 | default-dbus-session-bus")
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_RECOMMENDS "bubblewrap, ffmpeg, mesa-vulkan-drivers | vulkan-icd, pipewire-pulse | pulseaudio, qt6-wayland")
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_SUGGESTS "plasma-desktop, steam-installer | steam, qml6-module-org-kde-desktop, steamcmd, va-driver-all | va-driver")
set(CPACK_DEBIAN_UNSPECIFIED_PACKAGE_CONTROL_EXTRA ${PROJECT_SOURCE_DIR}/packaging/deb/postinst)
# 0755 for the maintainer script whatever the umask of the checkout.
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)


include(CPack)
