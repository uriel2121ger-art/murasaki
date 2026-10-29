// Implementation units of the vendored single-header libraries. Compiled without warnings (see CMakeLists.txt).

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <volk.h>
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include <stb_image.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include <dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include <dr_flac.h>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include <dr_wav.h>
