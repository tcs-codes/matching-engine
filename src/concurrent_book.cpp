#include "engine/concurrent_book.hpp"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace engine {

namespace {

// Best-effort core pinning. Returns false if pinning isn't supported or the
// core doesn't exist; callers ignore the result (pinning is a latency
// optimization, never a correctness requirement).
bool pinCurrentThreadToCore(int core) {
    if (core < 0) return false;
#if defined(_WIN32)
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(core), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

// Per-thread monotonic sequence counter; also used to hand out producer
// slots (one thread, one slot, so per-thread seqs are unique per slot).
std::uint64_t& threadSeq() {
    static thread_local std::uint64_t seq = 0;
    return seq;
}

// Sequence of the most recent op this thread enqueued (for waitForLastOp).
std::uint64_t& threadLastEnqueuedSeq() {
    static thread_local std::uint64_t seq = 0;
    return seq;
}

} // namespace

ConcurrentBook::ConcurrentBook(std::size_t producer_slots, std::size_t ring_capacity,
                               int matcher_core, Price min_price, Price max_price,
                               std::size_t order_capacity, MarketDataSink* sink)
    : book_(min_price, max_price, order_capacity), sink_(sink) {
    slots_.reserve(producer_slots);
    for (std::size_t i = 0; i < producer_slots; ++i) {
        slots_.push_back(std::make_unique<ProducerSlot>(ring_capacity));
    }
    matcher_ = std::thread([this, matcher_core] {
        if (pinCurrentThreadToCore(matcher_core)) {
            // pinned -- nothing to do; pinning is best-effort anyway
        }
        matcherLoop();
    });
}

ConcurrentBook::~ConcurrentBook() {
    stop();
}

void ConcurrentBook::stop() {
    running_.store(false, std::memory_order_release);
    if (matcher_.joinable()) {
        matcher_.join();
    }
}

int ConcurrentBook::slotIndex() {
    static thread_local int idx = -1;
    if (idx == -1) {
        const int claimed = next_slot_.fetch_add(1, std::memory_order_relaxed);
        if (claimed < 0 || claimed >= static_cast<int>(slots_.size())) {
            throw std::runtime_error("more concurrent producer threads than allocated slots (" +
                                     std::to_string(slots_.size()) + ")");
        }
        idx = claimed;
    }
    return idx;
}

// The matching thread's main loop. Polls every producer ring in turn, drains
// whatever is there, and only pauses when there was nothing to do. No
// condition variables, no OS wakeups -- pure busy-spin, which is what makes
// ingestion latency a handful of cache misses instead of a scheduler round
// trip. A cpuPause() backoff keeps it from hammering the pipeline when idle.
void ConcurrentBook::matcherLoop() {
    for (;;) {
        bool did_work = false;
        for (const auto& slot_ptr : slots_) {
            ProducerSlot& slot = *slot_ptr;
            OrderOp op;
            while (slot.in.try_pop(op)) {
                applyOp(slot, op);
                did_work = true;
            }
        }

        if (!running_.load(std::memory_order_acquire)) {
            // Stop requested: drain whatever ops are already in the rings so
            // in-flight work completes, then exit. Producers must have
            // stopped by now (see the thread contract on ConcurrentBook).
            for (const auto& slot_ptr : slots_) {
                ProducerSlot& slot = *slot_ptr;
                OrderOp op;
                while (slot.in.try_pop(op)) {
                    applyOp(slot, op);
                }
            }
            return;
        }

        if (!did_work) {
            cpuPause();
        }
    }
}

void ConcurrentBook::applyOp(ProducerSlot& slot, const OrderOp& op) {
    OpResult& r = slot.response.result;
    switch (op.type) {
        case OpType::AddLimit:
            r.fills = book_.addLimitOrder(op.id, op.side, op.price, op.quantity);
            r.ok = true;
            break;
        case OpType::AddIOC:
            r.fills = book_.addIOCOrder(op.id, op.side, op.price, op.quantity);
            r.ok = true;
            break;
        case OpType::Cancel: {
            // Report the exact resting quantity released, computed atomically
            // with the removal (the matcher is single-threaded, so there is
            // no get-then-cancel race here like there is on the producer
            // side). Accounting clients rely on this under concurrency.
            auto before = book_.get(op.id);
            r.ok = book_.cancel(op.id) == CancelResult::Cancelled;
            r.quantity_value = (r.ok && before.has_value()) ? before->quantity : 0;
            break;
        }
        case OpType::CancelReplace: {
            auto before = book_.get(op.id);
            if (!before.has_value()) {
                r.ok = false;
                r.quantity_value = 0;
                break;
            }
            const auto outcome = book_.cancelReplace(op.id, op.price, op.quantity);
            r.ok = outcome.result == ReplaceResult::Replaced;
            r.preserved_priority = outcome.preserved_priority;
            r.fills = std::move(outcome.fills);
            // Released volume: the shrink delta on the priority-preserving
            // path, the whole old quantity on the priority-losing path (the
            // old order is cancelled, then new_qty is inserted fresh).
            r.quantity_value = outcome.preserved_priority
                                   ? (before->quantity - op.quantity)
                                   : before->quantity;
            break;
        }
        case OpType::QueryBestBid: {
            const auto v = book_.bestBid();
            r.ok = v.has_value();
            r.price_value = v.value_or(0);
            break;
        }
        case OpType::QueryBestAsk: {
            const auto v = book_.bestAsk();
            r.ok = v.has_value();
            r.price_value = v.value_or(0);
            break;
        }
        case OpType::QueryIsCrossed:
            r.bool_value = book_.isCrossed();
            break;
        case OpType::QueryContains:
            r.bool_value = book_.contains(op.id);
            break;
        case OpType::QueryQuantityAt:
            r.quantity_value = book_.quantityAt(op.side, op.price);
            break;
        case OpType::QueryGet: {
            const auto v = book_.get(op.id);
            r.ok = v.has_value();
            if (v.has_value()) {
                r.order_value = *v;
            }
            break;
        }
    }

    // Market data tap: the matcher is the single source of truth, so emit
    // the fills this op produced and the resulting best bid/ask here. Only
    // mutating ops can produce fills; Cancel never touches r.fills, so it is
    // explicitly excluded (r.fills would otherwise hold a stale value).
    if (sink_ != nullptr) {
        const bool is_mutation = op.type == OpType::AddLimit || op.type == OpType::AddIOC ||
                                 op.type == OpType::Cancel || op.type == OpType::CancelReplace;
        if (is_mutation) {
            const auto bb = book_.bestBid();
            const auto ba = book_.bestAsk();
            const Price bid = bb.value_or(0);
            const Price ask = ba.value_or(0);
            if (op.type != OpType::Cancel) {
                for (const auto& f : r.fills) {
                    sink_->onTrade(f, bid, ask);
                }
            }
            sink_->onBookUpdate(bid, ask);
        }
    }

    // Publish: everything above (the result buffer) must be visible to the
    // producer before it sees the seq, hence the release store.
    slot.response.seq.store(op.seq, std::memory_order_release);
}

// Submit one op synchronously: push into this thread's ring, then spin on
// the response mailbox until the matcher has applied *this* op. The `>=`
// comparison (rather than ==) is required for the pipelined path, where the
// matcher may have applied ops past the one we're waiting on.
const OpResult& ConcurrentBook::submit(const OrderOp& op) {
    ProducerSlot& s = *slots_[static_cast<std::size_t>(slotIndex())];
    while (!s.in.try_push(op)) {
        cpuPause(); // ring full -- matcher is behind, wait for a slot
    }
    const std::uint64_t seq = op.seq;
    while (s.response.seq.load(std::memory_order_acquire) < seq) {
        cpuPause();
    }
    return s.response.result;
}

const OpResult& ConcurrentBook::submitOp(OrderOp op) {
    op.seq = ++threadSeq();
    return submit(op);
}

std::vector<Fill> ConcurrentBook::addOrder(OrderId id, Side side, Price price, Quantity quantity,
                                           OrderType type) {
    const OrderOp op{type == OrderType::IOC ? OpType::AddIOC : OpType::AddLimit, ++threadSeq(), id,
                     side, price, quantity};
    return submit(op).fills;
}

std::vector<Fill> ConcurrentBook::addLimitOrder(OrderId id, Side side, Price price,
                                                Quantity quantity) {
    return addOrder(id, side, price, quantity, OrderType::Limit);
}

std::vector<Fill> ConcurrentBook::addIOCOrder(OrderId id, Side side, Price price,
                                              Quantity quantity) {
    return addOrder(id, side, price, quantity, OrderType::IOC);
}

CancelResult ConcurrentBook::cancel(OrderId id) {
    const OrderOp op{OpType::Cancel, ++threadSeq(), id, Side::Buy, 0, 0};
    return submit(op).ok ? CancelResult::Cancelled : CancelResult::NotFound;
}

CancelReplaceOutcome ConcurrentBook::cancelReplace(OrderId id, Price new_price,
                                                   Quantity new_quantity) {
    const OrderOp op{OpType::CancelReplace, ++threadSeq(), id, Side::Buy, new_price, new_quantity};
    const OpResult& r = submit(op);
    CancelReplaceOutcome out;
    out.result = r.ok ? ReplaceResult::Replaced : ReplaceResult::NotFound;
    out.preserved_priority = r.preserved_priority;
    out.fills = r.fills;
    return out;
}

std::optional<Price> ConcurrentBook::bestBid() {
    const OrderOp op{OpType::QueryBestBid, ++threadSeq(), 0, Side::Buy, 0, 0};
    const OpResult& r = submit(op);
    if (!r.ok) return std::nullopt;
    return r.price_value;
}

std::optional<Price> ConcurrentBook::bestAsk() {
    const OrderOp op{OpType::QueryBestAsk, ++threadSeq(), 0, Side::Buy, 0, 0};
    const OpResult& r = submit(op);
    if (!r.ok) return std::nullopt;
    return r.price_value;
}

bool ConcurrentBook::isCrossed() {
    const OrderOp op{OpType::QueryIsCrossed, ++threadSeq(), 0, Side::Buy, 0, 0};
    return submit(op).bool_value;
}

bool ConcurrentBook::contains(OrderId id) {
    const OrderOp op{OpType::QueryContains, ++threadSeq(), id, Side::Buy, 0, 0};
    return submit(op).bool_value;
}

Quantity ConcurrentBook::quantityAt(Side side, Price price) {
    const OrderOp op{OpType::QueryQuantityAt, ++threadSeq(), 0, side, price, 0};
    return submit(op).quantity_value;
}

std::optional<Order> ConcurrentBook::get(OrderId id) {
    const OrderOp op{OpType::QueryGet, ++threadSeq(), id, Side::Buy, 0, 0};
    const OpResult& r = submit(op);
    if (!r.ok) return std::nullopt;
    return r.order_value;
}

void ConcurrentBook::enqueue(OrderId id, Side side, Price price, Quantity quantity,
                             OrderType type) {
    ProducerSlot& s = *slots_[static_cast<std::size_t>(slotIndex())];
    const OrderOp op{type == OrderType::IOC ? OpType::AddIOC : OpType::AddLimit, ++threadSeq(), id,
                     side, price, quantity};
    threadLastEnqueuedSeq() = op.seq;
    while (!s.in.try_push(op)) {
        cpuPause();
    }
}

void ConcurrentBook::enqueueCancel(OrderId id) {
    ProducerSlot& s = *slots_[static_cast<std::size_t>(slotIndex())];
    const OrderOp op{OpType::Cancel, ++threadSeq(), id, Side::Buy, 0, 0};
    threadLastEnqueuedSeq() = op.seq;
    while (!s.in.try_push(op)) {
        cpuPause();
    }
}

void ConcurrentBook::waitForLastOp() {
    ProducerSlot& s = *slots_[static_cast<std::size_t>(slotIndex())];
    const std::uint64_t seq = threadLastEnqueuedSeq();
    while (s.response.seq.load(std::memory_order_acquire) < seq) {
        cpuPause();
    }
}

} // namespace engine
