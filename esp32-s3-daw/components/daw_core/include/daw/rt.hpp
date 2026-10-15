// SPDX-License-Identifier: GPL-3.0-or-later
// Real-time communication primitives between the control thread and the audio thread.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <new>
#include <type_traits>

#include "daw/platform.hpp"

namespace daw {

// Single-producer single-consumer ring of trivially copyable T, capacity N (power of two).
// push() on one thread, pop() on another; neither blocks nor allocates.
template <class T, uint32_t N, bool External>
struct QueueStorage {
    T data[N];
    bool ok() const { return true; }
};
template <class T, uint32_t N>
struct QueueStorage<T, N, true> {
    T *data = nullptr;
    QueueStorage()
    {
        data = static_cast<T *>(platform::alloc(sizeof(T) * N, 16, platform::Mem::Large));
        if (data)
            for (uint32_t i = 0; i < N; i++) new (&data[i]) T;
    }
    ~QueueStorage() { platform::free(data); }
    bool ok() const { return data != nullptr; }
};

template <class T, uint32_t N, bool External = false>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value, "queue payload must be trivially copyable");

public:
    bool ok() const { return storage_.ok(); }
    SpscQueue() = default;
    SpscQueue(const SpscQueue &) = delete;
    SpscQueue &operator=(const SpscQueue &) = delete;
    bool push(const T &v)
    {
        if constexpr (External) if (!ok()) return false;
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= N)
            return false;
        storage_.data[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T &v)
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t h = head_.load(std::memory_order_acquire);
        if (t == h)
            return false;
        v = storage_.data[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    uint32_t size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    static constexpr uint32_t capacity() { return N; }

private:
    QueueStorage<T, N, External> storage_;
    alignas(32) std::atomic<uint32_t> head_{0};
    alignas(32) std::atomic<uint32_t> tail_{0};
};

// float stored in a 32-bit atomic (lock-free on Xtensa and x86)
class AtomicFloat {
public:
    void store(float v)
    {
        uint32_t u;
        __builtin_memcpy(&u, &v, 4);
        bits_.store(u, std::memory_order_relaxed);
    }
    float load() const
    {
        uint32_t u = bits_.load(std::memory_order_relaxed);
        float v;
        __builtin_memcpy(&v, &u, 4);
        return v;
    }

private:
    std::atomic<uint32_t> bits_{0};
};

// typed allocation helpers on the platform allocator (control thread only)
template <class T, class... A>
T *createIn(platform::Mem cls, A &&...args)
{
    void *p = platform::alloc(sizeof(T), alignof(T) < 8 ? 8 : alignof(T), cls);
    return p ? new (p) T(static_cast<A &&>(args)...) : nullptr;
}
template <class T>
void destroyIn(T *p)
{
    if (p) {
        p->~T();
        platform::free(p);
    }
}

// std allocator in PSRAM (Large) for model containers: keeps the internal RAM for audio and DMA
template <class T>
struct LargeAlloc {
    using value_type = T;
    LargeAlloc() = default;
    template <class U>
    LargeAlloc(const LargeAlloc<U> &) {}
    T *allocate(size_t n)
    {
        void *p = platform::alloc(n * sizeof(T), alignof(T) < 8 ? 8 : alignof(T), platform::Mem::Large);
        if (!p)
            __builtin_trap();  // out of PSRAM: the control thread cannot continue safely
        return static_cast<T *>(p);
    }
    void deallocate(T *p, size_t) { platform::free(p); }
    template <class U>
    bool operator==(const LargeAlloc<U> &) const { return true; }
    template <class U>
    bool operator!=(const LargeAlloc<U> &) const { return false; }
};

}  // namespace daw
