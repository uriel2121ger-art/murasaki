# Everything but the windows: the controller that follows the daemon, the global shortcut backends, the session
# watch and the D-Bus facade. No GUI stack, so CI builds and tests it everywhere.
# The hit masks of the pets (bit grids, the .mask file): no GUI, used by the logic and by the decoder.
add_library(murasaki_alphamask STATIC alphamask.cpp alphamask.h petbehaviours.h petgeometry.h)
target_include_directories(murasaki_alphamask PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_alphamask PUBLIC Qt6::Core)

# The crash ledger and the host of murasaki-screenmatch, on their own so that the daemon (which owns the screen cast) links them without the overlay.
add_library(murasaki_screenmatch_host STATIC crashledger.cpp crashledger.h screenmatchhost.cpp screenmatchhost.h)
target_include_directories(murasaki_screenmatch_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(murasaki_screenmatch_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(murasaki_screenmatch_host PUBLIC murasaki_screenmatch_logic murasaki_core Qt6::Core Qt6::Network)
set_target_properties(murasaki_screenmatch_host PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(murasaki_overlay_logic STATIC
  globalshortcut.cpp globalshortcut.h
  interactcontroller.cpp interactcontroller.h
  overlaycontroller.cpp overlaycontroller.h
  overlayservice.cpp overlayservice.h
  petaudiocontroller.cpp petaudiocontroller.h
  puppetenginehost.cpp puppetenginehost.h
  puppetmask.cpp puppetmask.h
  surfacemutewatcher.cpp surfacemutewatcher.h
  triggershortcuts.cpp triggershortcuts.h
  maskloader.h petbehaviours.h petfps.h petgeometry.h petinput.cpp petinput.h petinteraction.cpp petinteraction.h pointersource.h
  petstatemachine.cpp petstatemachine.h
  pettracker.cpp pettracker.h
  petsurface.h)
target_include_directories(murasaki_overlay_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(murasaki_overlay_logic PRIVATE ${PROJECT_SOURCE_DIR}/src) # ("puppet/exitcodes.h")
target_link_libraries(murasaki_overlay_logic PUBLIC murasaki_screenmatch_host murasaki_screenmatch_logic murasaki_bridge murasaki_audio_protocol murasaki_alphamask murasaki_app_logic murasaki_core murasaki_sessionwatch murasaki_session_logic Qt6::Core Qt6::DBus)

# The pointer sources (task 7 of the phase 2 plan): where a "global" pointer level gets the cursor from, and which one a session uses. Not wired
# into the controller yet. The KWin script is a resource the source writes to the runtime directory and loads over org.kde.kwin.Scripting.
add_library(murasaki_pointer_sources STATIC
  hyprlandpointersource.cpp hyprlandpointersource.h
  kwinpointersource.cpp kwinpointersource.h
  pointercapability.cpp pointercapability.h
  pollingpointersource.cpp pollingpointersource.h)
target_include_directories(murasaki_pointer_sources PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_pointer_sources PUBLIC murasaki_overlay_logic murasaki_session_logic Qt6::Core Qt6::DBus Qt6::Network)
qt_add_resources(murasaki_pointer_sources kwin_pet_cursor_script PREFIX /murasaki FILES kwin-pet-cursor.js)

# Pet animations decoded once into GPU textures (SpriteItem): FFmpeg decodes, Qt Quick draws.
if(MURASAKI_SURFACE AND FFMPEG_FOUND)
  add_library(murasaki_sprite STATIC
    spritedecoder.cpp spritedecoder.h
    spritemask.cpp spritemask.h
    poolmaskloader.cpp poolmaskloader.h
    spriteitem.cpp spriteitem.h
    # The scene engine's adaptive frame rate (pure logic, no Vulkan), built into this library as well: the pets step
    # their own rate down by the same rules when the machine cannot keep up.
    ${PROJECT_SOURCE_DIR}/src/scene-vk/engine/fpscontrol.cpp ${PROJECT_SOURCE_DIR}/src/scene-vk/engine/fpscontrol.h)
  target_include_directories(murasaki_sprite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(murasaki_sprite PRIVATE ${PROJECT_SOURCE_DIR}/src/scene-vk)
  target_link_libraries(murasaki_sprite PUBLIC murasaki_alphamask murasaki_core Qt6::Quick PkgConfig::FFMPEG)
  if(WEBPDEMUX_FOUND)
    # Qt's WebP plugin leaves part of the canvas uninitialised for animated files whose first frame is smaller than the canvas:
    # the pets' pixels come from libwebp's own animation decoder.
    target_compile_definitions(murasaki_sprite PUBLIC MURASAKI_HAVE_LIBWEBP)
    target_link_libraries(murasaki_sprite PUBLIC PkgConfig::WEBPDEMUX)
  else()
    message(WARNING "libwebpdemux not found: animated WebP pets are decoded by Qt's plugin, which can show uninitialised pixels")
  endif()
endif()

# The pets sweep's decoder: first, middle and last frame of an item file as PNG and a JSON line (tools/compat/pets_sweep.py).
if(TARGET murasaki_sprite)
  add_executable(murasaki-petprobe petprobe.cpp)
  target_link_libraries(murasaki-petprobe PRIVATE murasaki_sprite murasaki_overlay_logic murasaki_core Qt6::Core Qt6::Gui)
endif()

# The windows: Qt Quick, libmpv (through the Murasaki.Render module) and layer-shell.
if(MURASAKI_SURFACE AND LayerShellQt_FOUND AND TARGET murasaki_sprite)
  if(LayerShellQt_VERSION VERSION_GREATER_EQUAL 6.6)
    set(_murasaki_ls_screen 1)
  else()
    set(_murasaki_ls_screen 0)
  endif()
  qt_add_executable(murasaki-overlay main.cpp petwindow.cpp petwindow.h interactwindow.cpp interactwindow.h puppetitem.cpp puppetitem.h)
  target_include_directories(murasaki-overlay PRIVATE ${PROJECT_SOURCE_DIR}/src)
  qt_add_resources(murasaki-overlay murasaki_pets PREFIX /qt/qml/Murasaki/Pets FILES PetItem.qml)
  target_compile_definitions(murasaki-overlay PRIVATE MURASAKI_LAYERSHELL_HAS_SCREEN=${_murasaki_ls_screen})
  target_link_libraries(murasaki-overlay PRIVATE
    murasaki_overlay_logic murasaki_pointer_sources murasaki_sprite murasaki_renderplugin murasaki_render Qt6::Quick LayerShellQt::Interface)
  if(TARGET murasaki_x11)
    target_link_libraries(murasaki-overlay PRIVATE murasaki_x11)
    target_compile_definitions(murasaki-overlay PRIVATE MURASAKI_HAVE_X11)
  endif()
  # Relative pointer motion for a drag whose window the compositor animates (PetInteraction, F13). Optional: without it a drag
  # reads the window's frame alone.
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  find_program(WAYLAND_SCANNER wayland-scanner)
  set(_relxml ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)
  if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER AND EXISTS ${_relxml})
    enable_language(C) # (the scanner's code is C)
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
    file(MAKE_DIRECTORY ${_gen})
    add_custom_command(
      OUTPUT ${_gen}/relative-pointer-unstable-v1-client-protocol.h ${_gen}/relative-pointer-unstable-v1-protocol.c
      COMMAND ${WAYLAND_SCANNER} client-header ${_relxml} ${_gen}/relative-pointer-unstable-v1-client-protocol.h
      COMMAND ${WAYLAND_SCANNER} private-code ${_relxml} ${_gen}/relative-pointer-unstable-v1-protocol.c
      DEPENDS ${_relxml})
    target_sources(murasaki-overlay PRIVATE relativepointer.cpp relativepointer.h ${_gen}/relative-pointer-unstable-v1-protocol.c
                   ${_gen}/relative-pointer-unstable-v1-client-protocol.h)
    target_include_directories(murasaki-overlay PRIVATE ${_gen})
    target_link_libraries(murasaki-overlay PRIVATE PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(murasaki-overlay PRIVATE MURASAKI_HAVE_RELATIVE_POINTER)
  else()
    message(STATUS "wayland-client, wayland-scanner or wayland-protocols not found: a pet's drag reads the window's frame alone")
  endif()
  install(TARGETS murasaki-overlay)
elseif(MURASAKI_SURFACE)
  message(STATUS "LayerShellQt or FFmpeg (libavformat, libavcodec, libswscale) not found: murasaki-overlay will not be built")
endif()
