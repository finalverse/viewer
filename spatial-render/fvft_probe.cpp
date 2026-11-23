// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// fvft-probe OUTPUT_DIR: read-only diagnostic view of a finalverse.storm-frame-transport/1
// object (never claims the consumer slot, never writes): producer state, heartbeat age,
// counters and the metadata of the most recently published frame, as JSON.
#include "frame_transport.h"
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/mman.h>

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage: fvft-probe OUTPUT_DIR\n"); return 2; }
    std::ifstream f(std::string(argv[1]) + "/transport.json");
    std::stringstream ss; ss << f.rdbuf();
    std::smatch m1, m2;
    const std::string d = ss.str();
    if (!std::regex_search(d, m1, std::regex("\"shm\":\"(/fvft\\.[0-9a-f.]+)\"")) || !std::regex_search(d, m2, std::regex("\"nonce\":\"([0-9a-f]{32})\"")))
    { std::printf("{\"transport\":\"absent\"}\n"); return 1; }
    int fd = shm_open(m1[1].str().c_str(), O_RDONLY, 0);
    if (fd < 0) { std::printf("{\"transport\":\"unlinked\"}\n"); return 1; }
    struct stat st{};
    fstat(fd, &st);
    void* p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 1;
    const auto* h = static_cast<const fvft::Header*>(p);
    if (h->magic != fvft::kMagic || fvft::hex(h->nonce, 16) != m2[1].str()) { std::printf("{\"transport\":\"mismatch\"}\n"); return 1; }
    const uint32_t mid = fvft::load(h->middle) & 3u;
    const fvft::SlotMeta& m = h->meta[mid];
    std::printf("{\"transport\":\"%s\",\"state\":%u,\"heartbeat_age_ms\":%.1f,\"producer_pid\":%llu,\"consumer_pid\":%u,\"published\":%llu,\"dropped\":%llu,"
                "\"consumed\":%llu,\"last\":{\"sequence\":%llu,\"frame\":%llu,\"context\":\"%s\",\"control_sequence\":%llu,\"width\":%u,\"height\":%u,"
                "\"settled\":%s,\"capture_ms\":%.2f,\"triangles\":%llu}}\n",
                fvft::kSchema, fvft::load(h->state), double(fvft::now_ns() - fvft::load(h->heartbeat_ns)) / 1e6, (unsigned long long)h->producer_pid,
                fvft::load(h->consumer_pid), (unsigned long long)fvft::load(h->published), (unsigned long long)fvft::load(h->dropped),
                (unsigned long long)fvft::load(h->consumed), (unsigned long long)m.sequence, (unsigned long long)m.frame_index,
                fvft::text(m.context, sizeof m.context).c_str(), (unsigned long long)m.control_sequence, m.width, m.height,
                (m.flags & fvft::SETTLED) ? "true" : "false", m.capture_ms, (unsigned long long)m.triangles);
    return 0;
}
