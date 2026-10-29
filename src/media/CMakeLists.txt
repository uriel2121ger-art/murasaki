# The host side of the media messages of the bridge: MPRIS players, cover files and downloads, and the policy for all of it.
# Needs Qt Gui (QImageReader decodes the covers), so it exists where the GUI stack does.
find_package(Qt6 6.8 QUIET COMPONENTS Gui)
if(Qt6Gui_FOUND)
  add_library(murasaki_media STATIC
    artdecode.cpp artdecode.h
    artfetcher.cpp artfetcher.h
    mediahub.cpp mediahub.h
    mediapolicy.cpp mediapolicy.h
    mediawatcher.cpp mediawatcher.h)
  target_include_directories(murasaki_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(murasaki_media PUBLIC Qt6::Core Qt6::DBus Qt6::Network Qt6::Gui murasaki_bridge_client)
  set_target_properties(murasaki_media PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
