#pragma once

namespace murasaki {

// The surface switches wallpapers all day. Each switch decodes pictures and builds scenes on worker threads, and glibc
// keeps what they free in per-thread arenas: in a 10-minute storm of switches the anonymous memory of the surface grew
// by 35 MB a minute with the default settings and stayed flat with two arenas (or with large blocks mapped).
// Call once at startup, before the threads exist.
void tuneHeap();

// Gives free heap memory back to the system. True when something was released (always false off glibc).
bool trimHeap();

// QtWebEngine keeps one frame-sized dma-buf descriptor (about 4 MB) for every web wallpaper that was shown and replaced,
// even after the view and its profile are gone (measured: one per visit, 30 after 30 visits, none returned). Raise the
// soft limit of open descriptors to the hard one so a long-lived surface that rotates web items does not run out of
// them at 1024. Returns the soft limit in force afterwards (0 when it could not be read).
unsigned long raiseDescriptorLimit();

} // namespace murasaki
