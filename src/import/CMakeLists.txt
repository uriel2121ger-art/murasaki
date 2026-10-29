# Import of pets from sources Anima Engine's authoring tools take (sprite sheets, PNG sequences, ZIPs, videos, GIFs, .mdaa packages): the pure
# parts, with no GUI stack. See docs/pets/import.md.
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBPMUX IMPORTED_TARGET libwebpmux libwebp)
add_library(murasaki_import_logic STATIC
  apngwriter.cpp apngwriter.h
  boundedzip.cpp boundedzip.h
  colorkey.cpp colorkey.h
  framesink.h
  webpwriter.cpp webpwriter.h
  imagesequence.cpp imagesequence.h
  importfolders.cpp importfolders.h
  importspec.cpp importspec.h
  mdaaimport.cpp mdaaimport.h
  sandboxargs.cpp sandboxargs.h
  mddoparser.cpp mddoparser.h
  npyreader.cpp npyreader.h
  screenframes.cpp screenframes.h
  petimport.h
  sheetslicer.cpp sheetslicer.h
  triggerspack.cpp triggerspack.h)
target_include_directories(murasaki_import_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_import_logic PUBLIC murasaki_core ZLIB::ZLIB Qt6::Core)
if(WEBPMUX_FOUND)
  target_compile_definitions(murasaki_import_logic PUBLIC MURASAKI_HAVE_WEBPMUX)
  target_link_libraries(murasaki_import_logic PUBLIC PkgConfig::WEBPMUX)
else()
  message(WARNING "libwebpmux not found: pets cannot be imported as animated WebP")
endif()

# The video source (libavformat and libavcodec; the helper decodes untrusted videos with them, see videosource.h). Without libav the library still builds and
# every video is refused with "not a video".
pkg_check_modules(IMPORT_LIBAV IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
add_library(murasaki_import_video STATIC videosource.cpp videosource.h keyedsink.h)
target_link_libraries(murasaki_import_video PUBLIC murasaki_import_logic)
if(IMPORT_LIBAV_FOUND)
  target_compile_definitions(murasaki_import_video PUBLIC MURASAKI_HAVE_LIBAV)
  target_link_libraries(murasaki_import_video PUBLIC PkgConfig::IMPORT_LIBAV)
else()
  message(WARNING "libav not found: pets cannot be imported from videos")
endif()

# The job itself decodes images, so it needs Qt Gui (image I/O only: no platform plugin, no window): built when the GUI stack is present.
find_package(Qt6 6.8 QUIET COMPONENTS Gui)
if(Qt6Gui_FOUND)
  add_library(murasaki_import_job STATIC importjob.cpp importjob.h imagesniff.h)
  target_link_libraries(murasaki_import_job PUBLIC murasaki_import_logic murasaki_import_video Qt6::Gui)
  add_executable(murasaki-petimport petimport_main.cpp)
  target_link_libraries(murasaki-petimport PRIVATE murasaki_import_job)
  # next to murasakid: findPetImportHelper() looks in the daemon's own directory
  install(TARGETS murasaki-petimport RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
