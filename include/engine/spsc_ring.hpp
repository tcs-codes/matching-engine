#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace engine {

// Portable busy-wait pause. On x86 this is `_mm_pause` -- a few dozen cycles
// that yield the pipeline to hyperthread siblings without a syscall, which is
// the right thing to do while spinning on a flag or ring slot. On ARM it's
// the `yield` hint; everywhere else we fall back to a scheduler yield.
inline void cpuPause() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// ---------------------------------------------------------------------------
// SPSCRing<T> -- lock-free single-producer / single-consumer ring buffer.
//
// The canonical design: a fixed array of slots plus two monotonically
// increasing counters, `head` (written by the producer) and `tail` (written
// by the consumer). The producer writes its slot then release-stores `head`;
// the consumer acquire-loads `head` to see the data, reads it, then
// release-stores `tail` to hand the slot back. Because each counter is owned
// by exactly one thread, there are no read-modify-write operations at all --
// just two loads/stores with the minimum ordering, which is as cheap as a
// lock-free structure gets.
//
// * try_push/try_pop are non-blocking: full/empty returns false.
// * Capacity must be a power of two (indices are masked, not divided).
// * T must be trivially copyable (the payload is copied into the slot).
//
// This is the ingestion primitive of Phase 4's single-writer design: one
// ring per producer thread, all drained by the one matching thread. It is
// deliberately NOT a general-purpose concurrent queue (no multi-producer
// support, no blocking variants) -- keeping it minimal is what keeps it fast.
// ---------------------------------------------------------------------------
template <typename T>
class SPSCRing {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCRing requires a trivially copyable payload");

    explicit SPSCRing(std::size_t capacity) : capacity_(capacity), buffer_(capacity) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SPSCRing capacity must be a non-zero power of two");
        }
    }

    // Producer side: returns false if the ring is full. Never blocks.
    bool try_push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed); // ours
        if (head - tail_.load(std::memory_order_acquire) >= capacity_) {
            return false; // full -- consumer hasn't reclaimed a slot
        }
        buffer_[head & (capacity_ - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side: returns false if the ring is empty. Never blocks.
    bool try_pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed); // ours
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty -- nothing committed by the producer
        }
        out = buffer_[tail & (capacity_ - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
    }
    bool empty() const { return size() == 0; }

private:
    std::size_t capacity_;
    std::vector<T> buffer_;

    // Each counter is padded to its own cache line so the producer and
    // consumer never false-share (relevant once the book is a separate thread
    // spinning on these -- Phase 4's raison d'etre).
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace engine
