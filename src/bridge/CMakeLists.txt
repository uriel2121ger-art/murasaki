# Qt-free protocol + channel shared verbatim with the bridge producers. Keep it free of Qt and Murasaki headers.
add_library(murasaki_bridge STATIC
  channel.cpp channel.h
  protocol.h
  spanview.h
  mediaview.h media.cpp shortcutview.h)
target_include_directories(murasaki_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(murasaki_bridge PUBLIC cxx_std_20)
set_target_properties(murasaki_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The producer (engine side of the bridge, used by the Plasma host mode) needs EGL; GBM is optional and only
# adds RequestLinear (without it linear requests fall back to shared memory).
if(EGL_FOUND)
  add_library(murasaki_bridge_producer STATIC producer.cpp producer.h)
  target_link_libraries(murasaki_bridge_producer PUBLIC murasaki_bridge PkgConfig::EGL)
  set_target_properties(murasaki_bridge_producer PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(GBM_FOUND)
    target_compile_definitions(murasaki_bridge_producer PRIVATE MURASAKI_BRIDGE_HAVE_GBM)
    target_link_libraries(murasaki_bridge_producer PRIVATE PkgConfig::GBM)
  endif()
endif()

# dma-buf import through EGL: Qt-free, shared by the BridgeItem render thread and the GPU test.
if(EGL_FOUND)
  add_library(murasaki_dmabuf STATIC dmabufimport.cpp dmabufimport.h)
  target_include_directories(murasaki_dmabuf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(murasaki_dmabuf PUBLIC PkgConfig::EGL)
  set_target_properties(murasaki_dmabuf PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(qml)
