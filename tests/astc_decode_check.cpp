// Offline check for tools/astc_decode.h, driven by tests/astc_decode_check.py:
//   astc_decode_check <block w> <block h> <width> <height> <blocks.bin> <decoded.rgba>
// Decodes a row-major grid of ASTC blocks to an RGBA8 image and prints the decode rate and error-block count.
#include "../tools/astc_decode.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 7) { std::fprintf(stderr, "usage: bw bh width height blocks decoded\n"); return 2; }
    const int bw = std::atoi(argv[1]), bh = std::atoi(argv[2]), width = std::atoi(argv[3]), height = std::atoi(argv[4]);
    const int nx = (width + bw - 1) / bw, ny = (height + bh - 1) / bh;
    std::ifstream in(argv[5], std::ios::binary);
    const std::vector<uint8_t> blocks((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (blocks.size() != static_cast<size_t>(nx) * ny * 16) { std::fprintf(stderr, "size mismatch\n"); return 2; }
    std::vector<uint8_t> image(static_cast<size_t>(width) * height * 4);
    uint8_t texels[144][4];
    int errors = 0;
    std::map<std::string, int> reasons;
    const auto start = std::chrono::steady_clock::now();
    for (int by = 0; by < ny; ++by) for (int bx = 0; bx < nx; ++bx) {
        if (!refract::astc::decode_block(&blocks[(static_cast<size_t>(by) * nx + bx) * 16], bw, bh, false, texels)) {
            ++errors; ++reasons[refract::astc::detail::last_error];
        }
        for (int y = 0; y < bh && by * bh + y < height; ++y) for (int x = 0; x < bw && bx * bw + x < width; ++x)
            std::memcpy(&image[(static_cast<size_t>(by * bh + y) * width + bx * bw + x) * 4], texels[y * bw + x], 4);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::ofstream(argv[6], std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    std::printf("%dx%d: %d blocks, %d errors, %.1f Mpix/s", bw, bh, nx * ny, errors, width * height / seconds / 1e6);
    for (auto& [why, count] : reasons) std::printf("; %s %d", why.c_str(), count);
    std::printf("\n");
    return 0;
}
