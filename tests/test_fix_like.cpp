#include <gtest/gtest.h>

#include <string>

#include "engine/fix_like.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// FIX-like protocol tests. The important properties: strict checksum and
// body-length validation (a tampered message must be rejected), correct
// mapping of the FIX subset to engine ops, and execution reports that round
// trip through the same parser.
// ---------------------------------------------------------------------------

TEST(FixLike, MakeAndParseRoundTrip) {
    const std::string msg = makeFixMessage({{"35", "D"}, {"11", "order-1"}, {"55", "TEST"},
                                            {"54", "1"}, {"44", "10000"}, {"38", "10"},
                                            {"59", "1"}});
    EXPECT_EQ(msg.substr(0, 9), "8=FIX.4.2");
    EXPECT_EQ(msg.back(), kFixSeparator);

    const auto parsed = parseFixMessage(msg);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed->find("35"), "D");
    EXPECT_EQ(*parsed->find("11"), "order-1");
    EXPECT_EQ(*parsed->find("44"), "10000");
    // CheckSum is always exactly three digits.
    EXPECT_EQ(parsed->find("10")->size(), 3u);
}

TEST(FixLike, RejectsTamperedChecksum) {
    std::string msg = makeFixMessage({{"35", "D"}, {"11", "order-1"}, {"55", "TEST"},
                                      {"54", "1"}, {"44", "10000"}, {"38", "10"}});
    // Tamper with the price; checksum must now fail.
    msg.replace(msg.find("44=10000"), 8, "44=99999");
    const auto parsed = parseFixMessage(msg);
    EXPECT_FALSE(parsed.has_value());
}

TEST(FixLike, RejectsWrongBodyLength) {
    std::string msg = makeFixMessage({{"35", "D"}, {"11", "order-1"}, {"55", "TEST"},
                                      {"54", "1"}, {"44", "10000"}, {"38", "10"}});
    // Corrupt the declared body length.
    const std::size_t pos = msg.find("9=") + 2;
    msg[pos] = '1';
    const auto parsed = parseFixMessage(msg);
    EXPECT_FALSE(parsed.has_value());
}

TEST(FixLike, RejectsGarbage) {
    EXPECT_FALSE(parseFixMessage("hello world").has_value());
    EXPECT_FALSE(parseFixMessage("").has_value());
    EXPECT_FALSE(parseFixMessage(makeFixMessage({})).has_value()); // no MsgType
}

TEST(FixLike, NewOrderSingleToOp) {
    const auto msg = parseFixMessage(
        makeFixMessage({{"35", "D"}, {"11", "order-7"}, {"55", "TEST"}, {"54", "2"},
                        {"44", "10100"}, {"38", "25"}, {"59", "1"}}));
    ASSERT_TRUE(msg.has_value());

    const auto op = newOrderSingleToOp(*msg);
    ASSERT_TRUE(op.has_value());
    EXPECT_EQ(op->type, OpType::AddLimit);
    EXPECT_EQ(op->id, orderIdFromClOrdId("order-7"));
    EXPECT_EQ(op->side, Side::Sell); // 54=2
    EXPECT_EQ(op->price, 10100);
    EXPECT_EQ(op->quantity, 25);

    // 59=3 means IOC.
    const auto ioc_msg = parseFixMessage(
        makeFixMessage({{"35", "D"}, {"11", "order-8"}, {"55", "TEST"}, {"54", "1"},
                        {"44", "10000"}, {"38", "5"}, {"59", "3"}}));
    ASSERT_TRUE(ioc_msg.has_value());
    const auto ioc_op = newOrderSingleToOp(*ioc_msg);
    ASSERT_TRUE(ioc_op.has_value());
    EXPECT_EQ(ioc_op->type, OpType::AddIOC);
}

