# Test tool of tools/test-overlay-nested.sh: a fullscreen window of colored stripes.
add_executable(murasaki-overlay-backdrop main.cpp)
target_link_libraries(murasaki-overlay-backdrop PRIVATE Qt6::Gui)
