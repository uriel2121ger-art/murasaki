# Tests that talk to the session bus run under a private bus when dbus-run-session is available, so plain
# ctest never touches the developer's real session. The bus is ISOLATED: dbus-isolated-session.conf has no service
# directories, because a normal session bus always searches the compiled-in /usr/share/dbus-1/services (whatever
# XDG_DATA_DIRS says) and would start the real xdg-desktop-portal, kglobalacceld or murasakid for a test that calls
# a name nobody owns; those write the user's config. The XDG dirs are throw-away ones, one set per test.
find_program(MURASAKI_DBUS_RUN_SESSION dbus-run-session)

# murasaki_isolated_bus_prefix(VAR NAME): the command words that run whatever follows on an isolated private bus.
function(murasaki_isolated_bus_prefix var name)
  set(_home ${CMAKE_CURRENT_BINARY_DIR}/isolated_home/${name})
  file(MAKE_DIRECTORY ${_home})
  set(${var} ${MURASAKI_DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/dbus-isolated-session.conf --
      ${CMAKE_COMMAND} -E env MURASAKI_TEST_PRIVATE_BUS=1 XDG_CONFIG_HOME=${_home}/config XDG_DATA_HOME=${_home}/data
      XDG_CACHE_HOME=${_home}/cache XDG_STATE_HOME=${_home}/state PARENT_SCOPE)
endfunction()

function(murasaki_add_test name)
  cmake_parse_arguments(ARG "PRIVATE_BUS" "" "LIBS" ${ARGN})
  add_executable(${name} ${name}.cpp testutil.h)
  target_link_libraries(${name} PRIVATE murasaki_core Qt6::Test ${ARG_LIBS})
  if(ARG_PRIVATE_BUS AND MURASAKI_DBUS_RUN_SESSION)
    murasaki_isolated_bus_prefix(_prefix ${name})
    add_test(NAME ${name} COMMAND ${_prefix} $<TARGET_FILE:${name}>)
  else()
    add_test(NAME ${name} COMMAND ${name})
  endif()
endfunction()

murasaki_add_test(tst_vdf)
murasaki_add_test(tst_steamlocator)
murasaki_add_test(tst_weproperty)
murasaki_add_test(tst_shortcut LIBS murasaki_shortcut murasaki_core)
murasaki_add_test(tst_bridgechannel LIBS murasaki_bridge)

add_test(NAME murasakid_version COMMAND murasakid --version)
string(REPLACE "." "\\." _murasaki_version_re "${PROJECT_VERSION}")
set_tests_properties(murasakid_version PROPERTIES PASS_REGULAR_EXPRESSION "^murasakid ${_murasaki_version_re}")
add_test(NAME murasakictl_usage_exit
         COMMAND sh -c "$<TARGET_FILE:murasakictl> set x --bogus 2>/dev/null; test $? -eq 2")
murasaki_add_test(tst_weimporter)
murasaki_add_test(tst_itemrisks)
murasaki_add_test(tst_itempermissions)
murasaki_add_test(tst_animationinfo)
murasaki_add_test(tst_animaimporter)
murasaki_add_test(tst_localpets)
murasaki_add_test(tst_localpuppets)
murasaki_add_test(tst_petcheck)
murasaki_add_test(tst_automationmodels)
murasaki_add_test(tst_automationstore)
murasaki_add_test(tst_profileswitch)
murasaki_add_test(tst_timelinerunner)
murasaki_add_test(tst_triggerengine)
murasaki_add_test(tst_screenmatch)
murasaki_add_test(tst_screenrefstore)
murasaki_add_test(tst_securebytes)
murasaki_add_test(tst_packautomation)
murasaki_add_test(tst_demos)
murasaki_add_test(tst_automationmanager LIBS murasakid_service)
murasaki_add_test(tst_automationclosed LIBS murasakid_service)
target_compile_definitions(tst_automationclosed PRIVATE SOURCE_DIR="${PROJECT_SOURCE_DIR}")
murasaki_add_test(tst_automationsources PRIVATE_BUS LIBS murasakid_service)
murasaki_add_test(tst_triggershortcuts LIBS murasaki_overlay_logic)
target_compile_definitions(tst_petcheck PRIVATE PETS_CHECK_DOC="${PROJECT_SOURCE_DIR}/docs/pets/check.md")
if(TARGET murasakictl)
  target_compile_definitions(tst_petcheck PRIVATE MURASAKICTL_BIN="$<TARGET_FILE:murasakictl>")
  add_dependencies(tst_petcheck murasakictl)
endif()
if(TARGET murasaki-overlay)
  target_compile_definitions(tst_petcheck PRIVATE MURASAKI_OVERLAY_BIN="$<TARGET_FILE:murasaki-overlay>")
  add_dependencies(tst_petcheck murasaki-overlay)
endif()
murasaki_add_test(tst_presets)
murasaki_add_test(tst_library)
murasaki_add_test(tst_workshopwatcher)
murasaki_add_test(tst_workshopstate)
murasaki_add_test(tst_murasakiservice PRIVATE_BUS LIBS murasakid_service)
target_compile_definitions(tst_murasakiservice PRIVATE MURASAKI_PETAUDIO_BIN="$<TARGET_FILE:murasaki-petaudio>")
add_dependencies(tst_murasakiservice murasaki-petaudio)
murasaki_add_test(tst_workshopmetadata LIBS murasakid_service)
murasaki_add_test(tst_stageneed PRIVATE_BUS LIBS murasakid_service murasaki_overlay_logic)
murasaki_add_test(tst_transcodemanager LIBS murasakid_service)
murasaki_add_test(tst_transcodebuckets)
murasaki_add_test(tst_pathutil)
murasaki_add_test(tst_jsonfile)
murasaki_add_test(tst_filelog)
murasaki_add_test(tst_sceneenginelookup)
murasaki_add_test(tst_petmixer LIBS murasaki_audio_mixer)
target_compile_definitions(tst_petmixer PRIVATE PETS_AUDIO_DOC="${PROJECT_SOURCE_DIR}/docs/pets/audio.md")
murasaki_add_test(tst_audioimport LIBS murasaki_audio_import)
murasaki_add_test(tst_petaudiomanager LIBS murasakid_service)
murasaki_add_test(tst_overlayunit LIBS murasakid_service)
murasaki_add_test(tst_puppetchecker LIBS murasakid_service)
murasaki_add_test(tst_hostretirement LIBS murasaki_bridge_client)
target_include_directories(tst_hostretirement PRIVATE ${PROJECT_SOURCE_DIR}/src/bridge/qml)
murasaki_add_test(tst_programwatch LIBS murasaki_bridge_client)
target_include_directories(tst_programwatch PRIVATE ${PROJECT_SOURCE_DIR}/src/bridge/qml)
if(TARGET murasaki_bridge_item)
  murasaki_add_test(tst_bridgepark LIBS murasaki_bridge_item)
  set_tests_properties(tst_bridgepark PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
murasaki_add_test(tst_puppetmask LIBS murasaki_overlay_logic)
target_compile_definitions(tst_petaudiomanager PRIVATE MURASAKI_PETAUDIO_BIN="$<TARGET_FILE:murasaki-petaudio>" MURASAKI_SANDBOX_BIN="$<TARGET_FILE:murasaki-sandbox>")
add_dependencies(tst_petaudiomanager murasaki-petaudio murasaki-sandbox)
murasaki_add_test(tst_petaudio LIBS murasaki_audio_protocol)
murasaki_add_test(tst_petaudiocontroller LIBS murasaki_overlay_logic)
murasaki_add_test(tst_petaudiolink LIBS murasaki_overlay_logic)
murasaki_add_test(tst_surfacemutewatcher LIBS murasaki_overlay_logic)
target_compile_definitions(tst_petaudio PRIVATE MURASAKI_PETAUDIO_BIN="$<TARGET_FILE:murasaki-petaudio>")
add_dependencies(tst_petaudio murasaki-petaudio)
murasaki_add_test(tst_sandboxreport)
murasaki_add_test(tst_sandbox LIBS murasaki_landlock Qt6::Network)
target_compile_definitions(tst_sandbox PRIVATE MURASAKI_SANDBOX_PATH="$<TARGET_FILE:murasaki-sandbox>" MURASAKI_SCREENMATCH_PATH="$<TARGET_FILE:murasaki-screenmatch>")
add_dependencies(tst_sandbox murasaki-sandbox murasaki-screenmatch)
murasaki_add_test(tst_networkgate LIBS murasaki_render_logic Qt6::Network)
murasaki_add_test(tst_assignmentstore)
murasaki_add_test(tst_canvas)
murasaki_add_test(tst_petstore)
murasaki_add_test(tst_pettiming)
murasaki_add_test(tst_playliststore)
murasaki_add_test(tst_playlistmanager LIBS murasakid_service)
murasaki_add_test(tst_steamcmdparser LIBS murasakid_service)
murasaki_add_test(tst_steamcmdmanager LIBS murasakid_service)
murasaki_add_test(tst_settingsstore)
murasaki_add_test(tst_ctlutil LIBS murasakictl_logic)
# murasakictl pets against a daemon facade that owns the real service name: only ever on a private bus.
add_executable(tst_ctlpets tst_ctlpets.cpp testutil.h)
target_link_libraries(tst_ctlpets PRIVATE murasakid_service Qt6::Test)
target_compile_definitions(tst_ctlpets PRIVATE MURASAKICTL_PATH="$<TARGET_FILE:murasakictl>" MURASAKI_PETAUDIO_BIN="$<TARGET_FILE:murasaki-petaudio>" MURASAKI_PUPPET_PATH="$<TARGET_FILE:murasaki-puppet>")
add_dependencies(tst_ctlpets murasakictl murasaki-petaudio murasaki-puppet)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_ctlpets)
  add_test(NAME tst_ctlpets COMMAND ${_prefix} $<TARGET_FILE:tst_ctlpets>)
else()
  add_test(NAME tst_ctlpets COMMAND tst_ctlpets)
endif()
# And the examples: murasakictl demo against the real service with the automation attached.
add_executable(tst_ctldemo tst_ctldemo.cpp testutil.h)
target_link_libraries(tst_ctldemo PRIVATE murasakid_service Qt6::Test)
target_compile_definitions(tst_ctldemo PRIVATE MURASAKICTL_PATH="$<TARGET_FILE:murasakictl>")
add_dependencies(tst_ctldemo murasakictl)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_ctldemo)
  add_test(NAME tst_ctldemo COMMAND ${_prefix} $<TARGET_FILE:tst_ctldemo>)
