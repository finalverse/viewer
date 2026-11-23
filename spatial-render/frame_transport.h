// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// FINALVERSE native frame transport (Ω-XIX): finalverse.storm-frame-transport/1.
//
// One producer (the Storm spatial renderer) hands finished RGBA8 frames to one consumer
// (the Viewer) through a same-user POSIX shared-memory object holding three pixel slots
// and a lock-free triple buffer ("latest frame wins"):
//
//   producer writes slot `back`      consumer reads slot `front`      `middle` is shared
//   publish:  old = xchg(middle, back | FRESH); back = old & 3   (old FRESH => frame dropped)
//   acquire:  if (middle & FRESH) { old = xchg(middle, front); front = old & 3 }
//
// The producer only ever writes its `back` slot and the consumer only ever reads its
// `front` slot, so a partially written frame can never be read. There is no queue:
// a frame the consumer did not take before the next publish is counted as dropped
// (backpressure without blocking either side). `front` is stored in the header so a
// restarted Viewer resumes with the slot it owns; a consumer claim (pid) prevents two
// readers. The producer refreshes a heartbeat every loop; `state` turns CLOSED on clean
// shutdown and the object is unlinked. Discovery: the producer writes the owner-private
// OUTPUT/transport.json (schema, shm name, 128-bit nonce, geometry) once; the consumer
// checks that the mapped header carries the same nonce.
//
// Clocks: every *_ns value is CLOCK_UPTIME_RAW (same timebase in both processes).
#pragma once
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <stdlib.h>
#include <string>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fvft
{
inline constexpr const char* kSchema = "finalverse.storm-frame-transport/1";
inline constexpr uint32_t kMagic = 0x54465646u;  // "FVFT"
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kSlots = 3;
inline constexpr uint32_t kFresh = 4;
inline constexpr uint32_t kMaxWidth = 3840, kMaxHeight = 2160;
enum PixelFormat : uint32_t { RGBA8_UNORM = 1 };
enum ColorSpace : uint32_t { SRGB_DISPLAY_REFERRED = 1 };  // 8-bit encoded, shown as-is
enum State : uint32_t { STARTING = 0, LIVE = 1, CLOSED = 2 };
enum Flags : uint32_t { SETTLED = 1, LIVE_WORLD = 2 };

inline uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// Per-slot frame description, written by the producer before publishing the slot.
struct alignas(64) SlotMeta
{
    uint64_t sequence;         // transport sequence, 1.. per producer epoch
    uint64_t frame_index;      // renderer frame number
    uint64_t render_start_ns;  // loop start of the frame that produced these pixels
    uint64_t produced_ns;      // pixels complete in host memory, just before publish
    uint64_t control_sequence, source_revision, triangles, context_switches, refusals;
    uint32_t width, height, stride, objects;
    uint32_t flags, reserved;
    float capture_ms, switch_ms, cpu_submit_ms, gpu_wait_ms;
    double unit_to_metre, camera_distance_m;
    char context[40], definition_sha256[72], frame_id[64], units[16], transition[16], synchronization[16], backend[24];
    char last_error[160];
};

struct alignas(64) Header
{
    // Immutable after creation.
    uint32_t magic, version, header_bytes, slot_count;
    uint64_t slot_bytes, total_bytes;
    uint32_t max_width, max_height, pixel_format, color_space;
    uint64_t producer_pid, epoch_ns;
    uint8_t nonce[16];
    // Shared state (atomics).
    alignas(64) uint32_t middle;
    uint32_t front;         // the consumer's slot (written by the consumer only)
    uint32_t state;
    uint32_t consumer_pid;  // single-consumer claim
    uint64_t heartbeat_ns;  // producer liveness
    uint64_t published, dropped, consumed;
    SlotMeta meta[kSlots];
};
static_assert(sizeof(Header) < 4096, "header page");
inline constexpr size_t kHeaderBytes = 4096;

template <class T> inline T load(const T& v) { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
template <class T> inline void store(T& v, T x) { __atomic_store_n(&v, x, __ATOMIC_RELEASE); }
template <class T> inline T xchg(T& v, T x) { return __atomic_exchange_n(&v, x, __ATOMIC_ACQ_REL); }
template <class T> inline T add(T& v, T x) { return __atomic_add_fetch(&v, x, __ATOMIC_ACQ_REL); }

inline void copy_text(char* dst, size_t n, const std::string& s)
{
    const size_t k = std::min(n - 1, s.size());
    std::memcpy(dst, s.data(), k);
    dst[k] = 0;
}
inline std::string text(const char* src, size_t n) { return std::string(src, strnlen(src, n)); }
inline std::string hex(const uint8_t* p, size_t n)
{
    std::string o;
    for (size_t i = 0; i < n; i++) { o += "0123456789abcdef"[p[i] >> 4]; o += "0123456789abcdef"[p[i] & 15]; }
    return o;
}

struct Mapping
{
    void* base = MAP_FAILED;
    size_t bytes = 0;
    Header* h() const { return static_cast<Header*>(base); }
    uint8_t* slot(uint32_t i) const { return static_cast<uint8_t*>(base) + kHeaderBytes + size_t(i) * h()->slot_bytes; }
    bool ok() const { return base != MAP_FAILED; }
    void reset()
    {
        if (ok()) munmap(base, bytes);
        base = MAP_FAILED;
        bytes = 0;
    }
    ~Mapping() { reset(); }
};

// ------------------------------------------------------------------ producer
class Producer
{
public:
    // Creates /fvft.<pid>.<8 hex> (mode 0600, O_EXCL) sized for kSlots frames of max_w x max_h.
    bool create(uint32_t max_w, uint32_t max_h, std::string& error)
    {
        if (max_w == 0 || max_h == 0 || max_w > kMaxWidth || max_h > kMaxHeight) { error = "frame geometry outside the transport bounds"; return false; }
        uint8_t nonce[16];
        arc4random_buf(nonce, sizeof nonce);
        name_ = "/fvft." + std::to_string(getpid()) + "." + hex(nonce, 4);
        const uint64_t slot = uint64_t(max_w) * max_h * 4;
        const uint64_t total = kHeaderBytes + slot * kSlots;
        int fd = shm_open(name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { error = std::string("shm_open: ") + std::strerror(errno); return false; }
        if (ftruncate(fd, off_t(total)) != 0) { error = std::string("ftruncate: ") + std::strerror(errno); ::close(fd); shm_unlink(name_.c_str()); return false; }
        map_.base = mmap(nullptr, size_t(total), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        map_.bytes = size_t(total);
        ::close(fd);
        if (!map_.ok()) { error = std::string("mmap: ") + std::strerror(errno); shm_unlink(name_.c_str()); return false; }
        Header* h = map_.h();
        std::memset(h, 0, kHeaderBytes);
        h->magic = kMagic; h->version = kVersion; h->header_bytes = uint32_t(kHeaderBytes); h->slot_count = kSlots;
        h->slot_bytes = slot; h->total_bytes = total; h->max_width = max_w; h->max_height = max_h;
        h->pixel_format = RGBA8_UNORM; h->color_space = SRGB_DISPLAY_REFERRED;
        h->producer_pid = uint64_t(getpid()); h->epoch_ns = now_ns();
        std::memcpy(h->nonce, nonce, sizeof nonce);
        back_ = 0;
        store(h->middle, 1u);
        store(h->front, 2u);
        store(h->heartbeat_ns, now_ns());
        store(h->state, uint32_t(LIVE));
        return true;
    }
    const std::string& name() const { return name_; }
    std::string nonce_hex() const { return hex(map_.h()->nonce, 16); }
    // Discovery document for OUTPUT/transport.json.
    std::string discovery() const
    {
        const Header* h = map_.h();
        return std::string("{\"schema\":\"") + kSchema + "\",\"shm\":\"" + name_ + "\",\"nonce\":\"" + nonce_hex() + "\",\"slots\":" +
               std::to_string(kSlots) + ",\"max_width\":" + std::to_string(h->max_width) + ",\"max_height\":" + std::to_string(h->max_height) +
               ",\"pixel_format\":\"rgba8_unorm\",\"color_space\":\"srgb_display_referred\",\"bytes\":" + std::to_string(h->total_bytes) +
               ",\"producer_pid\":" + std::to_string(h->producer_pid) + ",\"clock\":\"CLOCK_UPTIME_RAW\"}";
    }
    // The slot the producer may write now (never the consumer's slot).
    uint8_t* back_pixels() { return map_.slot(back_); }
    SlotMeta& back_meta() { return map_.h()->meta[back_]; }
    uint32_t stride_for(uint32_t w) const { return w * 4; }
    bool fits(uint32_t w, uint32_t h) const { return w <= map_.h()->max_width && h <= map_.h()->max_height; }
    // Publishes the back slot; returns false if the previous published frame was never taken.
    bool publish()
    {
        Header* h = map_.h();
        SlotMeta& m = h->meta[back_];
        m.sequence = add(h->published, uint64_t(1));
        m.produced_ns = now_ns();
        const uint32_t old = xchg(h->middle, back_ | kFresh);
        back_ = old & 3u;
        if (old & kFresh) { add(h->dropped, uint64_t(1)); return false; }
        return true;
    }
    void heartbeat() { store(map_.h()->heartbeat_ns, now_ns()); }
    void close()
    {
        if (!map_.ok()) return;
        store(map_.h()->state, uint32_t(CLOSED));
        shm_unlink(name_.c_str());
        map_.reset();
    }
    ~Producer() { close(); }

private:
    Mapping map_;
    std::string name_;
    uint32_t back_ = 0;
};

// ------------------------------------------------------------------ consumer
struct Acquired
{
    const uint8_t* pixels = nullptr;  // valid until the next acquire()
    SlotMeta meta{};
    uint64_t acquired_ns = 0;
};

class Consumer
{
public:
    // Attaches to the object named in transport.json and verifies the nonce and geometry.
    bool attach(const std::string& shm, const std::string& nonce, std::string& error)
    {
        detach();
        if (shm.size() > 30 || shm.rfind("/fvft.", 0) != 0) { error = "transport name refused"; return false; }
        int fd = shm_open(shm.c_str(), O_RDWR, 0);
        if (fd < 0) { error = std::string("shm_open: ") + std::strerror(errno); return false; }
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_uid != getuid() || (st.st_mode & 077) != 0 || st.st_size < off_t(kHeaderBytes))
        { error = "transport object must be same-user, private and sized"; ::close(fd); return false; }
        map_.base = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        map_.bytes = size_t(st.st_size);
        ::close(fd);
        if (!map_.ok()) { error = std::string("mmap: ") + std::strerror(errno); return false; }
        const Header* h = map_.h();
        const uint64_t slot = uint64_t(h->max_width) * h->max_height * 4;
        if (h->magic != kMagic || h->version != kVersion || h->header_bytes != kHeaderBytes || h->slot_count != kSlots ||
            h->pixel_format != RGBA8_UNORM || h->max_width == 0 || h->max_height == 0 || h->max_width > kMaxWidth ||
            h->max_height > kMaxHeight || h->slot_bytes != slot || h->total_bytes != kHeaderBytes + slot * kSlots ||
            h->total_bytes > map_.bytes || hex(h->nonce, 16) != nonce)
        { error = "transport header does not match its discovery document"; map_.reset(); return false; }
        // Single consumer: take the claim if it is free or its holder is gone.
        Header* w = map_.h();
        uint32_t holder = load(w->consumer_pid);
        const uint32_t me = uint32_t(getpid());
        if (holder != 0 && holder != me && kill(pid_t(holder), 0) == 0) { error = "another consumer holds this transport"; map_.reset(); return false; }
        if (!__atomic_compare_exchange_n(&w->consumer_pid, &holder, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        { error = "consumer claim raced"; map_.reset(); return false; }
        front_ = load(w->front) & 3u;
        nonce_ = nonce;
        return true;
    }
    bool attached() const { return map_.ok(); }
    const std::string& nonce() const { return nonce_; }
    // Takes the newest published frame, if any. Never blocks.
    bool acquire(Acquired& out)
    {
        if (!map_.ok()) return false;
        Header* h = map_.h();
        if (!(load(h->middle) & kFresh)) return false;
        const uint32_t old = xchg(h->middle, front_);
        front_ = old & 3u;
        store(h->front, front_);
        add(h->consumed, uint64_t(1));
        out.meta = h->meta[front_];
        const uint64_t need = uint64_t(out.meta.stride) * out.meta.height;
        if (out.meta.width == 0 || out.meta.height == 0 || out.meta.width > h->max_width || out.meta.height > h->max_height ||
            out.meta.stride < out.meta.width * 4 || need > h->slot_bytes)
            return false;
        out.pixels = map_.slot(front_);
        out.acquired_ns = now_ns();
        return true;
    }
    // Producer state as seen by the consumer.
    uint32_t state() const { return map_.ok() ? load(map_.h()->state) : uint32_t(CLOSED); }
    uint64_t heartbeat_age_ns() const { return map_.ok() ? now_ns() - load(map_.h()->heartbeat_ns) : UINT64_MAX; }
    bool producer_alive() const { return map_.ok() && kill(pid_t(map_.h()->producer_pid), 0) == 0; }
    uint64_t published() const { return map_.ok() ? load(map_.h()->published) : 0; }
    uint64_t dropped() const { return map_.ok() ? load(map_.h()->dropped) : 0; }
    void detach()
    {
        if (map_.ok())
        {
            uint32_t me = uint32_t(getpid());
            __atomic_compare_exchange_n(&map_.h()->consumer_pid, &me, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        map_.reset();
        nonce_.clear();
    }
    ~Consumer() { detach(); }

private:
    Mapping map_;
    uint32_t front_ = 2;
    std::string nonce_;
};
}  // namespace fvft
