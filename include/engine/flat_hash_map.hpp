#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine {

// SplitMix64 finalizer -- cheap avalanche mixing so hash tables indexed by
// integer keys don't degenerate when the keys share low bits (e.g. ids that
// are multiples of 64, or sequential ids hashed into a power-of-two table).
inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Hash functor for integer-like keys (the FastOrderBook cancel index).
struct IdHash {
    std::size_t operator()(std::uint64_t x) const { return static_cast<std::size_t>(mix64(x)); }
};

// ---------------------------------------------------------------------------
// FlatHashMap<Key, Value> -- minimal open-addressing hash map (Phase 6).
//
// Replaces std::unordered_map as FastOrderBook's cancel index (OrderId ->
// pool index). Why it's faster for this workload:
//
//   * Zero per-node allocation. The whole table is one contiguous vector,
//     reserved once; inserts never allocate after that (no node-based
//     buckets, no pointer chasing).
//   * Cache locality. Probe chains walk adjacent cache lines, and the value
//     lives next to its key in the same slot.
//   * No rehash on every growth, and growth is amortized (doubling).
//
// The invariants we take on ourselves, since the standard library no longer
// provides them for us:
//
//   * Power-of-two capacity, hard load factor 0.7 -> a free slot always
//     exists, so probe loops always terminate.
//   * Erase uses *backward-shift deletion* (Knuth 6.4 R): the trailing
//     cluster is shifted back over the hole instead of leaving a tombstone.
//     Tombstones would poison a cancel-heavy book (deleted slots stay in
//     probe chains forever, degrading lookups); backward shift keeps chains
//     tight.
//
// Deliberately NOT a general-purpose container: no iteration API, keys are
// probed by value, values must be default-constructible and copyable.
// Correctness is validated differentially against std::unordered_map in
// tests/test_flat_hash_map.cpp.
// ---------------------------------------------------------------------------
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class FlatHashMap {
public:
    explicit FlatHashMap(std::size_t reserve_count = 0) {
        if (reserve_count > 0) reserve(reserve_count);
    }

    Value* find(const Key& key) {
        if (slots_.empty()) return nullptr;
        std::size_t i = index(key);
        for (;;) {
            if (!slots_[i].used) return nullptr;
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
    }

    const Value* find(const Key& key) const {
        if (slots_.empty()) return nullptr;
        std::size_t i = index(key);
        for (;;) {
            if (!slots_[i].used) return nullptr;
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
    }

    bool contains(const Key& key) const { return find(key) != nullptr; }

    // Insert, or update if present; returns a reference to the stored value.
    Value& operator[](const Key& key) {
        if (slots_.empty() || (size_ + 1) * kLoadFactorDen > slots_.size() * kLoadFactorNum) {
            grow();
        }
        std::size_t i = index(key);
        while (slots_[i].used && slots_[i].key != key) {
            i = (i + 1) & mask_;
        }
        if (!slots_[i].used) {
            slots_[i].used = true;
            slots_[i].key = key;
            ++size_;
        }
        return slots_[i].value;
    }

    // Backward-shift deletion. Returns false if the key wasn't present.
    bool erase(const Key& key) {
        if (slots_.empty()) return false;
        std::size_t i = index(key);
        while (slots_[i].used && slots_[i].key != key) {
            i = (i + 1) & mask_;
        }
        if (!slots_[i].used) return false;

        --size_;
        std::size_t r = i; // the hole
        std::size_t p = (r + 1) & mask_;
        while (slots_[p].used) {
            const std::size_t h = index(slots_[p].key);
            if (!inInterval(h, r, p)) {
                // p's element can move into the hole without becoming
                // unreachable from its home slot.
                slots_[r] = slots_[p];
                slots_[p].used = false;
                r = p;
            }
            p = (p + 1) & mask_;
        }
        slots_[r].used = false;
        return true;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // Ensure capacity for `count` elements without rehashing.
    void reserve(std::size_t count) {
        std::size_t needed = 8;
        while (needed * kLoadFactorNum < count * kLoadFactorDen) {
            needed <<= 1;
        }
        if (needed > slots_.size()) {
            rehash(needed);
        }
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        bool used{false};
    };

    static constexpr std::size_t kLoadFactorNum = 7;
    static constexpr std::size_t kLoadFactorDen = 10; // 0.7

    std::vector<Slot> slots_;
    std::size_t mask_{0};
    std::size_t size_{0};

    std::size_t index(const Key& key) const { return Hash{}(key) & mask_; }

    void grow() {
        const std::size_t cap = slots_.empty() ? 8 : slots_.size() * 2;
        rehash(cap);
    }

    void rehash(std::size_t new_capacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        mask_ = new_capacity - 1;
        slots_.assign(new_capacity, Slot{});
        for (const Slot& s : old) {
            if (!s.used) continue;
            std::size_t i = Hash{}(s.key) & mask_;
            while (slots_[i].used) {
                i = (i + 1) & mask_;
            }
            slots_[i] = s;
        }
    }

    // Is `h` in the circular interval (r, p]? -- the positions after the
    // hole r, up to and including p. An element at p whose home is in that
    // interval must not move into the hole (it would become unreachable).
    static bool inInterval(std::size_t h, std::size_t r, std::size_t p) {
        if (r < p) return h > r && h <= p;
        if (r > p) return h > r || h <= p; // wraps around the end
        return false;
    }
};

} // namespace engine
