#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "engine/fast_order_book.hpp"
#include "engine/market_data.hpp"
#include "engine/order.hpp"
#include "engine/spsc_ring.hpp"
#include "engine/trade.hpp"
#include "engine/types.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// Phase 4: the single-writer design.
//
// The matching engine from Phases 1-3 is single-threaded by construction.
// Phase 4 keeps it that way and adds the concurrency *around* it, the way
// real low-latency systems do:
//
//   * One dedicated "matching thread" owns FastOrderBook. No locks, no
//     atomics on the book itself -- the fastest lock is no lock.
//   * Producers (client threads) never touch the book. Each producer has a
//     lock-free SPSCRing<OrderOp> of its own (SPSC: one writer, and the
//     matching thread as the sole reader of every ring).
//   * The matching thread busy-spins polling the rings -- no condition
//     variables, no futex wakeups, no scheduler latency -- and applies ops
//     to the book in FIFO order per ring.
//   * Results come back the same lock-free way: each producer slot has a
//     response "mailbox" (an atomic sequence number plus a result buffer).
//     The matcher fills the buffer, then publishes the op's sequence number;
//     the producer spins until its sequence number appears.
//
// The op/result plumbing is deliberately minimal and allocation-free on the
// producer side. The matcher builds one std::vector<Fill> per mutating op
// that fills -- noted as a candidate for pooling/reuse in Phase 5/6.
//
// THREAD CONTRACT
// ---------------
// * Producer threads claim a slot on first use (thread-local). The number of
//   *concurrent* producer threads must be <= producer_slots or submission
//   throws. Each producer thread may use any number of the synchronous /
//   pipelined calls below; ops from one thread are applied in submission
//   order.
// * stop() (and the destructor) join the matching thread. Call stop() only
//   after every producer thread has finished; submitting after stop() hangs.
//   Once stopped, book() gives direct (now single-threaded) access.
// ---------------------------------------------------------------------------

// The unit of work flowing from a producer to the matching thread. POD by
// design so it can live in the SPSCRing without allocation or ownership.
enum class OpType : std::uint8_t {
    AddLimit,
    AddIOC,
    Cancel,
    CancelReplace,
    QueryBestBid,
    QueryBestAsk,
    QueryIsCrossed,
    QueryContains,
    QueryQuantityAt,
    QueryGet,
};

struct OrderOp {
    OpType type{OpType::AddLimit};
    std::uint64_t seq{0}; // per-producer sequence, used to correlate responses
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{}; // qty for adds; new qty (and price) for cancel-replace
};

// What the matching thread publishes back for one op. Only the fields
// relevant to the op's type are meaningful (e.g. fills for mutations,
// price_value for best-bid/ask queries).
struct OpResult {
    std::vector<Fill> fills;
    bool ok{false};                 // cancel/replace success, query found
    bool preserved_priority{false}; // cancel-replace: priority kept?
    bool bool_value{false};         // isCrossed / contains queries
    Price price_value{};            // bestBid / bestAsk queries
    Quantity quantity_value{};      // quantityAt query
    Order order_value{};            // get query
};

class ConcurrentBook {
public:
    // producer_slots: max concurrent producer threads (one SPSC ring each).
    // ring_capacity:  power-of-two ring size per producer (ops, not bytes).
    // matcher_core:   core to pin the matching thread to, -1 = don't pin.
    // min/max_price, order_capacity: forwarded to the underlying FastOrderBook.
    // `sink` (optional): a market-data tap called by the matcher thread after
    // every mutating op. Install it at construction; the pointer must outlive
    // the book. See market_data.hpp.
    explicit ConcurrentBook(std::size_t producer_slots = 16, std::size_t ring_capacity = 1024,
                            int matcher_core = -1, Price min_price = 0, Price max_price = 100000,
                            std::size_t order_capacity = 1u << 20,
                            MarketDataSink* sink = nullptr);
    ~ConcurrentBook();

    ConcurrentBook(const ConcurrentBook&) = delete;
    ConcurrentBook& operator=(const ConcurrentBook&) = delete;

    // --- Synchronous operations (thread-safe) ------------------------------
    // Same semantics as FastOrderBook: submit, wait for the matcher to apply
    // the op, return the result. The wait is a busy-spin on the response
    // mailbox, so round-trip latency is the meaningful number here.

    std::vector<Fill> addOrder(OrderId id, Side side, Price price, Quantity quantity,
                               OrderType type = OrderType::Limit);
    std::vector<Fill> addLimitOrder(OrderId id, Side side, Price price, Quantity quantity);
    std::vector<Fill> addIOCOrder(OrderId id, Side side, Price price, Quantity quantity);
    CancelResult cancel(OrderId id);
    CancelReplaceOutcome cancelReplace(OrderId id, Price new_price, Quantity new_quantity);

    std::optional<Price> bestBid();
    std::optional<Price> bestAsk();
    bool isCrossed();
    bool contains(OrderId id);
    Quantity quantityAt(Side side, Price price);
    std::optional<Order> get(OrderId id);

    // --- Pipelined ingestion (for high-throughput producers) ---------------
    // Push an op and return immediately; the matcher applies it in order.
    // Individual results are intentionally not retrievable (the response
    // mailbox only holds the most recent one) -- use the synchronous API
    // when you need fills. waitForLastOp() blocks until every op this thread
    // has enqueued so far has been applied; it's the batching primitive the
    // throughput benchmark uses to beat per-op round-trip latency.
    void enqueue(OrderId id, Side side, Price price, Quantity quantity,
                 OrderType type = OrderType::Limit);
    void enqueueCancel(OrderId id);
    void waitForLastOp();

    // --- Advanced / test hook ----------------------------------------------
    // Submit a raw op (seq is stamped internally) and return the matcher's
    // result. The high-level API discards details this exposes: for Cancel
    // and CancelReplace, `quantity_value` is the exact resting volume the op
    // released, computed atomically by the matcher -- useful for accounting
    // that must stay correct with concurrent producers. The returned
    // reference is invalidated by the next op submitted from this thread.
    const OpResult& submitOp(OrderOp op);

    // Stops the matching thread (idempotent). Called by the destructor; call
    // it explicitly before touching book(). Contract: no producer thread may
    // be submitting when stop() is called.
    void stop();

    // Direct access to the underlying book -- only valid after stop().
    FastOrderBook& book() { return book_; }
    const FastOrderBook& book() const { return book_; }

private:
    struct ProducerSlot {
        explicit ProducerSlot(std::size_t ring_capacity) : in(ring_capacity) {}
        SPSCRing<OrderOp> in;
        struct Response {
            // Matcher release-stores the applied op's seq after filling
            // `result`; producer acquire-loads it before reading `result`.
            std::atomic<std::uint64_t> seq{0};
            OpResult result;
        } response;
    };

    FastOrderBook book_;
    MarketDataSink* sink_;
    std::vector<std::unique_ptr<ProducerSlot>> slots_;
    std::atomic<int> next_slot_{0};
    std::atomic<bool> running_{true};
    std::thread matcher_;

    void matcherLoop();
    void applyOp(ProducerSlot& slot, const OrderOp& op);
    int slotIndex();
    const OpResult& submit(const OrderOp& op);
};

} // namespace engine
