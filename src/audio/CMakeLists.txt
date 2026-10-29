find_package(Threads REQUIRED)
# The audio of the desktop pets (docs/pets/audio.md): the mixer and the asset reader (no Qt), the import of a sound file (libav) and the player process.
add_library(murasaki_audio_mixer STATIC petmixer.cpp petmixer.h)
target_include_directories(murasaki_audio_mixer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/core)
target_compile_features(murasaki_audio_mixer PUBLIC cxx_std_20)

add_library(murasaki_audio_asset STATIC petaudioasset.cpp petaudioasset.h sha256.cpp sha256.h)
target_link_libraries(murasaki_audio_asset PUBLIC murasaki_audio_mixer)

# Decoding a sound the user picked: libavformat, libavcodec and libswresample. Without them the library builds and every import is refused.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDIO_LIBAV IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
add_library(murasaki_audio_import STATIC audioimport.cpp audioimport.h)
target_link_libraries(murasaki_audio_import PUBLIC murasaki_audio_asset)
if(AUDIO_LIBAV_FOUND)
  target_compile_definitions(murasaki_audio_import PUBLIC MURASAKI_HAVE_LIBAV)
  target_link_libraries(murasaki_audio_import PUBLIC PkgConfig::AUDIO_LIBAV)
else()
  message(WARNING "libav or libswresample not found: pets cannot get a sound from a file")
endif()

# The player process and the protocol it speaks (also linked by the overlay's controller for formatting commands).
add_library(murasaki_audio_protocol STATIC protocol.cpp protocol.h)
target_link_libraries(murasaki_audio_protocol PUBLIC murasaki_audio_asset)

add_library(murasaki_audio_player STATIC player.cpp player.h)
target_link_libraries(murasaki_audio_player PUBLIC murasaki_audio_mixer Threads::Threads)
pkg_check_modules(AUDIO_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
if(AUDIO_PIPEWIRE_FOUND)
  target_compile_definitions(murasaki_audio_player PUBLIC MURASAKI_HAVE_PIPEWIRE)
  target_link_libraries(murasaki_audio_player PUBLIC PkgConfig::AUDIO_PIPEWIRE)
else()
  message(STATUS "murasaki-petaudio: PipeWire not found, pets cannot play sounds")
endif()


add_executable(murasaki-petaudio petaudio_main.cpp)
target_link_libraries(murasaki-petaudio PRIVATE murasaki_audio_import murasaki_audio_protocol murasaki_audio_player)
install(TARGETS murasaki-petaudio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
