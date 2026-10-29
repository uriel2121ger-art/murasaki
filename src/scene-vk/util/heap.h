#pragma once

namespace murasaki::scene {

// glibc keeps the blocks freed after a scene is decoded (a 4k picture is freed into an arena, not given back), so a
// scene of 23 layers held 226 MB after its load against 82 MB with the threshold below. Large blocks go to mmap, which
// returns them when freed. Call once at startup, before the first big allocation.
void configureHeap();

// Gives free heap memory back to the system: after a scene has loaded and decoded its pictures. True when memory was
// released (always false off glibc).
bool trimHeap();

} // namespace murasaki::scene
