// test/native/test_lxmf_command/test_lxmf_command.cpp
//
// Host-side tests for the LXMF remote-command logic: the msgpack
// reader, LXMF unpacking (against messages packed by the reference
// Python LXMF — see lxmf_vectors.h), command matching and reply text.
// Msgpack.cpp and LxmfCommand.cpp have no Arduino dependency, so they
// are compiled in directly rather than copied.

#include <unity.h>
#include <string.h>
#include <vector>

#include "../../../src/Msgpack.cpp"
#include "../../../src/LxmfCommand.cpp"
#include "lxmf_vectors.h"

using namespace rlr;
using namespace rlr::lxmf_command;

void setUp() {}
void tearDown() {}

// ---- unpack against reference LXMF output ------------------------

static void test_unpack_status() {
    Message m;
    TEST_ASSERT_TRUE(unpack(LXMF_STATUS, sizeof(LXMF_STATUS), m));
    TEST_ASSERT_EQUAL_STRING("/status", m.content);
    TEST_ASSERT_EQUAL_MEMORY(LXMF_SOURCE_HASH, m.source_hash, HASH_LEN);
    TEST_ASSERT_EQUAL_MEMORY(LXMF_DEST_HASH, m.dest_hash, HASH_LEN);
}

static void test_unpack_trims_and_skips_title() {
    Message m;
    TEST_ASSERT_TRUE(unpack(LXMF_BATTERY, sizeof(LXMF_BATTERY), m));
    TEST_ASSERT_EQUAL_STRING("/Battery", m.content);
}

static void test_unpack_long_content_truncated() {
    Message m;
    TEST_ASSERT_TRUE(unpack(LXMF_LONG, sizeof(LXMF_LONG), m));
    TEST_ASSERT_EQUAL(MAX_CONTENT, strlen(m.content));
    TEST_ASSERT_EQUAL(Command::NONE, match(m.content));
}

static void test_unpack_rejects_every_truncation() {
    // No prefix of a valid message may parse or read out of bounds.
    for (size_t n = 0; n < sizeof(LXMF_STATUS); n++) {
        std::vector<uint8_t> cut(LXMF_STATUS, LXMF_STATUS + n);
        Message m;
        bool ok = unpack(cut.data(), cut.size(), m);
        // The fields map is the last element; only a cut inside it can
        // still parse (unpack never reads past content).
        if (ok) TEST_ASSERT_EQUAL_STRING("/status", m.content);
    }
}

static void test_unpack_rejects_garbage() {
    Message m;
    uint8_t junk[120];
    memset(junk, 0xff, sizeof(junk));
    TEST_ASSERT_FALSE(unpack(junk, sizeof(junk), m));
    TEST_ASSERT_FALSE(unpack(nullptr, 0, m));
    TEST_ASSERT_FALSE(unpack(junk, 96, m));   // header only
}

// ---- msgpack reader ---------------------------------------------

static void test_reader_skips_nested() {
    msgpack::Writer w;
    w.array_header(3);
    w.map_header(1); w.uint(2); w.array_header(2); w.float64(1.5); w.str("x");
    w.bin((const uint8_t*)"abc", 3);
    w.integer(-70000);
    msgpack::Reader r(w.data(), w.size());
    size_t n;
    TEST_ASSERT_TRUE(r.array_header(n));
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_TRUE(r.skip());
    const uint8_t* p; size_t len;
    TEST_ASSERT_TRUE(r.bytes(p, len));
    TEST_ASSERT_EQUAL(3, len);
    TEST_ASSERT_TRUE(r.skip());
    TEST_ASSERT_EQUAL(0, r.remaining());
}

static void test_reader_rejects_oversized_counts() {
    const uint8_t big_array[] = {0xdd, 0xff, 0xff, 0xff, 0xff, 0x01};
    msgpack::Reader r(big_array, sizeof(big_array));
    TEST_ASSERT_FALSE(r.skip());
    const uint8_t big_bin[] = {0xc6, 0x00, 0x01, 0x00, 0x00, 0x01};
    msgpack::Reader r2(big_bin, sizeof(big_bin));
    const uint8_t* p; size_t len;
    TEST_ASSERT_FALSE(r2.bytes(p, len));
}

static void test_reader_depth_limit() {
    std::vector<uint8_t> deep(64, 0x91);   // 64 nested fixarrays
    deep.push_back(0xc0);
    msgpack::Reader r(deep.data(), deep.size());
    TEST_ASSERT_FALSE(r.skip());
}

