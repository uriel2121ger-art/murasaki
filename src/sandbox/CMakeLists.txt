# murasaki-sandbox: puts a Landlock sandbox around a scene engine and execs it (Linux only, no Qt). See
# docs/security.md.
add_library(murasaki_landlock STATIC landlock.cpp landlock.h bwrap.cpp bwrap.h profile.cpp profile.h seccomp.cpp seccomp.h)
target_include_directories(murasaki_landlock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(murasaki_landlock PUBLIC Threads::Threads)

add_executable(murasaki-sandbox main.cpp)
target_link_libraries(murasaki-sandbox PRIVATE murasaki_landlock)
install(TARGETS murasaki-sandbox RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
