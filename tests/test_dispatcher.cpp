#include "librim/message_dispatcher.hpp"
#include <gtest/gtest.h>

using namespace librim;

enum class TestType : uint32_t
{
    MsgA = 1,
    MsgB = 2
};

struct TestHeader
{
    TestType type;
    uint32_t length;
};

struct MsgA
{
    TestHeader header;
    uint8_t a;
    uint32_t b;
};

struct MsgB
{
    TestHeader header;
    char data[12];
};

TEST(MessageDispatcherTest, BasicDispatch)
{
    bool msga_called = false;
    bool msgb_called = false;

    MessageDispatcher<TestType, TestHeader> dispatcher([](const TestHeader &h) -> std::pair<TestType, std::size_t> {
        return {h.type, h.length + sizeof(TestHeader)};
    });

    dispatcher.add_handler<MsgA>(TestType::MsgA, [&](MsgA &msg) {
        msga_called = true;
        EXPECT_EQ(msg.a, 42);
        EXPECT_EQ(msg.b, 100);
    });

    dispatcher.add_handler<MsgB>(TestType::MsgB, [&](MsgB &msg) {
        msgb_called = true;
        EXPECT_EQ(std::string(msg.data, 5), "Hello");
    });

    // Call MsgA
    MsgA pktA;
    pktA.header.type = TestType::MsgA;
    pktA.header.length = 5;
    pktA.a = 42;
    pktA.b = 100;

    std::vector<std::byte> packetA(sizeof(MsgA));
    std::memcpy(packetA.data(), &pktA, sizeof(MsgA));
    EXPECT_TRUE(dispatcher.dispatch(packetA));
    EXPECT_TRUE(msga_called);

    // Call MsgB
    MsgB pktB;
    pktB.header.type = TestType::MsgB;
    pktB.header.length = 12;
    std::memcpy(pktB.data, "Hello", 5);

    std::vector<std::byte> packetB(sizeof(MsgB));
    std::memcpy(packetB.data(), &pktB, sizeof(MsgB));
    EXPECT_TRUE(dispatcher.dispatch(packetB));
    EXPECT_TRUE(msgb_called);
}

TEST(MessageDispatcherTest, IncompleteHeader)
{
    MessageDispatcher<TestType, TestHeader> dispatcher(
        [](const TestHeader & /*h*/) -> std::pair<TestType, std::size_t> { return {TestType::MsgA, 0}; });

    dispatcher.add_handler<MsgA>(TestType::MsgA, [](MsgA &) {});

    std::vector<std::byte> short_packet(sizeof(TestHeader) - 1);
    EXPECT_FALSE(dispatcher.dispatch(short_packet));
}
