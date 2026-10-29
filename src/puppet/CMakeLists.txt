# The puppet engine's process and its pure parts (docs/pets/puppets.md). No Qt: the process reads hostile model folders.
add_library(murasaki_puppet_logic STATIC
  check.cpp check.h
  expression.cpp expression.h
  lookat.cpp lookat.h
  manifest.cpp manifest.h
  model.h
  motion.cpp motion.h
  openformat.cpp openformat.h
  params.cpp params.h
  physics.cpp physics.h
  pose.cpp pose.h
  session.cpp session.h
  assets.cpp assets.h
  strictjson.cpp strictjson.h
  view.cpp view.h)
target_include_directories(murasaki_puppet_logic PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/bridge)
target_compile_features(murasaki_puppet_logic PUBLIC cxx_std_20)

add_executable(murasaki-puppet main.cpp)
target_compile_definitions(murasaki-puppet PRIVATE MURASAKI_VERSION="${PROJECT_VERSION}")
target_link_libraries(murasaki-puppet PRIVATE murasaki_puppet_logic)
install(TARGETS murasaki-puppet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- the engine: Vulkan, the bridge and the renderer ---------------------------------------------------------------------------------------------------------------
# Built from the few sources of murasaki-scene-vk it needs (the Vulkan context, the bridge sink and the frame buffers, the GLSL compiler and the bounded image decoder), not the whole scene library:
# the puppet process links no script engine, no audio and no video.
if(TARGET murasaki_scene_vk_third_party AND (TARGET glslang::SPIRV OR TARGET SPIRV))
  set(_scene ${PROJECT_SOURCE_DIR}/src/scene-vk)
  add_library(murasaki_puppet_gpu STATIC
    ${_scene}/core/bridgesink.cpp ${_scene}/core/context.cpp ${_scene}/core/framebuffers.cpp
    ${_scene}/shaders/glslcompiler.cpp ${_scene}/assets/imagedecode.cpp ${_scene}/util/sha256.cpp
    engine.cpp engine.h render.cpp render.h)
  target_include_directories(murasaki_puppet_gpu PUBLIC ${_scene})
  target_link_libraries(murasaki_puppet_gpu PUBLIC murasaki_puppet_logic murasaki_scene_vk_third_party murasaki_bridge)
  if(TARGET glslang::SPIRV)
    target_link_libraries(murasaki_puppet_gpu PUBLIC glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
  else()
    target_link_libraries(murasaki_puppet_gpu PUBLIC glslang SPIRV glslang-default-resource-limits)
  endif()
  if(TARGET PkgConfig::MURASAKI_LIBDEFLATE)
    target_link_libraries(murasaki_puppet_gpu PUBLIC PkgConfig::MURASAKI_LIBDEFLATE)
    target_compile_definitions(murasaki_puppet_gpu PUBLIC MURASAKI_SCENE_VK_LIBDEFLATE=1)
  endif()
  if(JPEG_FOUND)
    target_link_libraries(murasaki_puppet_gpu PUBLIC JPEG::JPEG)
    target_compile_definitions(murasaki_puppet_gpu PUBLIC MURASAKI_SCENE_VK_LIBJPEG=1)
  endif()
  target_compile_features(murasaki_puppet_gpu PUBLIC cxx_std_20)
  target_compile_options(murasaki_puppet_gpu PUBLIC -Wno-missing-field-initializers)
  target_link_libraries(murasaki-puppet PRIVATE murasaki_puppet_gpu)
  target_compile_definitions(murasaki-puppet PRIVATE MURASAKI_PUPPET_ENGINE=1)
  set(MURASAKI_PUPPET_ENGINE_BUILT ON PARENT_SCOPE)
else()
  message(STATUS "murasaki-puppet: Vulkan (murasaki-scene-vk) is not built: no engine, only --check")
endif()