else()
  add_test(NAME tst_ctldemo COMMAND tst_ctldemo)
endif()
set_tests_properties(tst_ctldemo PROPERTIES TIMEOUT 120)
# The same for the playlist commands.
add_executable(tst_ctlscreen tst_ctlscreen.cpp testutil.h)
target_link_libraries(tst_ctlscreen PRIVATE murasakictl_logic murasaki_core Qt6::DBus Qt6::Test)
target_compile_definitions(tst_ctlscreen PRIVATE MURASAKICTL_PATH="$<TARGET_FILE:murasakictl>")
add_dependencies(tst_ctlscreen murasakictl)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_ctlscreen)
  add_test(NAME tst_ctlscreen COMMAND ${_prefix} $<TARGET_FILE:tst_ctlscreen>)
else()
  add_test(NAME tst_ctlscreen COMMAND tst_ctlscreen)
endif()
set_tests_properties(tst_ctlscreen PROPERTIES TIMEOUT 180)

add_executable(tst_ctlplaylists tst_ctlplaylists.cpp testutil.h)
target_link_libraries(tst_ctlplaylists PRIVATE murasakid_service Qt6::Test)
target_compile_definitions(tst_ctlplaylists PRIVATE MURASAKICTL_PATH="$<TARGET_FILE:murasakictl>")
add_dependencies(tst_ctlplaylists murasakictl)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_ctlplaylists)
  add_test(NAME tst_ctlplaylists COMMAND ${_prefix} $<TARGET_FILE:tst_ctlplaylists>)
else()
  add_test(NAME tst_ctlplaylists COMMAND tst_ctlplaylists)
endif()
# And the SteamCMD commands, with a fake steamcmd behind the facade.
add_executable(tst_ctlsteamcmd tst_ctlsteamcmd.cpp fakesteamcmd.h testutil.h)
target_link_libraries(tst_ctlsteamcmd PRIVATE murasakid_service Qt6::Test)
target_compile_definitions(tst_ctlsteamcmd PRIVATE MURASAKICTL_PATH="$<TARGET_FILE:murasakictl>")
add_dependencies(tst_ctlsteamcmd murasakictl)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_ctlsteamcmd)
  add_test(NAME tst_ctlsteamcmd COMMAND ${_prefix} $<TARGET_FILE:tst_ctlsteamcmd>)
else()
  add_test(NAME tst_ctlsteamcmd COMMAND tst_ctlsteamcmd)
