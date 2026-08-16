#include <gtest/gtest.h>

#include <sstream>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/order_stream.hpp"

using namespace engine;

namespace {

OpStreamGenerator::Config makeConfig(std::uint64_t seed, std::size_t count = 5000) {
    return OpStreamGenerator::Config{.seed = seed, .count = count, .mid = 10000, .spread = 50};
}

std::vector<OrderOp> generate(std::uint64_t seed, std::size_t count = 5000) {
    OpStreamGenerator gen(makeConfig(seed, count));
    std::vector<OrderOp> ops;
    for (auto op = gen.next(); op; op = gen.next()) {
        ops.push_back(*op);
    }
    return ops;
}

} // namespace

// ---------------------------------------------------------------------------
// OpStreamGenerator: determinism (the same seed must reproduce the stream
// bit-for-bit -- replay and benchmarking depend on it), a sane op mix, and
// dump/parse round-trip fidelity.
// ---------------------------------------------------------------------------

TEST(OrderStream, SameSeedProducesIdenticalStream) {
    const auto a = generate(42);
    const auto b = generate(42);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].type, b[i].type) << "op " << i;
        EXPECT_EQ(a[i].id, b[i].id) << "op " << i;
        EXPECT_EQ(a[i].side, b[i].side) << "op " << i;
        EXPECT_EQ(a[i].price, b[i].price) << "op " << i;
        EXPECT_EQ(a[i].quantity, b[i].quantity) << "op " << i;
    }
}

TEST(OrderStream, DifferentSeedProducesDifferentStream) {
    const auto a = generate(1);
    const auto b = generate(2);
    ASSERT_EQ(a.size(), b.size());
    bool differs = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].id != b[i].id || a[i].price != b[i].price) {
            differs = true;
            break;
        }
    }
    EXPECT_TRUE(differs);
}

TEST(OrderStream, ProducesAllOpTypesAndRightCount) {
    const auto ops = generate(7, 20000);
    EXPECT_EQ(ops.size(), 20000u);

    std::size_t limits = 0, iocs = 0, cancels = 0, replaces = 0;
    for (const auto& op : ops) {
        switch (op.type) {
            case OpType::AddLimit: ++limits; break;
            case OpType::AddIOC: ++iocs; break;
            case OpType::Cancel: ++cancels; break;
            case OpType::CancelReplace: ++replaces; break;
            default: break;
        }
    }
    // The mix is add-heavy (60% limits, 10% IOC), with cancels/replaces on
    // top; every type must appear or the stream is degenerate.
    EXPECT_GT(limits, 5000u);
    EXPECT_GT(iocs, 500u);
    EXPECT_GT(cancels, 1000u);
    EXPECT_GT(replaces, 500u);

    // Generated prices must stay inside the default book range (0, 100000).
    for (const auto& op : ops) {
        if (op.type == OpType::AddLimit || op.type == OpType::AddIOC) {
            EXPECT_GE(op.price, 0);
            EXPECT_LE(op.price, 100000);
        }
    }
}

TEST(OrderStream, DumpParseRoundTripIsExact) {
    const auto ops = generate(123);
    std::ostringstream os;
    dumpOps(os, ops);
    std::istringstream is(os.str());

    const auto parsed = loadOps(is);
    ASSERT_EQ(parsed.size(), ops.size());
    for (std::size_t i = 0; i < ops.size(); ++i) {
        EXPECT_EQ(parsed[i].type, ops[i].type) << "op " << i;
        EXPECT_EQ(parsed[i].id, ops[i].id) << "op " << i;
        EXPECT_EQ(parsed[i].side, ops[i].side) << "op " << i;
        EXPECT_EQ(parsed[i].price, ops[i].price) << "op " << i;
        EXPECT_EQ(parsed[i].quantity, ops[i].quantity) << "op " << i;
    }
}

TEST(OrderStream, LoadIgnoresCommentsAndBlankLines) {
    std::istringstream in(
        "# engine op log v1\n"
        "\n"
        "L 1 0 10000 10\n"
        "# a comment between ops\n"
        "C 1\n");
    const auto ops = loadOps(in);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].type, OpType::AddLimit);
    EXPECT_EQ(ops[0].id, 1u);
    EXPECT_EQ(ops[1].type, OpType::Cancel);
    EXPECT_EQ(ops[1].id, 1u);
}

// ---------------------------------------------------------------------------
// Replay equivalence: the op log round trip must reproduce the exact book
// state of directly applying the stream -- this is what makes --replay a
// faithful way to re-measure a recorded session.
// ---------------------------------------------------------------------------

TEST(OrderStream, ReplayMatchesDirectApplication) {
    const auto ops = generate(99, 8000);

    std::ostringstream os;
    dumpOps(os, ops);
    std::istringstream is(os.str());
    const auto parsed = loadOps(is);

    ConcurrentBook direct(2, 1024);
    ConcurrentBook replayed(2, 1024);
    for (const auto& op : ops) {
        direct.submitOp(op);
    }
    for (const auto& op : parsed) {
        replayed.submitOp(op);
    }
    direct.stop();
    replayed.stop();

    EXPECT_EQ(direct.book().bidLevelCount(), replayed.book().bidLevelCount());
    EXPECT_EQ(direct.book().askLevelCount(), replayed.book().askLevelCount());
    EXPECT_EQ(direct.book().bestBid(), replayed.book().bestBid());
    EXPECT_EQ(direct.book().bestAsk(), replayed.book().bestAsk());
    EXPECT_EQ(direct.book().isCrossed(), replayed.book().isCrossed());

    for (const auto& op : ops) {
        if (op.type == OpType::AddLimit || op.type == OpType::AddIOC) {
            EXPECT_EQ(direct.book().contains(op.id), replayed.book().contains(op.id))
                << "id " << op.id;
            if (direct.book().contains(op.id)) {
                EXPECT_EQ(direct.book().get(op.id)->quantity,
                          replayed.book().get(op.id)->quantity)
                    << "id " << op.id;
            }
        }
    }
}