// ---- matching ----------------------------------------------------

static void test_match() {
    TEST_ASSERT_EQUAL(Command::STATUS,  match("/status"));
    TEST_ASSERT_EQUAL(Command::STATUS,  match("STATUS"));
    TEST_ASSERT_EQUAL(Command::BATTERY, match("/Battery"));
    TEST_ASSERT_EQUAL(Command::BATTERY, match("/bat"));
    TEST_ASSERT_EQUAL(Command::HELP,    match("/help"));
    TEST_ASSERT_EQUAL(Command::NONE,    match("hello there"));
    TEST_ASSERT_EQUAL(Command::NONE,    match("/statusx"));
    TEST_ASSERT_EQUAL(Command::NONE,    match(""));
    TEST_ASSERT_EQUAL(Command::NONE,    match(nullptr));

    Message m;
    TEST_ASSERT_TRUE(unpack(LXMF_CHAT, sizeof(LXMF_CHAT), m));
    TEST_ASSERT_EQUAL(Command::NONE, match(m.content));
}

// ---- replies -----------------------------------------------------

static Stats sample() {
    Stats s{};
    s.display_name = "Rptr-723F88467151";
    s.version      = "v0.6.5";
    s.uptime_s     = 2 * 86400 + 3 * 3600 + 4 * 60 + 5;
    s.radio_up     = true;
    s.tx_enabled   = true;
    s.freq_hz      = 918300000;
    s.bw_hz        = 250000;
    s.sf           = 10;
    s.packets_in   = 30;
    s.packets_out  = 15;
    s.paths        = 4;
    s.battery_mv   = 3787;
    return s;
}

static void test_battery_reply() {
    char buf[MAX_REPLY + 1];
    Stats s = sample();
    format_reply(Command::BATTERY, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Battery 3.78 V (54.1%)", buf);
    s.battery_mv = 0;
    format_reply(Command::BATTERY, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Battery: no reading", buf);
    s.battery_mv = 4300;
    format_reply(Command::BATTERY, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Battery 4.30 V (100.0%)", buf);
}

static void test_status_reply() {
    char buf[MAX_REPLY + 1];
    size_t n = format_reply(Command::STATUS, sample(), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "Rptr-723F88467151 v0.6.5\n"
        "Up 2d 3h 4m\n"
        "Radio up, TX on, 918.300 MHz SF10 BW250\n"
        "Packets in 30 / out 15, paths 4\n"
        "Battery 3.78 V (54.1%)", buf);
    TEST_ASSERT_EQUAL(strlen(buf), n);
}

static void test_status_reply_fits_worst_case() {
    // Longest name/version and big counters must still fit untruncated.
    Stats s = sample();
    s.display_name = "0123456789012345678901234567890";
    s.version      = "v0.6.5-123-gabcdef0";
    s.uptime_s     = 0xffffffff;
    s.packets_in = s.packets_out = s.paths = 0xffffffff;
    s.freq_hz = 1100000000; s.bw_hz = 500000; s.sf = 12;
    char buf[MAX_REPLY + 1];
    size_t n = format_reply(Command::STATUS, s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n < MAX_REPLY);
    TEST_ASSERT_NOT_NULL(strstr(buf, "Battery"));
}

static void test_reply_truncates_safely() {
    char buf[10];
    size_t n = format_reply(Command::STATUS, sample(), buf, sizeof(buf));
    TEST_ASSERT_EQUAL(9, n);
    TEST_ASSERT_EQUAL(9, strlen(buf));
    TEST_ASSERT_EQUAL(0, format_reply(Command::NONE, sample(), buf, sizeof(buf)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unpack_status);
    RUN_TEST(test_unpack_trims_and_skips_title);
    RUN_TEST(test_unpack_long_content_truncated);
    RUN_TEST(test_unpack_rejects_every_truncation);
    RUN_TEST(test_unpack_rejects_garbage);
    RUN_TEST(test_reader_skips_nested);
    RUN_TEST(test_reader_rejects_oversized_counts);
    RUN_TEST(test_reader_depth_limit);
    RUN_TEST(test_match);
    RUN_TEST(test_battery_reply);
    RUN_TEST(test_status_reply);
    RUN_TEST(test_status_reply_fits_worst_case);
    RUN_TEST(test_reply_truncates_safely);
    return UNITY_END();
}
