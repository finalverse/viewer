// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// finalverse.storm-frame-transport/1 conformance: a forked producer publishes frames
// whose every byte encodes the sequence; the consumer checks it never sees a torn or
// stale-reordered frame, that drops are counted instead of queued, that a restarted
// consumer resumes on its own slot, that a second consumer is refused, that a forged
// nonce is refused and that a clean producer shutdown is observed.
#include "frame_transport.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(c, what) do { if (!(c)) { std::fprintf(stderr, "FAIL %s\n", what); failures++; } else std::printf("PASS %s\n", what); } while (0)

int main()
{
    const uint32_t W = 640, H = 360, N = 3000;
    char dir[] = "/tmp/fvft-test-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string disc = std::string(dir) + "/transport.json";
    int ready[2];
    if (pipe(ready)) return 2;
    pid_t child = fork();
    if (child == 0)
    {
        fvft::Producer p;
        std::string e;
        if (!p.create(W, H, e)) { std::fprintf(stderr, "%s\n", e.c_str()); _exit(3); }
        { std::ofstream(disc) << p.name() << "\n" << p.nonce_hex() << "\n"; }
        char b = 1;
        (void)!write(ready[1], &b, 1);
        usleep(100000);
        for (uint32_t i = 1; i <= N; i++)
        {
            uint8_t* px = p.back_pixels();
            std::memset(px, int(i & 0xff), size_t(W) * H * 4);
            auto& m = p.back_meta();
            m.width = W; m.height = H; m.stride = W * 4; m.frame_index = i;
            p.heartbeat();
            p.publish();
            if (i % 7 == 0) usleep(300);
        }
        usleep(200000);
        p.close();
        _exit(0);
    }
    char b;
    (void)!read(ready[0], &b, 1);
    std::string name, nonce;
    { std::ifstream f(disc); f >> name >> nonce; }

    fvft::Consumer bad;
    std::string e;
    CHECK(!bad.attach(name, std::string(32, '0'), e), "forged nonce refused");
    fvft::Consumer c;
    CHECK(c.attach(name, nonce, e), "consumer attaches with the discovery nonce");
    pid_t other = fork();
    if (other == 0) { fvft::Consumer second; std::string e2; _exit(second.attach(name, nonce, e2) ? 1 : 0); }
    int otherStatus = 0;
    waitpid(other, &otherStatus, 0);
    CHECK(WIFEXITED(otherStatus) && WEXITSTATUS(otherStatus) == 0, "second consumer process refused while the first holds the claim");

    uint64_t last = 0, got = 0, torn = 0, reordered = 0;
    bool restarted = false;
    const uint64_t deadline = fvft::now_ns() + 20'000'000'000ull;
    while (fvft::now_ns() < deadline)
    {
        fvft::Acquired a;
        if (c.acquire(a))
        {
            got++;
            const uint8_t want = uint8_t(a.meta.frame_index & 0xff);
            for (size_t k = 0; k < size_t(W) * H * 4; k += 4093) if (a.pixels[k] != want) { torn++; break; }
            if (a.pixels[size_t(W) * H * 4 - 1] != want) torn++;
            if (a.meta.sequence <= last) reordered++;
            last = a.meta.sequence;
            usleep(500);  // a slow consumer: frames must be dropped, not queued
            if (!restarted && got == 200)
            {
                c.detach();  // consumer restart resumes on its own slot
                restarted = c.attach(name, nonce, e);
            }
        }
        if (c.state() == fvft::CLOSED) break;
        usleep(50);
    }
    const uint64_t published = c.published(), dropped = c.dropped();
    int status = 0;
    waitpid(child, &status, 0);
    std::printf("published=%llu consumed=%llu dropped=%llu last_seq=%llu\n", (unsigned long long)published, (unsigned long long)got,
                (unsigned long long)dropped, (unsigned long long)last);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "producer exited cleanly");
    CHECK(published == N, "every frame published");
    CHECK(torn == 0, "no torn frame observed");
    CHECK(reordered == 0, "sequence strictly increasing");
    CHECK(restarted, "consumer re-attached after restart");
    CHECK(dropped > 0 && got + dropped >= N - 2, "slow consumer: frames dropped (bounded), none queued");
    CHECK(last == N, "newest frame delivered");
    CHECK(c.state() == fvft::CLOSED || !c.attached(), "clean producer shutdown observed");
    fvft::Consumer late;
    CHECK(!late.attach(name, nonce, e), "name unlinked after shutdown");
    std::remove(disc.c_str());
    rmdir(dir);
    std::printf(failures ? "frame transport: %d FAILED\n" : "frame transport: all checks passed\n", failures);
    return failures ? 1 : 0;
}
