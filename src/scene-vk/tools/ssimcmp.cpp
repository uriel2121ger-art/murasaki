// scene-vk-ssimcmp A.pam B.pam: the SSIM (render/ssim.h) and the largest channel difference of two RGBA pictures
// written by scene-vk-snap. Prints "ssim <value> maxdiff <n>"; exits 2 when the pictures differ in size.

#include "render/ssim.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool readPam(const char *path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &pixels)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic, key;
    in >> magic;
    int depth = 0;
    while (in >> key && key != "ENDHDR") {
        if (key == "WIDTH") {
            in >> w;
        } else if (key == "HEIGHT") {
            in >> h;
        } else if (key == "DEPTH") {
            in >> depth;
        } else {
            std::string skip;
            in >> skip;
        }
    }
    in.get(); // the newline after ENDHDR
    if (magic != "P7" || depth != 4 || w == 0 || h == 0) {
        return false;
    }
    pixels.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return pixels.size() == static_cast<size_t>(w) * h * 4;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: scene-vk-ssimcmp A.pam B.pam\n");
        return 1;
    }
    uint32_t wa = 0, ha = 0, wb = 0, hb = 0;
    std::vector<uint8_t> a, b;
    if (!readPam(argv[1], wa, ha, a) || !readPam(argv[2], wb, hb, b)) {
        std::fprintf(stderr, "cannot read an RGBA PAM\n");
        return 1;
    }
    if (wa != wb || ha != hb) {
        std::fprintf(stderr, "sizes differ\n");
        return 2;
    }
    std::printf("ssim %.5f maxdiff %d\n", murasaki::scene::ssimRgba(a.data(), b.data(), wa, ha), murasaki::scene::maxChannelDifference(a.data(), b.data(), a.size()));
    return 0;
}
