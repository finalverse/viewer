// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// fvft-grab OUTPUT_DIR OUT.ppm: read-only evidence capture of the most recently published
// frame of a finalverse.storm-frame-transport/1 object (Ω-XX visual quality gates). It never
// claims the consumer slot and never writes to the transport; it copies the shared middle
// slot into a binary PPM. These are the renderer's own pixels, exactly as the Viewer gets them.
#include "frame_transport.h"
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>
#include <sys/mman.h>

int main(int argc, char** argv)
{
    if (argc != 3) { std::fprintf(stderr, "usage: fvft-grab OUTPUT_DIR OUT.ppm\n"); return 2; }
    std::ifstream f(std::string(argv[1]) + "/transport.json");
    std::stringstream ss; ss << f.rdbuf();
    std::smatch m1, m2;
    const std::string d = ss.str();
    if (!std::regex_search(d, m1, std::regex("\"shm\":\"(/fvft\\.[0-9a-f.]+)\"")) || !std::regex_search(d, m2, std::regex("\"nonce\":\"([0-9a-f]{32})\"")))
    { std::fprintf(stderr, "no transport\n"); return 1; }
    int fd = shm_open(m1[1].str().c_str(), O_RDONLY, 0);
    if (fd < 0) { std::fprintf(stderr, "transport unlinked\n"); return 1; }
    struct stat st{};
    fstat(fd, &st);
    void* p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 1;
    const auto* h = static_cast<const fvft::Header*>(p);
    if (h->magic != fvft::kMagic || fvft::hex(h->nonce, 16) != m2[1].str()) { std::fprintf(stderr, "transport mismatch\n"); return 1; }
    const uint32_t mid = fvft::load(h->middle) & 3u;
    const fvft::SlotMeta m = h->meta[mid];
    if (m.width == 0 || m.height == 0 || m.stride < m.width * 4) { std::fprintf(stderr, "no frame yet\n"); return 1; }
    const uint8_t* px = static_cast<const uint8_t*>(p) + fvft::kHeaderBytes + size_t(mid) * h->slot_bytes;
    std::vector<uint8_t> rgb(size_t(m.width) * m.height * 3);
    for (uint32_t y = 0; y < m.height; y++)
        for (uint32_t x = 0; x < m.width; x++)
            for (int c = 0; c < 3; c++) rgb[(size_t(y) * m.width + x) * 3 + c] = px[size_t(y) * m.stride + x * 4 + c];
    FILE* o = std::fopen(argv[2], "wb");
    if (!o) return 1;
    std::fprintf(o, "P6\n%u %u\n255\n", m.width, m.height);
    std::fwrite(rgb.data(), 1, rgb.size(), o);
    std::fclose(o);
    std::printf("{\"frame\":%llu,\"context\":\"%s\",\"width\":%u,\"height\":%u,\"settled\":%s,\"cpu_submit_ms\":%.2f,\"capture_ms\":%.2f,\"triangles\":%llu}\n",
                (unsigned long long)m.frame_index, fvft::text(m.context, sizeof m.context).c_str(), m.width, m.height,
                (m.flags & fvft::SETTLED) ? "true" : "false", m.cpu_submit_ms, m.capture_ms, (unsigned long long)m.triangles);
    return 0;
}
