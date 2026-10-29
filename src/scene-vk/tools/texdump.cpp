// scene-vk-texdump ITEMDIR PATH OUT.pam: decodes a .tex of an item (first mip, RGBA8 formats) to a PAM file, for
// looking at what a texture really contains (orientation, channels). Development tool.
#include "assets/tex.h"
#include "assets/vfs.h"

#include <cstdio>
#include <fstream>

using namespace murasaki::scene;

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: scene-vk-texdump ITEMDIR PATH OUT.pam\n");
        return 2;
    }
    const Vfs vfs = Vfs::forItem(argv[1], findAssetsDir({}, argv[1]));
    const auto bytes = vfs.read(argv[2]);
    if (!bytes) {
        std::fprintf(stderr, "not found\n");
        return 1;
    }
    std::string error;
    const auto tex = parseTex({bytes->data(), bytes->size()}, {}, &error);
    if (!tex || tex->mips.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const TexMip &m = tex->mips[0];
    std::printf("format %d flags %u storage %ux%u image %ux%u mip0 %ux%u bytes %zu frames %zu\n", tex->rawFormat, tex->flags, tex->textureWidth,
                tex->textureHeight, tex->imageWidth, tex->imageHeight, m.width, m.height, m.data.size(), tex->frames.size());
    if (tex->format == PixelFormat::R8) {
        std::ofstream gray(argv[3], std::ios::binary);
        gray << "P5\n" << m.width << " " << m.height << "\n255\n";
        gray.write(reinterpret_cast<const char *>(m.data.data()), static_cast<std::streamsize>(m.data.size()));
        return 0;
    }
    if (tex->format != PixelFormat::Rgba8) {
        return 0;
    }
    std::ofstream out(argv[3], std::ios::binary);
    out << "P7\nWIDTH " << m.width << "\nHEIGHT " << m.height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    out.write(reinterpret_cast<const char *>(m.data.data()), static_cast<std::streamsize>(m.data.size()));
    return 0;
}