TEST(FixLike, CancelAndCancelReplaceToOp) {
    const auto cancel_msg = parseFixMessage(
        makeFixMessage({{"35", "F"}, {"11", "c1"}, {"41", "order-7"}, {"55", "TEST"}}));
    ASSERT_TRUE(cancel_msg.has_value());
    const auto cancel_op = orderCancelRequestToOp(*cancel_msg);
    ASSERT_TRUE(cancel_op.has_value());
    EXPECT_EQ(cancel_op->type, OpType::Cancel);
    EXPECT_EQ(cancel_op->id, orderIdFromClOrdId("order-7"));

    const auto replace_msg = parseFixMessage(
        makeFixMessage({{"35", "G"}, {"11", "c2"}, {"41", "order-7"}, {"55", "TEST"},
                        {"44", "10200"}, {"38", "30"}}));
    ASSERT_TRUE(replace_msg.has_value());
    const auto replace_op = cancelReplaceRequestToOp(*replace_msg);
    ASSERT_TRUE(replace_op.has_value());
    EXPECT_EQ(replace_op->type, OpType::CancelReplace);
    EXPECT_EQ(replace_op->id, orderIdFromClOrdId("order-7"));
    EXPECT_EQ(replace_op->price, 10200);
    EXPECT_EQ(replace_op->quantity, 30);
}

TEST(FixLike, OrderIdMappingIsDeterministicAndScatters) {
    EXPECT_EQ(orderIdFromClOrdId("abc"), orderIdFromClOrdId("abc"));
    EXPECT_NE(orderIdFromClOrdId("abc"), orderIdFromClOrdId("abd"));
    // Sequential-ish ClOrdIds must not collide (mixer applied).
    EXPECT_NE(orderIdFromClOrdId("1"), orderIdFromClOrdId("2"));
}

TEST(FixLike, ExecutionReportRoundTrips) {
    const auto order = parseFixMessage(makeFixMessage(
        {{"35", "D"}, {"11", "order-7"}, {"55", "TEST"}, {"54", "2"}, {"44", "10100"},
        {"38", "25"}}));
    ASSERT_TRUE(order.has_value());

    const Fill fill{.resting_order_id = 1, .incoming_order_id = 2, .price = 10100, .quantity = 10};
    const std::string er = tradeExecutionReport(*order, fill, /*cum_qty=*/10, /*leaves_qty=*/15,
                                                "exec-1");
    const auto parsed = parseFixMessage(er);
    ASSERT_TRUE(parsed.has_value()) << er;
    EXPECT_EQ(*parsed->find("35"), "8");
    EXPECT_EQ(*parsed->find("150"), "F");
    EXPECT_EQ(*parsed->find("39"), "1"); // partially filled
    EXPECT_EQ(*parsed->find("32"), "10");
    EXPECT_EQ(*parsed->find("31"), "10100");
    EXPECT_EQ(*parsed->find("151"), "15");
    EXPECT_EQ(*parsed->find("14"), "10");
    EXPECT_EQ(*parsed->find("11"), "order-7");

    // A full fill flips the status to Filled.
    const std::string er2 =
        tradeExecutionReport(*order, fill, /*cum_qty=*/25, /*leaves_qty=*/0, "exec-2");
    const auto parsed2 = parseFixMessage(er2);
    ASSERT_TRUE(parsed2.has_value());
    EXPECT_EQ(*parsed2->find("39"), "2");
}

TEST(FixLike, AckReportRoundTrips) {
    const auto order = parseFixMessage(makeFixMessage(
        {{"35", "D"}, {"11", "order-9"}, {"55", "TEST"}, {"54", "1"}, {"44", "9900"},
        {"38", "50"}}));
    ASSERT_TRUE(order.has_value());
    const std::string ack = ackExecutionReport(*order, "0", "0", /*leaves_qty=*/50, "exec-0");
    const auto parsed = parseFixMessage(ack);
    ASSERT_TRUE(parsed.has_value()) << ack;
    EXPECT_EQ(*parsed->find("150"), "0");
    EXPECT_EQ(*parsed->find("39"), "0");
    EXPECT_EQ(*parsed->find("151"), "50");
}