endif()
murasaki_add_test(tst_autostart)
# The script test needs only QJSEngine, so it does not depend on the surface switch.
find_package(Qt6 6.4 QUIET COMPONENTS Qml)
if(Qt6Qml_FOUND)
  murasaki_add_test(tst_plasmacontrol LIBS murasaki_plasma Qt6::Qml)
  murasaki_add_test(tst_webapiscript LIBS murasaki_render_logic Qt6::Qml)
  murasaki_add_test(tst_kwinmonitorscript LIBS Qt6::Qml)
  target_compile_definitions(tst_kwinmonitorscript PRIVATE KWIN_MONITOR_SCRIPT="${PROJECT_SOURCE_DIR}/src/surface/kwin-monitor.js")
  murasaki_add_test(tst_kwincursorscript LIBS Qt6::Qml)
  target_compile_definitions(tst_kwincursorscript PRIVATE KWIN_CURSOR_SCRIPT="${PROJECT_SOURCE_DIR}/src/surface/kwin-cursor.js")
  murasaki_add_test(tst_propertymodel LIBS murasaki_app_logic)
endif()
murasaki_add_test(tst_wallpapercontent LIBS murasaki_render_logic)
murasaki_add_test(tst_sessionstate LIBS murasaki_session_logic)
murasaki_add_test(tst_toplevelstate LIBS murasaki_session_logic)
murasaki_add_test(tst_compositorprofile LIBS murasaki_session_logic)
# The foreign-toplevel monitor against a fake Wayland compositor (libwayland-server in a thread of the test).
if(TARGET murasaki_wlr_protocol)
  pkg_check_modules(WAYLAND_SERVER IMPORTED_TARGET wayland-server)
  if(WAYLAND_SERVER_FOUND)
    set(_server_header ${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-server-protocol.h)
    add_custom_command(OUTPUT ${_server_header}
      COMMAND ${MURASAKI_WAYLAND_SCANNER} server-header
              ${PROJECT_SOURCE_DIR}/src/surface/protocols/wlr-foreign-toplevel-management-unstable-v1.xml ${_server_header}
      DEPENDS ${PROJECT_SOURCE_DIR}/src/surface/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
    add_executable(tst_foreigntoplevelmonitor tst_foreigntoplevelmonitor.cpp ${_server_header})
    target_include_directories(tst_foreigntoplevelmonitor PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(tst_foreigntoplevelmonitor PRIVATE murasaki_session_monitors murasaki_wlr_protocol PkgConfig::WAYLAND_SERVER Qt6::Test)
    add_test(NAME tst_foreigntoplevelmonitor COMMAND tst_foreigntoplevelmonitor)
    set_tests_properties(tst_foreigntoplevelmonitor PROPERTIES TIMEOUT 60)
  endif()
  if(WAYLAND_SERVER_FOUND AND TARGET murasaki_idle_protocol)
    set(_idle_server_header ${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-server-protocol.h)
    add_custom_command(OUTPUT ${_idle_server_header}
      COMMAND ${MURASAKI_WAYLAND_SCANNER} server-header
              ${PROJECT_SOURCE_DIR}/src/surface/protocols/ext-idle-notify-v1.xml ${_idle_server_header}
      DEPENDS ${PROJECT_SOURCE_DIR}/src/surface/protocols/ext-idle-notify-v1.xml)
    add_executable(tst_waylandidlemonitor tst_waylandidlemonitor.cpp ${_idle_server_header})
    target_include_directories(tst_waylandidlemonitor PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(tst_waylandidlemonitor PRIVATE murasaki_session_monitors murasaki_idle_protocol PkgConfig::WAYLAND_SERVER Qt6::Test)
    add_test(NAME tst_waylandidlemonitor COMMAND tst_waylandidlemonitor)
    set_tests_properties(tst_waylandidlemonitor PROPERTIES TIMEOUT 60)
  endif()
endif()
murasaki_add_test(tst_shellwindows)
murasaki_add_test(tst_playbackpolicy LIBS murasaki_session_logic)
murasaki_add_test(tst_spansync LIBS murasaki_session_logic)
murasaki_add_test(tst_x11state LIBS murasaki_x11_logic)
murasaki_add_test(tst_pausepolicy LIBS murasaki_session_logic)
murasaki_add_test(tst_outputhealth LIBS murasaki_session_logic)
# The optional services (GameMode, UPower) are faked on a private bus.
murasaki_add_test(tst_buspropertywatcher PRIVATE_BUS LIBS murasaki_session_logic)
murasaki_add_test(tst_buslease PRIVATE_BUS LIBS murasakid_service)
if(TARGET murasaki_media)
  murasaki_add_test(tst_mediahost PRIVATE_BUS LIBS murasaki_media)
endif()
if(TARGET murasaki_shortcut_host)
  murasaki_add_test(tst_shortcuthost LIBS murasaki_shortcut_host murasaki_core)
endif()
murasaki_add_test(tst_audiospectrum LIBS murasaki_session_logic)
murasaki_add_test(tst_cursorrouter LIBS murasaki_session_logic)
# SurfaceService is small and needs only Qt D-Bus, so it is compiled straight into its test.
murasaki_add_test(tst_surfaceservice PRIVATE_BUS LIBS Qt6::DBus)
target_sources(tst_surfaceservice PRIVATE ${PROJECT_SOURCE_DIR}/src/surface/surfaceservice.cpp
                                          ${PROJECT_SOURCE_DIR}/src/surface/surfaceservice.h)
target_include_directories(tst_surfaceservice PRIVATE ${PROJECT_SOURCE_DIR}/src/surface ${PROJECT_SOURCE_DIR}/src/core)
# The GUI's data layer needs only Qt Core + D-Bus; the tests run it against a fake D-Bus backend.
murasaki_add_test(tst_daemonclient LIBS murasaki_app_logic)
murasaki_add_test(tst_librarymodel LIBS murasaki_app_logic)
murasaki_add_test(tst_usenowflow LIBS murasaki_app_logic)
murasaki_add_test(tst_overlayclient LIBS murasaki_app_logic)
murasaki_add_test(tst_libraryfiltermodel LIBS murasaki_app_logic)
murasaki_add_test(tst_systemcontrol LIBS murasaki_app_logic)
# Every UI string of the GUI must be translated: the .ts must exist, be complete, and match what lupdate finds
# in the sources (lupdate is optional; without it only the completeness check runs).
set(_ts_args -DTS=${PROJECT_SOURCE_DIR}/src/app/i18n/murasaki_es.ts)
if(MURASAKI_LUPDATE)
  list(APPEND _ts_args -DLUPDATE=${MURASAKI_LUPDATE} -DSRC=${PROJECT_SOURCE_DIR}/src/app
       -DWORK=${CMAKE_CURRENT_BINARY_DIR}/translations_check)
endif()
add_test(NAME translations_complete COMMAND ${CMAKE_COMMAND} ${_ts_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.cmake)

# The GUI loads and every page opens without a QML warning. It runs offscreen on a private session bus with no
# daemon (empty library allowed), without network (the Workshop shows its offline page), and with its own XDG
# directories so it never touches the developer's session, portals or browser profile.
if(MURASAKI_APP AND MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix murasaki_smoke)
  add_test(NAME murasaki_smoke
           COMMAND ${_prefix}
             ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen QT_ACCESSIBILITY=0 QT_NO_XDG_DESKTOP_PORTAL=1
               MURASAKI_SMOKE_ALLOW_EMPTY=1 "QTWEBENGINE_CHROMIUM_FLAGS=--proxy-server=127.0.0.1:9 --disable-gpu"
               $<TARGET_FILE:murasaki> --smoke-test)
  set_tests_properties(murasaki_smoke PROPERTIES TIMEOUT 90)
endif()

# The automation sweep (tools/compat/automation_sweep.py): hostile specs and files, the caps, a fire storm and a soak against a real murasakid on a private bus.
find_program(MURASAKI_BUSCTL busctl)
execute_process(COMMAND python3 -c "import PIL" RESULT_VARIABLE _pil_missing OUTPUT_QUIET ERROR_QUIET)
if(MURASAKI_DBUS_RUN_SESSION AND MURASAKI_BUSCTL AND NOT _pil_missing AND TARGET murasakid AND TARGET murasakictl)
  add_test(NAME automation_sweep COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/compat/automation_sweep.py --build-dir ${CMAKE_BINARY_DIR} --storm 250 --switches 40 --name ctest)
  set_tests_properties(automation_sweep PROPERTIES TIMEOUT 240 ENVIRONMENT "MURASAKI_SWEEP_CACHE=${CMAKE_CURRENT_BINARY_DIR}/sweep_cache")
  add_dependencies(tst_automationmanager murasakid murasakictl)
  # the inner half of the sweep deletes automation to test the caps: started by hand, outside its private directory, it must refuse
  add_test(NAME automation_sweep_refuses COMMAND sh -c "MURASAKI_SWEEP_BUS=1 python3 -I ${PROJECT_SOURCE_DIR}/tools/compat/automation_sweep.py >/dev/null 2>&1; test $? -ne 0")
endif()

# The pages of the automation against a client with data (the Kirigami QML module is needed at run time, as for the smoke test).
if(MURASAKI_APP)
  add_executable(tst_automationpages tst_automationpages.cpp fakebackend.h)
  target_link_libraries(tst_automationpages PRIVATE murasaki_app_logic murasaki_core Qt6::Test Qt6::Gui Qt6::Quick Qt6::Qml)
  add_test(NAME tst_automationpages COMMAND tst_automationpages)
  set_tests_properties(tst_automationpages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0;QT_NO_XDG_DESKTOP_PORTAL=1" TIMEOUT 90)
endif()

# Scene engine (SceneItem) logic: launch arguments, restart policy and the bridge client against a fake producer.
murasaki_add_test(tst_sceneengine LIBS murasaki_render_logic)
murasaki_add_test(tst_sceneselect LIBS murasaki_render_logic)
if(TARGET murasaki-scene-vk)
  # a test that runs the real engine's --check for the fallback verdicts
  target_compile_definitions(tst_sceneselect PRIVATE MURASAKI_SCENE_VK_PATH="$<TARGET_FILE:murasaki-scene-vk>")
  add_dependencies(tst_sceneselect murasaki-scene-vk)
endif()
murasaki_add_test(tst_bridgeclient LIBS murasaki_bridge_client)
# The dma-buf import needs a render node; the test skips itself without one.
if(TARGET murasaki_dmabuf)
  murasaki_add_test(tst_dmabufimport LIBS murasaki_dmabuf)
endif()


# Bridge producer against the real consumer pieces; needs EGL and a render node (skips itself without one).
if(TARGET murasaki_bridge_producer AND TARGET murasaki_dmabuf)
  murasaki_add_test(tst_bridgeproducer LIBS murasaki_bridge_producer murasaki_render_logic murasaki_dmabuf)
endif()

# The shared Murasaki.Bridge module (the one the Plasma package ships) loads from the build directory and its
# types instantiate without a warning. Offscreen, on a private bus with no daemon.
if(TARGET murasaki_bridgeplugin)
  murasaki_add_test(tst_bridgeqml PRIVATE_BUS LIBS Qt6::Quick Qt6::DBus)
  add_dependencies(tst_bridgeqml murasaki_bridgeplugin)
  target_include_directories(tst_bridgeqml PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
  target_compile_definitions(tst_bridgeqml PRIVATE MURASAKI_BRIDGE_QML_DIR="${MURASAKI_BRIDGE_QML_DIR}")
  set_tests_properties(tst_bridgeqml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0" TIMEOUT 60)
endif()

# Wallpaper.qml's switching: the real file against stand-in delegates, offscreen.
if(TARGET Qt6::Quick)
  murasaki_add_test(tst_wallpaperswitch LIBS Qt6::Quick Qt6::Qml)
  target_compile_definitions(tst_wallpaperswitch PRIVATE MURASAKI_WALLPAPER_QML="${PROJECT_SOURCE_DIR}/src/render/Wallpaper.qml"
    MURASAKI_PREVIEWITEM_QML="${PROJECT_SOURCE_DIR}/src/render/PreviewItem.qml")
  set_tests_properties(tst_wallpaperswitch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0" TIMEOUT 120)
endif()

# SurfaceController with fake sinks against a fake daemon on a private bus; offscreen, no compositor.
if(MURASAKI_SURFACE)
  murasaki_add_test(tst_surfaceheap LIBS murasaki_surface_core Qt6::Gui)
  murasaki_add_test(tst_surfacecontroller PRIVATE_BUS LIBS murasaki_surface_core Qt6::Gui)
  set_tests_properties(tst_surfacecontroller PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0;XDG_CURRENT_DESKTOP=" TIMEOUT 60)
  if(TARGET murasaki_x11)
    # The same binary on a private Xvfb for the one test that needs an X11 session (it skips itself elsewhere).
    target_link_libraries(tst_surfacecontroller PRIVATE murasaki_x11)
    target_compile_definitions(tst_surfacecontroller PRIVATE MURASAKI_X11_TESTS)
    murasaki_isolated_bus_prefix(_x11_prefix tst_surfacecontroller_x11)
    add_test(NAME tst_surfacecontroller_x11
             COMMAND ${_x11_prefix} ${CMAKE_CURRENT_SOURCE_DIR}/with-xvfb.sh $<TARGET_FILE:tst_surfacecontroller>
                     anX11SessionPausesWhenAWindowIsFullscreen)
    set_tests_properties(tst_surfacecontroller_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
                         ENVIRONMENT "XDG_CURRENT_DESKTOP=")
  endif()
endif()

# Web items have no network unless allowed per item: real WebItem and profile wiring against servers on 127.0.0.1 that
# the test starts. Offscreen with the software scene graph (the test forces both).
if(MURASAKI_SURFACE)
  add_executable(tst_webnetwork tst_webnetwork.cpp testutil.h)
  target_link_libraries(tst_webnetwork PRIVATE murasaki_render murasaki_renderplugin Qt6::Test Qt6::Gui Qt6::Quick
                        Qt6::Network Qt6::WebEngineQuick)
  add_test(NAME tst_webnetwork COMMAND tst_webnetwork)
  set_tests_properties(tst_webnetwork PROPERTIES ENVIRONMENT "QT_ACCESSIBILITY=0;XDG_CURRENT_DESKTOP=" TIMEOUT 120)
endif()


# PuppetItem (the puppet pet's picture): how it tells a refusal from a crash and what it launches, without a window or an engine.
if(MURASAKI_SURFACE)
  add_executable(tst_puppetitem tst_puppetitem.cpp ${PROJECT_SOURCE_DIR}/src/overlay/puppetitem.cpp ${PROJECT_SOURCE_DIR}/src/overlay/puppetitem.h testutil.h)
  target_include_directories(tst_puppetitem PRIVATE ${PROJECT_SOURCE_DIR}/src/overlay ${PROJECT_SOURCE_DIR}/src/bridge/qml ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(tst_puppetitem PRIVATE murasaki_render murasaki_overlay_logic murasaki_core Qt6::Test Qt6::Gui Qt6::Quick)
  add_test(NAME tst_puppetitem COMMAND tst_puppetitem)
  set_tests_properties(tst_puppetitem PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0" TIMEOUT 120)
endif()

# VideoItem pet mode: an alpha file keeps its alpha through libmpv's render API. Needs a Qt platform with OpenGL
# (skips itself otherwise, as on the offscreen platform without an X server).
if(MURASAKI_SURFACE)
  add_executable(tst_videoitem tst_videoitem.cpp animfixtures.h testutil.h)
  target_link_libraries(tst_videoitem PRIVATE murasaki_render Qt6::Test Qt6::Gui Qt6::Quick)
  # mpv's own pan and zoom against the maths of a spanned video: software render, no GPU needed.
  add_executable(tst_mpvspan tst_mpvspan.cpp testutil.h)
  target_link_libraries(tst_mpvspan PRIVATE murasaki_core Qt6::Test Qt6::Gui PkgConfig::MPV)
  add_test(NAME tst_mpvspan COMMAND tst_mpvspan)
  set_tests_properties(tst_mpvspan PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_ACCESSIBILITY=0" TIMEOUT 90)
  add_test(NAME tst_videoitem COMMAND tst_videoitem)
  set_tests_properties(tst_videoitem PROPERTIES ENVIRONMENT "QT_ACCESSIBILITY=0;XDG_CURRENT_DESKTOP=" TIMEOUT 90)
endif()

# The sprite decoder needs FFmpeg's libraries; the offscreen render test also a Qt platform with OpenGL.
if(TARGET murasaki_sprite)
  murasaki_add_test(tst_spritedecoder LIBS murasaki_sprite murasaki_core)
  murasaki_add_test(tst_spritemask LIBS murasaki_sprite murasaki_core Qt6::Gui)
  if(WEBPDEMUX_FOUND) # its assertions are about libwebp's compositing; Qt's reader (the fallback) is the bug they guard against
    murasaki_add_test(tst_spritewebp LIBS murasaki_sprite murasaki_core Qt6::Gui)
  endif()
  add_executable(tst_spriteitem tst_spriteitem.cpp animfixtures.h testutil.h)
  target_link_libraries(tst_spriteitem PRIVATE murasaki_sprite Qt6::Test Qt6::Gui Qt6::Quick)
  add_test(NAME tst_spriteitem COMMAND tst_spriteitem)
  # The test forces the offscreen platform and the software scene graph itself; the display is never used.
  set_tests_properties(tst_spriteitem PROPERTIES ENVIRONMENT "QT_ACCESSIBILITY=0;XDG_CURRENT_DESKTOP=" TIMEOUT 90)
endif()

# The X11 presenter against a private Xvfb (tests/with-xvfb.sh): window hints and the session monitor. Skipped (77)
# without Xvfb.
if(TARGET murasaki_x11)
  find_program(MURASAKI_XVFB Xvfb)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(XEXT QUIET IMPORTED_TARGET xext x11)
  endif()
  foreach(_name tst_x11monitor tst_x11hints)
    if(_name STREQUAL "tst_x11hints" AND NOT TARGET PkgConfig::XEXT)
      continue()
    endif()
    add_executable(${_name} ${_name}.cpp)
    target_link_libraries(${_name} PRIVATE murasaki_x11 Qt6::Gui Qt6::Test)
    if(_name STREQUAL "tst_x11hints")
      target_link_libraries(${_name} PRIVATE PkgConfig::XEXT murasaki_overlay_logic)  # (petsurface.h: the flags the pet windows really use)
    endif()
    add_test(NAME ${_name} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/with-xvfb.sh $<TARGET_FILE:${_name}>)
    set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endforeach()
endif()

# The overlay's logic (controller, shortcut backends, session watch) needs only Qt Core + D-Bus.
murasaki_add_test(tst_overlaycontroller LIBS murasaki_overlay_logic)
murasaki_add_test(tst_interactcontroller LIBS murasaki_overlay_logic)
murasaki_add_test(tst_petsurface LIBS murasaki_overlay_logic)
murasaki_add_test(tst_petgeometry LIBS murasaki_overlay_logic)
murasaki_add_test(tst_stagegeometry LIBS murasaki_overlay_logic)
target_compile_definitions(tst_stagegeometry PRIVATE PETS_STAGE_DOC="${PROJECT_SOURCE_DIR}/docs/pets/stage.md")
murasaki_add_test(tst_petinteraction LIBS murasaki_overlay_logic)
murasaki_add_test(tst_petstates LIBS murasaki_overlay_logic)
murasaki_add_test(tst_pettracker LIBS murasaki_overlay_logic)
target_compile_definitions(tst_petstates PRIVATE PETS_STATES_DOC="${PROJECT_SOURCE_DIR}/docs/pets/states.md")
murasaki_add_test(tst_petinput LIBS murasaki_overlay_logic)
murasaki_add_test(tst_pointersource PRIVATE_BUS LIBS murasaki_pointer_sources Qt6::Qml)
target_compile_definitions(tst_pointersource PRIVATE KWIN_PET_CURSOR_SCRIPT="${PROJECT_SOURCE_DIR}/src/overlay/kwin-pet-cursor.js")
murasaki_add_test(tst_petlayers LIBS murasaki_overlay_logic)
murasaki_add_test(tst_alphamask LIBS murasaki_overlay_logic Qt6::Gui)
target_compile_definitions(tst_alphamask PRIVATE PETS_BEHAVIOURS_DOC="${PROJECT_SOURCE_DIR}/docs/pets/behaviours.md")
murasaki_add_test(tst_crashledger LIBS murasaki_overlay_logic)
# These two fake well-known names of the user's desktop (the GlobalShortcuts portal, kglobalaccel, ScreenSaver):
# they run only on an isolated bus (MURASAKI_TEST_PRIVATE_BUS=1) and skip themselves anywhere else.
foreach(_name tst_globalshortcut tst_sessionwatch)
  add_executable(${_name} ${_name}.cpp)
  target_link_libraries(${_name} PRIVATE murasaki_overlay_logic Qt6::Test)
  if(MURASAKI_DBUS_RUN_SESSION)
    murasaki_isolated_bus_prefix(_prefix ${_name})
    add_test(NAME ${_name} COMMAND ${_prefix} $<TARGET_FILE:${_name}>)
  else()
    add_test(NAME ${_name} COMMAND ${_name})
  endif()
endforeach()

# The screen cast portal client against a fake portal (also a name of the user's desktop: private bus only).
add_executable(tst_screencastportal tst_screencastportal.cpp)
target_link_libraries(tst_screencastportal PRIVATE murasakid_service Qt6::Test)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_screencastportal)
  add_test(NAME tst_screencastportal COMMAND ${_prefix} $<TARGET_FILE:tst_screencastportal>)
else()
  add_test(NAME tst_screencastportal COMMAND tst_screencastportal)
endif()

# The layer-shell presenter on sway, labwc and Hyprland (headless, private bus, grim screenshots). Skips (77) for every
# compositor that is not installed, and entirely without grim.
if(TARGET murasaki-surface AND MURASAKI_SURFACE)
  add_test(NAME wlroots_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-wlroots-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/wlroots-nested-out)
  set_tests_properties(wlroots_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The GNOME Shell extension's pure modules (window classification, the state report, placement), with node.
find_program(MURASAKI_NODE NAMES node nodejs)
if(MURASAKI_NODE)
  add_test(NAME gnome_extension_node
    COMMAND ${MURASAKI_NODE} --test ${PROJECT_SOURCE_DIR}/data/gnome-shell/murasaki@uriel2121ger-art.github.io/test/lib.test.js)
endif()

# GNOME: a headless gnome-shell with the extension, private bus. Skips (77) without gnome-shell.
if(TARGET murasaki-surface AND MURASAKI_SURFACE)
  add_test(NAME gnome_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-gnome-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/gnome-nested-out)
  set_tests_properties(gnome_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
endif()

# Plasma host mode end to end (nested kwin, private bus, private murasakid, bridge-view as the consumer). Needs the
# user's Wallpaper Engine library and a GPU, so the script skips (77) unless MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(TARGET bridge-view AND MURASAKI_SURFACE)
  add_test(NAME host_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-host-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/host-nested-out)
  set_tests_properties(host_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The desktop pets in a nested kwin: a generated pet over a striped fullscreen backdrop, transparency, arrange mode,
# the global shortcut (a real kglobalacceld on the private bus), UpdatePet round trips and the transcode. Needs a GPU;
# skipped (77) unless MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(TARGET murasaki-overlay AND TARGET murasaki-overlay-backdrop)
  add_test(NAME overlay_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-overlay-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/overlay-nested-out)
  set_tests_properties(overlay_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 700)
  # The same checks through the same overlay on sway (headless) and on Hyprland (a window of a virtual KWin); each skips when its
  # compositor is not installed.
  foreach(comp sway hyprland)
    add_test(NAME overlay_nested_${comp}
      COMMAND ${PROJECT_SOURCE_DIR}/tools/test-overlay-nested.sh --build-dir ${CMAKE_BINARY_DIR} --compositor ${comp}
              --out-dir ${CMAKE_BINARY_DIR}/overlay-nested-${comp}-out)
    set_tests_properties(overlay_nested_${comp} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 700)
  endforeach()
  # The same script with two virtual outputs: a desktop stage is one picture across both.
  add_test(NAME overlay_desktop_nested
    COMMAND ${CMAKE_COMMAND} -E env MURASAKI_NESTED_OUTPUTS=2 ${PROJECT_SOURCE_DIR}/tools/test-overlay-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/overlay-desktop-nested-out)
  set_tests_properties(overlay_desktop_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
  # The overlay on an X11 session (a private Xvfb, no window manager): the pet and the stage as the X server sees them.
  add_test(NAME overlay_x11
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-overlay-x11.sh --build-dir ${CMAKE_BINARY_DIR} --out-dir ${CMAKE_BINARY_DIR}/overlay-x11-out)
  set_tests_properties(overlay_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  # A puppet pet on the same private X11 session: the item and its sandboxed check, the engine drawing, a crash restarted, three crashes blocking the model.
  add_test(NAME overlay_puppet_x11
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-overlay-puppet-x11.sh --build-dir ${CMAKE_BINARY_DIR} --out-dir ${CMAKE_BINARY_DIR}/overlay-puppet-x11-out)
  set_tests_properties(overlay_puppet_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
endif()

# The pets' sound player against a real PipeWire in a private session with a null sink (also inside murasaki-sandbox, and when PipeWire goes away).
# Skipped (77) unless MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(TARGET murasaki-petaudio)
  add_test(NAME petaudio_pipewire
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-petaudio-pipewire.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/petaudio-pipewire-out)
  set_tests_properties(petaudio_pipewire PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# The pointer facts behind the desktop pets' interactions (docs/pets/pointer-facts.md): a probe layer surface in a nested
# kwin, sway and Hyprland, pointer motion injected through org_kde_kwin_fake_input or zwlr_virtual_pointer_v1. Skipped (77)
# unless MURASAKI_NESTED_TESTS=1 is set when ctest runs, and for every compositor that is not installed.
if(TARGET murasaki-pointer-probe AND TARGET murasaki-pointer-fakeinput AND TARGET murasaki-pointer-sink)
  add_test(NAME overlay_pointer_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-overlay-pointer-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/overlay-pointer-nested-out)
  set_tests_properties(overlay_pointer_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The Plasma wallpaper package in a nested plasmashell (installs into a temp prefix, never the real one). Same gate
# as above: skipped (77) unless MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(MURASAKI_SURFACE AND TARGET murasaki_bridge_qml)
  add_test(NAME plasma_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-plasma-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/plasma-nested-out)
  set_tests_properties(plasma_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

# The screen triggers in a nested kwin with the real portal (xdg-desktop-portal and its KDE backend) up to the desktop's dialog, which needs a person: skipped (77) unless
# MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(TARGET murasakid AND TARGET murasaki-screenmatch)
  add_test(NAME screenwatch_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-screenwatch-nested.sh --build-dir ${CMAKE_BINARY_DIR}
            --out-dir ${CMAKE_BINARY_DIR}/screenwatch-nested-out)
  set_tests_properties(screenwatch_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# murasaki-scene-vk (src/scene-vk): the engine library is plain C++; its tests link Qt Test only for the test runner.
# GPU tests skip themselves without a Vulkan 1.3 device (or dma-buf export, or a render node for the EGL side).
if(TARGET murasaki_scene_vk_lib)
  if(TARGET murasaki_dmabuf)
    murasaki_add_test(tst_scenevk_core LIBS murasaki_scene_vk_lib murasaki_dmabuf)
  endif()
  murasaki_add_test(tst_scenevk_json LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_assets LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_imagedecode LIBS murasaki_scene_vk_lib Qt6::Gui)
  murasaki_add_test(tst_scenevk_puppet LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_hostile_mutation LIBS murasaki_scene_vk_lib Qt6::Gui)
  murasaki_add_test(tst_scenevk_scene LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_media LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_shaders LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_materials LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_uniforms LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_analysis LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_passopt LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_particles LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_audio LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_script LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_puppetgen LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_bonephysics LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_attachment LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_shortcut LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_scriptapi LIBS murasaki_scene_vk_lib)
  target_compile_definitions(tst_scenevk_scriptapi PRIVATE SCENE_VK_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs/scene-vk")
  murasaki_add_test(tst_scenevk_text LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_video LIBS murasaki_scene_vk_lib)
  # Twice (2026-10-20, 2026-10-24) this test hung the iGPU's gfx ring (reset failed, reboot) while other GPU tests ran beside
  # it (every other battery it passed). Its VAAPI surfaces come down through radeonsi blits on that same ring: it runs by itself.
  set_tests_properties(tst_scenevk_video PROPERTIES RUN_SERIAL TRUE)
  murasaki_add_test(tst_scenevk_pipewire LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_render LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_viewmapping LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_camera LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_fpscontrol LIBS murasaki_scene_vk_lib)
  murasaki_add_test(tst_scenevk_heap LIBS murasaki_scene_vk_lib)
  # The real binary against a fake bridge consumer.
  murasaki_add_test(tst_scenevk_engine LIBS murasaki_scene_vk_lib)
  target_compile_definitions(tst_scenevk_engine PRIVATE MURASAKI_SCENE_VK_PATH="$<TARGET_FILE:murasaki-scene-vk>"
                             MURASAKI_SANDBOX_PATH="$<TARGET_FILE:murasaki-sandbox>")
  target_link_libraries(tst_scenevk_engine PRIVATE murasaki_landlock)
  add_dependencies(tst_scenevk_engine murasaki-scene-vk murasaki-sandbox)
  murasaki_add_test(tst_scenevk_check LIBS murasaki_scene_vk_lib)
  target_compile_definitions(tst_scenevk_check PRIVATE MURASAKI_SCENE_VK_PATH="$<TARGET_FILE:murasaki-scene-vk>")
  add_dependencies(tst_scenevk_check murasaki-scene-vk)
endif()


# The whole install in a temporary prefix: every unit, activation file, desktop entry and the Plasma package points at
# the installed files, and the installed programs run with a clean environment (needs the complete build).
add_test(NAME install_layout COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test-install-layout.sh ${CMAKE_BINARY_DIR})
set_tests_properties(install_layout PROPERTIES TIMEOUT 300)

# A web wallpaper on the installed layout (temporary prefix, clean environment, private bus, virtual kwin): its local
# files load and the network stays closed. Skipped (77) unless MURASAKI_NESTED_TESTS=1 is set when ctest runs.
if(TARGET bridge-view AND MURASAKI_SURFACE)
  add_test(NAME installed_web_nested
    COMMAND ${PROJECT_SOURCE_DIR}/tools/test-installed-web-nested.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(installed_web_nested PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
endif()

murasaki_add_test(tst_apngwriter LIBS murasaki_import_logic)
murasaki_add_test(tst_sheetslicer LIBS murasaki_import_logic)
murasaki_add_test(tst_boundedzip LIBS murasaki_import_logic)
murasaki_add_test(tst_mddoparser LIBS murasaki_import_logic)
murasaki_add_test(tst_importspec LIBS murasaki_import_logic)

if(TARGET murasaki_import_job)
  murasaki_add_test(tst_petimportjob LIBS murasaki_import_job Qt6::Gui)
  target_compile_definitions(tst_petimportjob PRIVATE MURASAKI_PETIMPORT_BIN="$<TARGET_FILE:murasaki-petimport>")
  add_dependencies(tst_petimportjob murasaki-petimport)
endif()
murasaki_add_test(tst_colorkey LIBS murasaki_import_logic)
murasaki_add_test(tst_videosource LIBS murasaki_import_video)

if(TARGET murasaki_import_job)
  murasaki_add_test(tst_petimportmanager LIBS murasakid_service murasaki_import_job Qt6::Gui)
  target_compile_definitions(tst_petimportmanager PRIVATE MURASAKI_PETIMPORT_BIN="$<TARGET_FILE:murasaki-petimport>"
                             MURASAKI_SANDBOX_BIN="$<TARGET_FILE:murasaki-sandbox>")
  add_dependencies(tst_petimportmanager murasaki-petimport murasaki-sandbox)
endif()

murasaki_add_test(tst_sandboxargs LIBS murasaki_import_logic)
target_compile_definitions(tst_sandboxargs PRIVATE MURASAKI_SANDBOX_BIN="$<TARGET_FILE:murasaki-sandbox>")
add_dependencies(tst_sandboxargs murasaki-sandbox)
target_compile_definitions(tst_importspec PRIVATE PETS_IMPORT_DOC="${PROJECT_SOURCE_DIR}/docs/pets/import.md")
murasaki_add_test(tst_mdaaimport LIBS murasaki_import_logic Qt6::Gui)
murasaki_add_test(tst_npyreader LIBS murasaki_import_logic)
murasaki_add_test(tst_screenframes LIBS murasaki_import_logic Qt6::Gui)

# The geometry of a stage against an independent implementation in exact arithmetic (tools/compat/stage_geometry_check.py).
if(TARGET murasaki-petprobe)
  find_program(PYTHON3_EXECUTABLE python3)
  if(PYTHON3_EXECUTABLE)
    add_test(NAME stage_geometry_check
      COMMAND ${PYTHON3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/compat/stage_geometry_check.py --probe $<TARGET_FILE:murasaki-petprobe>)
    set_tests_properties(stage_geometry_check PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endif()
endif()

# Puppets (docs/pets/puppets.md): the manifest check, the strict JSON reader, the limits and names tables, and the murasaki-puppet --check skeleton.
murasaki_add_test(tst_puppetmanifest LIBS murasaki_puppet_logic)
target_compile_definitions(tst_puppetmanifest PRIVATE PUPPETS_DOC="${PROJECT_SOURCE_DIR}/docs/pets/puppets.md" MURASAKI_PUPPET_BIN="$<TARGET_FILE:murasaki-puppet>")
add_dependencies(tst_puppetmanifest murasaki-puppet)
murasaki_add_test(tst_puppetmotion LIBS murasaki_puppet_logic)
murasaki_add_test(tst_puppetlife LIBS murasaki_puppet_logic)
murasaki_add_test(tst_puppetsession LIBS murasaki_puppet_logic)
murasaki_add_test(tst_puppetphysics LIBS murasaki_puppet_logic)
murasaki_add_test(tst_puppetview LIBS murasaki_puppet_logic)
murasaki_add_test(tst_puppetenginehost LIBS murasaki_overlay_logic)
murasaki_add_test(tst_screenmatchprotocol LIBS murasaki_screenmatch_logic)
murasaki_add_test(tst_screenmatchsession LIBS murasaki_screenmatch_logic)
murasaki_add_test(tst_screenmatchbuffer LIBS murasaki_screenmatch_logic)
murasaki_add_test(tst_screenmatchhost LIBS murasaki_overlay_logic)
murasaki_add_test(tst_screenmatchplan LIBS murasaki_screenmatch_logic)
# The daemon's side of the screen triggers against a fake portal and a fake helper: only ever on a private bus.
add_executable(tst_screenwatch tst_screenwatch.cpp testutil.h)
target_link_libraries(tst_screenwatch PRIVATE murasakid_service Qt6::Test)
if(MURASAKI_DBUS_RUN_SESSION)
  murasaki_isolated_bus_prefix(_prefix tst_screenwatch)
  add_test(NAME tst_screenwatch COMMAND ${_prefix} $<TARGET_FILE:tst_screenwatch>)
else()
  add_test(NAME tst_screenwatch COMMAND tst_screenwatch)
endif()
set_tests_properties(tst_screenwatch PROPERTIES TIMEOUT 180)
# A real PipeWire server of its own (a private runtime directory), a test producer of video frames and the real helper behind the real sandbox.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(TEST_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
endif()
if(TEST_PIPEWIRE_FOUND)
  add_executable(screenmatch_pwproducer screenmatch/pwproducer.cpp)
  target_link_libraries(screenmatch_pwproducer PRIVATE PkgConfig::TEST_PIPEWIRE)
  murasaki_add_test(tst_screenmatchpipewire LIBS murasaki_overlay_logic)
  target_compile_definitions(tst_screenmatchpipewire PRIVATE MURASAKI_SANDBOX_PATH="$<TARGET_FILE:murasaki-sandbox>" MURASAKI_SCREENMATCH_PATH="$<TARGET_FILE:murasaki-screenmatch>"
                             MURASAKI_PWPRODUCER_PATH="$<TARGET_FILE:screenmatch_pwproducer>")
  add_dependencies(tst_screenmatchpipewire murasaki-sandbox murasaki-screenmatch screenmatch_pwproducer)
endif()
murasaki_add_test(tst_puppetopen LIBS murasaki_puppet_logic)
target_compile_definitions(tst_puppetopen PRIVATE MURASAKI_PUPPET_BIN="$<TARGET_FILE:murasaki-puppet>")
add_dependencies(tst_puppetopen murasaki-puppet)
# The golden curves of tst_puppetmotion are generated by an independent script; this fails when the header is out of date.
find_program(PYTHON3_EXECUTABLE python3)
if(NOT PYTHON3_EXECUTABLE)
  message(WARNING "python3 not found: the golden curves of the puppet motions are not checked against their generator")
else()
  add_test(NAME puppet_curves_golden COMMAND ${PYTHON3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/compat/puppet_curves.py --check ${CMAKE_CURRENT_SOURCE_DIR}/puppetcurvegolden.h)
endif()

# Universal means no scene of the user's library is left incomplete: this build's --check on every installed scene fails on a
# refusal not listed in tools/compat/check-census-allowed.tsv. Needs the library; skipped (77) unless MURASAKI_NESTED_TESTS=1.
if(PYTHON3_EXECUTABLE AND TARGET murasaki-scene-vk)
  add_test(NAME check_census
    COMMAND ${PYTHON3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/compat/check_census.py --ctest --engine $<TARGET_FILE:murasaki-scene-vk>)
  set_tests_properties(check_census PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The puppet engine's renderer and process: needs Vulkan (murasaki-scene-vk's context), runs on whatever device there is, a software one included; skips without one.
if(TARGET murasaki_puppet_gpu)
  murasaki_add_test(tst_puppetrender LIBS murasaki_puppet_gpu Qt6::Gui)
  murasaki_add_test(tst_puppetengine LIBS murasaki_puppet_gpu Qt6::Gui)
  target_compile_definitions(tst_puppetengine PRIVATE MURASAKI_PUPPET_BIN="$<TARGET_FILE:murasaki-puppet>" MURASAKI_SANDBOX_BIN="$<TARGET_FILE:murasaki-sandbox>")
  add_dependencies(tst_puppetengine murasaki-puppet murasaki-sandbox)
endif()

# Temporary directories of the tests go under ~/.cache, never to /tmp (a RAM disk on the development machines). The path is short on purpose: a test makes a Unix socket in it (108 bytes at most).
set(_murasaki_test_tmp "$ENV{HOME}/.cache/murasaki-tt")
file(MAKE_DIRECTORY "${_murasaki_test_tmp}")
get_property(_murasaki_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _murasaki_all_tests)
  set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION "TMPDIR=set:${_murasaki_test_tmp}")
endforeach()
