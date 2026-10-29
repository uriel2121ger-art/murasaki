# murasaki-scene-vk: the own Vulkan scene engine (subproject 10). Plain C++20 + POSIX, no Qt: it is a process of its
# own that speaks the bridge protocol. See docs/scene-vk/README.md.

find_path(MURASAKI_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(MURASAKI_LZ4 IMPORTED_TARGET liblz4)
  pkg_check_modules(MURASAKI_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3) # optional: sound objects and the spectrum
  pkg_check_modules(MURASAKI_FREETYPE QUIET IMPORTED_TARGET freetype2)       # optional: text objects
  pkg_check_modules(MURASAKI_FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
  pkg_check_modules(MURASAKI_LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libswscale libavutil) # optional: video textures
  pkg_check_modules(MURASAKI_LIBVA QUIET IMPORTED_TARGET libva)                                       # optional: video pictures as dma-bufs
endif()
if(NOT MURASAKI_VULKAN_INCLUDE_DIR OR NOT MURASAKI_LZ4_FOUND)
  if(MURASAKI_SCENE_VK STREQUAL "ON")
    message(FATAL_ERROR "MURASAKI_SCENE_VK=ON needs the Vulkan headers and liblz4")
  endif()
  message(STATUS "murasaki-scene-vk disabled: Vulkan headers or liblz4 not found")
  return()
endif()

# ---- GLSL -> SPIR-V: a system glslang when there is one, otherwise a pinned release built from source --------
# Only the compiler library is built (no binaries, tools, tests or SPIRV-Tools optimizer), see docs/third-party.md.
find_package(glslang CONFIG QUIET)
if(glslang_FOUND AND TARGET glslang::SPIRV AND TARGET glslang::glslang-default-resource-limits)
  message(STATUS "murasaki-scene-vk: using the system glslang")
else()
  include(FetchContent)
  set(MURASAKI_FETCH_DIR "" CACHE PATH "Shared download cache for pinned third-party tarballs (optional)")
  set(_download_dir)
  if(MURASAKI_FETCH_DIR)
    file(MAKE_DIRECTORY ${MURASAKI_FETCH_DIR})
    set(_download_dir DOWNLOAD_DIR ${MURASAKI_FETCH_DIR})
  endif()
  FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
    URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
    ${_download_dir}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
  # The warning flags of Murasaki (-Werror on CI) are for our code, not for glslang.
  get_directory_property(_saved_options COMPILE_OPTIONS)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
  set(_saved_shared ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(glslang)
  set(BUILD_SHARED_LIBS ${_saved_shared})
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_saved_options}")
endif()

enable_language(C) # volk.c

# ---- vendored single-file libraries (third_party/, listed in docs/third-party.md) -----------------------------
add_library(murasaki_scene_vk_third_party STATIC
  third_party/volk/volk.c
  third_party/vorbis_impl.c
  third_party/impl.cpp)
target_include_directories(murasaki_scene_vk_third_party SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vma
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs
  ${MURASAKI_VULKAN_INCLUDE_DIR})
# volk resolves every Vulkan entry point at run time: nothing links against libvulkan.
target_compile_definitions(murasaki_scene_vk_third_party PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(murasaki_scene_vk_third_party PUBLIC ${CMAKE_DL_LIBS})
target_compile_options(murasaki_scene_vk_third_party PRIVATE -w)
set_target_properties(murasaki_scene_vk_third_party PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- QuickJS-ng (MIT, vendored): SceneScript. No quickjs-libc: a script gets no files, no network, no processes ------
add_library(murasaki_scene_vk_quickjs STATIC
  third_party/quickjs/quickjs.c
  third_party/quickjs/dtoa.c
  third_party/quickjs/libregexp.c
  third_party/quickjs/libunicode.c)
target_include_directories(murasaki_scene_vk_quickjs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs)
target_compile_definitions(murasaki_scene_vk_quickjs PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
target_compile_options(murasaki_scene_vk_quickjs PRIVATE -w -O2) # an interpreter is slow without optimisation, in any build type
target_link_libraries(murasaki_scene_vk_quickjs PUBLIC m)
set_target_properties(murasaki_scene_vk_quickjs PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# ---- the engine library (everything but main), shared with the tests -------------------------------------------
add_library(murasaki_scene_vk_lib STATIC
  assets/pkg.cpp assets/pkg.h
  assets/imagedecode.cpp assets/imagedecode.h
  puppet/mdl.cpp puppet/mdl.h
  puppet/bonephysics.cpp puppet/bonephysics.h
  render/puppetdraw.cpp render/puppetdraw.h
  assets/texcache.cpp assets/texcache.h
  assets/tex.cpp assets/tex.h
  assets/vfs.cpp assets/vfs.h
  core/bridgesink.cpp core/bridgesink.h
  core/chunkedsubmit.cpp core/chunkedsubmit.h
  core/context.cpp core/context.h
  core/framebuffers.cpp core/framebuffers.h
  core/vk.h
  engine/check.cpp engine/check.h
  engine/engine.cpp engine/engine.h
  engine/fpscontrol.cpp engine/fpscontrol.h
  engine/options.cpp engine/options.h
  render/analysis.cpp render/analysis.h
  render/camera.cpp render/camera.h
  render/particlerenderer.cpp render/particlerenderer.h
  render/gpuprofiler.cpp render/gpuprofiler.h
  render/passes.cpp render/passes.h
  render/passprobe.cpp render/passprobe.h
  render/renderer.h
  render/shaderlib.cpp render/shaderlib.h
  render/sprite.cpp render/sprite.h
  render/targets.cpp render/targets.h
  render/viewmapping.cpp render/viewmapping.h
  render/lights.cpp render/lights.h
  render/shadows.cpp render/shadows.h
  render/uniforms.cpp render/uniforms.h
  render/scenerenderer.cpp render/scenerenderer.h
  render/scenerenderer_effects.cpp render/scenerenderer_layer.cpp render/scenerenderer_media.cpp
  render/scenerenderer_mesh.cpp render/scenerenderer_resources.cpp render/scenerenderer_shadows.cpp
  render/scenerenderer_text.cpp render/scenerenderer_view.cpp render/scenerenderer_internal.h
  render/shake.cpp render/shake.h
  render/ssim.cpp render/ssim.h
  render/scenescripts.cpp render/scenescripts.h
  render/texture.cpp render/texture.h
  render/videoconvert.cpp render/videoconvert.h
  shaders/cache.cpp shaders/cache.h
  shaders/glslcompiler.cpp shaders/glslcompiler.h
  shaders/source.cpp shaders/source.h
  shaders/translate.cpp shaders/translate.h
  audio/backend.h audio/soundstream.cpp audio/soundstream.h audio/pipewire.cpp audio/sceneaudio.cpp audio/sceneaudio.h
  audio/soundplayer.cpp audio/soundplayer.h audio/spectrum.cpp audio/spectrum.h
  text/textraster.cpp text/textraster.h
  assets/texfile.cpp assets/texfile.h
  assets/videodecoder.cpp assets/videodecoder.h
  script/prelude.cpp script/prelude.h script/scripthost.cpp script/scripthost.h
  particles/definition.cpp particles/definition.h
  particles/system.cpp particles/system.h
  media/mediastate.cpp media/mediastate.h
  scene/effect.cpp scene/effect.h
  scene/material.cpp scene/material.h
  scene/properties.cpp scene/properties.h
  scene/scene.cpp scene/scene.h
  util/heap.cpp util/heap.h
  util/json.cpp util/json.h
  util/sha256.cpp util/sha256.h
  util/log.h
  util/math.h)
target_include_directories(murasaki_scene_vk_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_scene_vk_lib PUBLIC murasaki_scene_vk_third_party murasaki_scene_vk_quickjs murasaki_bridge PkgConfig::MURASAKI_LZ4)
if(PkgConfig_FOUND)
  pkg_check_modules(MURASAKI_LIBDEFLATE QUIET IMPORTED_TARGET libdeflate) # PNG inflate for the embedded pictures
endif()
if(TARGET PkgConfig::MURASAKI_LIBDEFLATE)
  message(STATUS "murasaki-scene-vk: PNG through libdeflate")
  target_link_libraries(murasaki_scene_vk_lib PUBLIC PkgConfig::MURASAKI_LIBDEFLATE)
  target_compile_definitions(murasaki_scene_vk_lib PUBLIC MURASAKI_SCENE_VK_LIBDEFLATE=1)
endif()
find_package(JPEG QUIET) # libjpeg-turbo: the fast path for embedded JPEG textures (stb_image otherwise)
if(JPEG_FOUND)
  message(STATUS "murasaki-scene-vk: JPEG through ${JPEG_LIBRARIES}")
  target_link_libraries(murasaki_scene_vk_lib PUBLIC JPEG::JPEG)
  target_compile_definitions(murasaki_scene_vk_lib PUBLIC MURASAKI_SCENE_VK_LIBJPEG=1)
else()
  message(STATUS "murasaki-scene-vk: libjpeg not found, JPEG textures decode with stb_image (slower)")
endif()
if(TARGET PkgConfig::MURASAKI_FREETYPE AND TARGET PkgConfig::MURASAKI_FONTCONFIG)
  message(STATUS "murasaki-scene-vk: text through FreeType and fontconfig")
  target_link_libraries(murasaki_scene_vk_lib PUBLIC PkgConfig::MURASAKI_FREETYPE PkgConfig::MURASAKI_FONTCONFIG)
  target_compile_definitions(murasaki_scene_vk_lib PUBLIC MURASAKI_SCENE_VK_TEXT=1)
else()
  message(STATUS "murasaki-scene-vk: FreeType or fontconfig not found, scenes with text are refused (they show their preview)")
endif()
find_package(Threads REQUIRED)
if(TARGET PkgConfig::MURASAKI_LIBAV)
  # libav is a plugin of its own, loaded with dlopen when a scene has a video: linking libavcodec into the engine would put every
  # codec's libraries (125 of them) in the startup of every scene. The engine knows only the C interface of assets/videoapi.h.
  message(STATUS "murasaki-scene-vk: video textures through a libav plugin (${MURASAKI_LIBAV_libavcodec_VERSION})")
  add_library(murasaki_scene_vk_video MODULE assets/videoplugin.cpp assets/videoapi.h assets/videodecoder.h)
  set_target_properties(murasaki_scene_vk_video PROPERTIES OUTPUT_NAME murasaki-scene-vk-video PREFIX "lib" POSITION_INDEPENDENT_CODE ON
                        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        INSTALL_RPATH "$ORIGIN") # libav libraries shipped beside the plugin (a bundle) are found first
  target_include_directories(murasaki_scene_vk_video PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(murasaki_scene_vk_video PRIVATE cxx_std_20)
  target_link_libraries(murasaki_scene_vk_video PRIVATE PkgConfig::MURASAKI_LIBAV Threads::Threads)
  find_path(MURASAKI_HWCONTEXT_VAAPI_DIR libavutil/hwcontext_vaapi.h HINTS ${MURASAKI_LIBAV_INCLUDE_DIRS})
  if(MURASAKI_LIBVA_FOUND AND MURASAKI_HWCONTEXT_VAAPI_DIR)
    target_compile_definitions(murasaki_scene_vk_video PRIVATE MURASAKI_VIDEO_EXPORT=1)
    target_link_libraries(murasaki_scene_vk_video PRIVATE PkgConfig::MURASAKI_LIBVA)
  endif()
  target_link_libraries(murasaki_scene_vk_lib PUBLIC ${CMAKE_DL_LIBS})
  add_dependencies(murasaki_scene_vk_lib murasaki_scene_vk_video) # whatever links the engine's code gets the plugin built first
  target_compile_definitions(murasaki_scene_vk_lib PUBLIC MURASAKI_SCENE_VK_VIDEO=1 MURASAKI_SCENE_VK_LIBDIR="${CMAKE_INSTALL_LIBDIR}"
                             MURASAKI_SCENE_VK_VIDEO_BUILD_PLUGIN="$<TARGET_FILE:murasaki_scene_vk_video>")
  install(TARGETS murasaki_scene_vk_video LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/murasaki)
else()
  message(STATUS "murasaki-scene-vk: libav not found, scenes with video textures are refused (they show their preview)")
endif()
if(TARGET PkgConfig::MURASAKI_PIPEWIRE)
  message(STATUS "murasaki-scene-vk: audio through PipeWire")
  target_link_libraries(murasaki_scene_vk_lib PUBLIC PkgConfig::MURASAKI_PIPEWIRE)
  target_compile_definitions(murasaki_scene_vk_lib PUBLIC MURASAKI_SCENE_VK_PIPEWIRE=1)
else()
  message(STATUS "murasaki-scene-vk: PipeWire not found, scenes play no sound")
endif()
if(TARGET glslang::SPIRV)
  target_link_libraries(murasaki_scene_vk_lib PUBLIC glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
else()
  target_link_libraries(murasaki_scene_vk_lib PUBLIC glslang SPIRV glslang-default-resource-limits)
endif()
target_compile_features(murasaki_scene_vk_lib PUBLIC cxx_std_20)
# Vulkan structs are written `VkFoo info{VK_STRUCTURE_TYPE_FOO};`: the rest is zero by design.
target_compile_options(murasaki_scene_vk_lib PUBLIC -Wno-missing-field-initializers)

add_executable(murasaki-scene-vk main.cpp)
target_link_libraries(murasaki-scene-vk PRIVATE murasaki_scene_vk_lib)
if(TARGET murasaki_scene_vk_video)
  add_dependencies(murasaki-scene-vk murasaki_scene_vk_video)
endif()

# murasaki-scene lookup (src/core/sceneenginelookup.cpp) finds it as `murasaki-scene-vk` on PATH or in ~/.local/bin.
install(TARGETS murasaki-scene-vk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The licence texts of the code compiled into the engine (and into murasaki-puppet, which reuses some of it): MIT, BSD and
# Apache-2.0 want the notice to travel with the binaries. glslang is only linked statically when it was fetched (a system
# glslang comes with its own package and licence).
set(_licdir ${CMAKE_INSTALL_DATADIR}/doc/murasaki/licenses)
install(FILES third_party/quickjs/LICENSE DESTINATION ${_licdir} RENAME quickjs-ng.LICENSE)
install(FILES third_party/volk/LICENSE.md DESTINATION ${_licdir} RENAME volk.LICENSE.md)
install(FILES third_party/vma/LICENSE.txt DESTINATION ${_licdir} RENAME vma.LICENSE.txt)
install(FILES third_party/stb/LICENSE DESTINATION ${_licdir} RENAME stb.LICENSE)
install(FILES third_party/dr_libs/LICENSE DESTINATION ${_licdir} RENAME dr_libs.LICENSE)
if(glslang_SOURCE_DIR)
  install(FILES ${glslang_SOURCE_DIR}/LICENSE.txt DESTINATION ${_licdir} RENAME glslang.LICENSE.txt)
  install(DIRECTORY ${glslang_SOURCE_DIR}/LICENSES/ DESTINATION ${_licdir}/glslang
          PATTERN "GPL-3.0-or-later.txt" EXCLUDE PATTERN "Bison-exception-2.2.txt" EXCLUDE)
endif()

# Development tool: translates and compiles every shader the user's scenes use (docs/scene-vk/shaders.md).
add_executable(scene-vk-shadercheck tools/shadercheck.cpp)
target_link_libraries(scene-vk-shadercheck PRIVATE murasaki_scene_vk_lib)

add_executable(scene-vk-texdump tools/texdump.cpp)
target_link_libraries(scene-vk-texdump PRIVATE murasaki_scene_vk_lib)

add_executable(scene-vk-snap tools/snap.cpp)
target_link_libraries(scene-vk-snap PRIVATE murasaki_scene_vk_lib)

add_executable(scene-vk-ssimcmp tools/ssimcmp.cpp)
target_link_libraries(scene-vk-ssimcmp PRIVATE murasaki_scene_vk_lib)

add_executable(scene-vk-particlesurvey tools/particlesurvey.cpp)
target_link_libraries(scene-vk-particlesurvey PRIVATE murasaki_scene_vk_lib)
