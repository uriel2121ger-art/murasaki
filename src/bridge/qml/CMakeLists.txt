# Qt Core consumer side of the bridge: the socket client, the buffer hand-over queue and the helpers every bridge
# consumer shares (restart policy, properties as JSON). Built everywhere and tested on CI.
add_library(murasaki_bridge_client STATIC
  bridgeclient.cpp bridgeclient.h
  bridgesupport.cpp bridgesupport.h)
target_include_directories(murasaki_bridge_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_bridge_client PUBLIC Qt6::Core murasaki_bridge murasaki_filelog)
set_target_properties(murasaki_bridge_client PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TARGET murasaki_dmabuf AND Qt6Quick_FOUND)
  # BridgeItem exists twice on purpose. murasaki-surface links this static copy into Murasaki.Render (SceneItem
  # derives from it), which keeps the surface binary self-contained: it needs no installed module and no
  # run-time lookup. The Plasma plugin cannot link anything of ours, so the same sources are also compiled
  # into the shared Murasaki.Bridge module below. The two copies never meet in one process: BridgeItem is
  # registered to QML only by Murasaki.Bridge, and the surface never imports it (Wallpaper.qml uses SceneItem).
  add_library(murasaki_bridge_item STATIC bridgeitem.cpp bridgeitem.h bridgenode.cpp bridgenode.h)
  target_include_directories(murasaki_bridge_item PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(murasaki_bridge_item PUBLIC Qt6::Quick murasaki_bridge_client PRIVATE murasaki_dmabuf)
  set_target_properties(murasaki_bridge_item PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # Murasaki.Bridge: the module the Plasma wallpaper package ships and imports by relative path, so plasmashell
  # needs no QML_IMPORT_PATH. A shared backing library plus a plugin, both with an $ORIGIN runpath (the
  # plugin finds the backing library next to itself; BUILD_WITH_INSTALL_RPATH keeps the build tree and the
  # installed copy identical). It links only Qt Quick, Qt D-Bus, EGL (through murasaki_dmabuf) and the bridge.
  set(MURASAKI_BRIDGE_QML_DIR ${CMAKE_BINARY_DIR}/qml CACHE INTERNAL "Import path of the built Murasaki.Bridge module")
  set(_module_dir ${MURASAKI_BRIDGE_QML_DIR}/Murasaki/Bridge)
  qt_add_library(murasaki_bridge_qml SHARED)
  qt_add_qml_module(murasaki_bridge_qml
    URI Murasaki.Bridge
    VERSION 1.0
    PLUGIN_TARGET murasaki_bridgeplugin
    OUTPUT_DIRECTORY ${_module_dir}
    SOURCES
      bridgeitem.cpp bridgeitem.h
      bridgenode.cpp bridgenode.h
      murasakistatus.cpp murasakistatus.h)
  target_include_directories(murasaki_bridge_qml PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
  # What MurasakiStatus.openMurasaki() starts: the GUI of this install.
  target_compile_definitions(murasaki_bridge_qml PRIVATE MURASAKI_APP_PROGRAM="${CMAKE_INSTALL_FULL_BINDIR}/murasaki")
  target_link_libraries(murasaki_bridge_qml PRIVATE Qt6::Quick Qt6::DBus murasaki_bridge_client murasaki_dmabuf)
  set_target_properties(murasaki_bridge_qml murasaki_bridgeplugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${_module_dir}
    BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN" BUILD_WITH_INSTALL_RPATH ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  # Export only what the QML plugin needs (the generated registration function and the plugin entry points):
  # the static libraries folded into the backing library must not leak their symbols into plasmashell.
  target_link_options(murasaki_bridge_qml PRIVATE LINKER:--exclude-libs,ALL)

  # data/CMakeLists.txt installs the module into Qt's QML directory, where the Plasma wallpaper package loads it from.
endif()
