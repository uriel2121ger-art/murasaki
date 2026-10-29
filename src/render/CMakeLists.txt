# Qt-Core-only logic, always built and tested on CI.
add_library(murasaki_render_logic STATIC
  networkgate.cpp networkgate.h
  sceneengine.cpp sceneengine.h
  webapiscript.cpp webapiscript.h
  wallpapercontent.cpp wallpapercontent.h)
target_include_directories(murasaki_render_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_render_logic PUBLIC murasaki_core murasaki_bridge murasaki_bridge_client Qt6::Network)

if(MURASAKI_SURFACE)
  # Linked statically into murasaki-surface (Q_IMPORT_QML_PLUGIN), together with a static copy of BridgeItem
  # (murasaki_bridge_item, SceneItem's base): the surface stays self-contained and never loads the shared
  # Murasaki.Bridge module the Plasma plugin ships (see src/bridge/qml/CMakeLists.txt).
  qt_add_qml_module(murasaki_render
    URI Murasaki.Render
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Murasaki/Render
    SOURCES
      fileaccessinterceptor.cpp fileaccessinterceptor.h
      itemschemehandler.cpp itemschemehandler.h
      sceneitem.cpp sceneitem.h
      webprofilefactory.cpp webprofilefactory.h
      videoitem.cpp videoitem.h
    QML_FILES
      PreviewItem.qml
      Wallpaper.qml
      WebItem.qml)
  target_include_directories(murasaki_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(murasaki_render
    PUBLIC Qt6::Quick Qt6::WebEngineCore Qt6::WebEngineQuick murasaki_render_logic murasaki_bridge_item murasaki_media murasaki_shortcut_host
    PRIVATE Qt6::OpenGL PkgConfig::MPV)
endif()
