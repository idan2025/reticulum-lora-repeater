#pragma once
// =====================================================================
//  src/LxmfCommand.h — pure logic for the LXMF remote-status commands.
//
//  Someone on the go sends "/status" or "/battery" from Sideband /
//  MeshChat to this node's lxmf.delivery address and gets a text reply.
//  This file holds everything that does not touch the radio or
//  Reticulum — LXMF unpacking, command matching, reply formatting — so
//  it can be unit-tested on the host (test/native/test_lxmf_command).
//  The Reticulum glue lives in LxmfInbox.cpp.
// =====================================================================

#include <stdint.h>
#include <stddef.h>

namespace rlr { namespace lxmf_command {

static constexpr size_t HASH_LEN      = 16;   // truncated destination hash
static constexpr size_t SIG_LEN       = 64;   // Ed25519 signature
static constexpr size_t MAX_CONTENT   = 48;   // longer bodies can't be a command
static constexpr size_t MAX_REPLY     = 200;

enum class Command { NONE, STATUS, BATTERY, HELP };

struct Message {
    const uint8_t* dest_hash;              // points into the input buffer
    const uint8_t* source_hash;            // points into the input buffer
    char           content[MAX_CONTENT + 1];
};

// Unpack a full LXMF message: dest(16) || source(16) || signature(64) ||
// msgpack [timestamp, title, content, fields, ...]. Content is copied
// with surrounding whitespace trimmed and truncated to MAX_CONTENT.
// Returns false for anything that is not a well-formed LXMF message.
// The signature is not checked here (see LxmfInbox.cpp).
bool unpack(const uint8_t* data, size_t len, Message& out);

// Map message content to a command. Case-insensitive; the leading "/"
// is optional so "status" also works from clients that eat slashes.
// Anything else is NONE — the node stays silent rather than spend
// airtime answering chatter.
Command match(const char* content);

// Everything a reply can report, gathered by the caller.
struct Stats {
    const char* display_name;
    const char* version;
    uint32_t    uptime_s;
    bool        radio_up;
    bool        tx_enabled;
    uint32_t    freq_hz;
    uint32_t    bw_hz;
    uint8_t     sf;
    uint32_t    packets_in;
    uint32_t    packets_out;
    uint32_t    paths;
    uint16_t    battery_mv;               // 0 = no reading
};

// Render the reply for `cmd` into buf (NUL-terminated, truncated to n).
// Returns the length written. NONE renders nothing.
size_t format_reply(Command cmd, const Stats& s, char* buf, size_t n);

}} // namespace rlr::lxmf_command
