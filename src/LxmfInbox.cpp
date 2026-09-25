// src/LxmfInbox.cpp — see LxmfInbox.h.
//
// Delivery paths, matching the Python LXMF router (LXMRouter.py):
//
//   Inbound
//     OPPORTUNISTIC  single packet to our lxmf.delivery. Plaintext is
//                    source||signature||payload; our hash is implied.
//     DIRECT         the sender opens a Link to our lxmf.delivery and
//                    sends the full message (dest hash included) as a
//                    link packet. This is what Sideband / MeshChat use
//                    when a path is known.
//   Both are proven (PROVE_ALL) so the sender marks the message
//   delivered instead of retrying.
//
//   Reply
//     After a DIRECT delivery is proven, current LXMF identifies itself
//     on the link ("backchannel") and starts accepting messages on it.
//     We wait briefly for that, then reply over the same link — no need
//     to have heard the sender's announce. Otherwise we fall back to an
//     opportunistic reply, which needs the sender's identity (from its
//     announce) and ideally a path, requested on demand.
//
// The message signature is not verified: the replies are read-only
// status, and verifying would require the sender's announce, which the
// link backchannel path deliberately does not depend on.

#include "LxmfInbox.h"
#include "LxmfCommand.h"
#include "Lxmf.h"
#include "Radio.h"
#include "Telemetry.h"
#include "Transport.h"

#include <Arduino.h>
#include <string.h>

#include <microReticulum/Transport.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Link.h>
#include <microReticulum/Packet.h>
#include <microReticulum/Bytes.h>

#ifndef RLR_VERSION
  #define RLR_VERSION "0.1.0-dev"
#endif

namespace rlr { namespace lxmf_inbox {

using namespace rlr::lxmf_command;

static constexpr size_t   MAX_PENDING         = 2;
static constexpr size_t   MAX_LINKS           = 3;
static constexpr size_t   RECENT_IDS          = 8;
static constexpr size_t   ID_LEN              = 8;       // signature prefix as message id
static constexpr size_t   SENDER_SLOTS        = 4;
static constexpr uint32_t SENDER_COOLDOWN_MS  = 15000;   // per sender
static constexpr uint32_t REPLY_SPACING_MS    = 5000;    // between any two replies
static constexpr uint32_t BACKCHANNEL_WAIT_MS = 12000;   // for link identify
static constexpr uint32_t REPLY_GIVE_UP_MS    = 60000;
static constexpr double   LINK_IDLE_S         = 120.0;

struct Pending {
    bool      used = false;
    uint8_t   src[HASH_LEN];
    Command   cmd = Command::NONE;
    uint32_t  t0 = 0;
    bool      path_requested = false;
    RNS::Link link{RNS::Type::NONE};
};

struct Sender {
    uint8_t  src[HASH_LEN];
    uint32_t last_ms = 0;
    bool     used = false;
};

static const Config*     s_cfg = nullptr;
static RNS::Destination* s_dest = nullptr;
static Pending           s_pending[MAX_PENDING];
static Sender            s_senders[SENDER_SLOTS];
static uint8_t           s_recent[RECENT_IDS][ID_LEN];
static size_t            s_recent_next = 0;
static uint32_t          s_last_reply_ms = 0;
static RNS::Link         s_links[MAX_LINKS] = {RNS::Link{RNS::Type::NONE},
                                               RNS::Link{RNS::Type::NONE},
                                               RNS::Link{RNS::Type::NONE}};

static void print_hash(const uint8_t* h) {
    Serial.print(RNS::Bytes(h, HASH_LEN).toHex().c_str());
}

// ---- admission ---------------------------------------------------

static bool seen_before(const uint8_t* id) {
    for (size_t i = 0; i < RECENT_IDS; i++) {
        if (memcmp(s_recent[i], id, ID_LEN) == 0) return true;
    }
    memcpy(s_recent[s_recent_next], id, ID_LEN);
    s_recent_next = (s_recent_next + 1) % RECENT_IDS;
    return false;
}

// True if this sender may get a reply now; records the attempt.
static bool sender_allowed(const uint8_t* src, uint32_t now) {
    Sender* slot = nullptr;
    for (auto& s : s_senders) {
        if (s.used && memcmp(s.src, src, HASH_LEN) == 0) { slot = &s; break; }
    }
    if (slot) {
        if ((uint32_t)(now - slot->last_ms) < SENDER_COOLDOWN_MS) return false;
    } else {
        // Take a free slot, else evict the least recently seen sender.
        slot = &s_senders[0];
        for (auto& s : s_senders) {
            if (!s.used) { slot = &s; break; }
            if ((uint32_t)(now - s.last_ms) > (uint32_t)(now - slot->last_ms)) slot = &s;
        }
        memcpy(slot->src, src, HASH_LEN);
        slot->used = true;
    }
    slot->last_ms = now;
    return true;
}

static void handle_message(const uint8_t* data, size_t len, const RNS::Link& link) {
    Message msg;
    if (!unpack(data, len, msg)) return;

    Command cmd = match(msg.content);
    if (cmd == Command::NONE) return;   // not for us — stay off the air

    Serial.print("LxmfInbox: \"");
    Serial.print(msg.content);
    Serial.print("\" from ");
    print_hash(msg.source_hash);
    Serial.println(link ? " (link)" : " (opportunistic)");

    // Signature bytes identify the message; LXMF retries resend them unchanged.
    if (seen_before(data + HASH_LEN * 2)) {
        Serial.println("LxmfInbox: duplicate, already answered");
        return;
    }
    if (!sender_allowed(msg.source_hash, millis())) {
        Serial.println("LxmfInbox: sender cooldown, not replying");
        return;
    }
    for (auto& p : s_pending) {
        if (p.used) continue;
        p.used = true;
        memcpy(p.src, msg.source_hash, HASH_LEN);
        p.cmd = cmd;
        p.t0 = millis();
        p.path_requested = false;
        p.link = link;
        return;
    }
    Serial.println("LxmfInbox: reply queue full, dropping");
}

// ---- Reticulum callbacks (enqueue only) --------------------------

static void on_opportunistic(const RNS::Bytes& data, const RNS::Packet& packet) {
    try {
        if (packet.destination_type() == RNS::Type::Destination::LINK) return;
        // Rebuild the full message: our hash is implied by the packet.
        std::vector<uint8_t> full;
        full.reserve(HASH_LEN + data.size());
        full.insert(full.end(), s_dest->hash().data(), s_dest->hash().data() + HASH_LEN);
        full.insert(full.end(), data.data(), data.data() + data.size());
        handle_message(full.data(), full.size(), RNS::Link{RNS::Type::NONE});
    }
    catch (const std::exception& e) {
        Serial.print("LxmfInbox: opportunistic handler exception: ");
        Serial.println(e.what());
    }
}

static void on_link_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
    try {
        handle_message(data.data(), data.size(), packet.link());
    }
    catch (const std::exception& e) {
        Serial.print("LxmfInbox: link handler exception: ");
        Serial.println(e.what());
    }
}

static size_t live_links() {
    size_t n = 0;
    for (auto& l : s_links) {
        if (l && l.status() != RNS::Type::Link::CLOSED) n++;
    }
    return n;
}

static void update_link_admission() {
    if (s_dest) s_dest->accepts_links(live_links() < MAX_LINKS);
}

static void on_link_closed(RNS::Link& link) {
    for (auto& l : s_links) {
        if (l && l.link_id() == link.link_id()) l = RNS::Link{RNS::Type::NONE};
    }
    update_link_admission();
}

static void on_link_established(RNS::Link& link) {
    try {
        link.set_packet_callback(on_link_packet);
        link.set_link_closed_callback(on_link_closed);
        link.set_resource_strategy(RNS::Type::Link::ACCEPT_NONE);   // commands are one packet
        bool tracked = false;
        for (auto& l : s_links) {
            if (!l || l.status() == RNS::Type::Link::CLOSED) { l = link; tracked = true; break; }
        }
        if (!tracked) {
            // Admission should have prevented this; never let links pile up.
            link.teardown();
        }
        update_link_admission();
    }
    catch (const std::exception& e) {
        Serial.print("LxmfInbox: link setup exception: ");
        Serial.println(e.what());
    }
}

// ---- replies -----------------------------------------------------

static void render(Command cmd, char* buf, size_t n) {
    Stats s{};
    s.display_name = s_cfg->display_name;
    s.version      = RLR_VERSION;
    s.uptime_s     = millis() / 1000;
    s.radio_up     = rlr::radio::online();
    s.tx_enabled   = rlr::radio::tx_enabled();
    s.freq_hz      = s_cfg->freq_hz;
    s.bw_hz        = s_cfg->bw_hz;
    s.sf           = s_cfg->sf;
    s.packets_in   = rlr::transport::packets_in();
    s.packets_out  = rlr::transport::packets_out();
    s.paths        = rlr::transport::path_count();
    s.battery_mv   = rlr::telemetry::read_battery_mv(*s_cfg);
    format_reply(cmd, s, buf, n);
}

static bool backchannel_ready(Pending& p) {
    if (!p.link || p.link.status() != RNS::Type::Link::ACTIVE) return false;
    const RNS::Identity& remote = p.link.get_remote_identity();
    if (!remote) return false;
    RNS::Bytes h = RNS::Destination::hash(remote, "lxmf", "delivery");
    return h.size() == HASH_LEN && memcmp(h.data(), p.src, HASH_LEN) == 0;
}

// Try to move one pending reply forward. Returns true if it transmitted.
static bool service(Pending& p, uint32_t now) {
    uint32_t age = now - p.t0;
    char text[MAX_REPLY + 1];

    if (backchannel_ready(p)) {
        render(p.cmd, text, sizeof(text));
        rlr::lxmf::send_over_link(p.link, p.src, text);
        p.used = false;
        p.link = RNS::Link{RNS::Type::NONE};
        return true;
    }

    // Give a link sender time to identify before falling back.
    bool linked = p.link && p.link.status() == RNS::Type::Link::ACTIVE;
    if (linked && age < BACKCHANNEL_WAIT_MS) return false;

    RNS::Bytes src(p.src, HASH_LEN);
    bool known = (bool)RNS::Identity::recall(src);
    bool path  = RNS::Transport::has_path(src);

    if (known && (path || age >= REPLY_GIVE_UP_MS / 2)) {
        render(p.cmd, text, sizeof(text));
        rlr::lxmf::send_opportunistic(p.src, text, nullptr, 0);
        p.used = false;
        p.link = RNS::Link{RNS::Type::NONE};
        return true;
    }
    if (!path && !p.path_requested) {
        p.path_requested = true;
        RNS::Transport::request_path(src);
        return true;   // the path request itself used the radio
    }
    if (age >= REPLY_GIVE_UP_MS) {
        Serial.print("LxmfInbox: no way to reach ");
        print_hash(p.src);
        Serial.println(" (no announce heard), dropping reply");
        p.used = false;
        p.link = RNS::Link{RNS::Type::NONE};
    }
    return false;
}

// ---- public API --------------------------------------------------

void attach(RNS::Destination& dest, const Config& cfg) {
    if (!config::lxmf_commands_enabled(cfg)) {
        Serial.println("LxmfInbox: lxmf_commands disabled in config — not answering messages");
        return;
    }
    if (!config::tx_enabled(cfg)) return;   // can't reply; don't accept links either
    try {
        s_cfg  = &cfg;
        s_dest = &dest;
        dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
        dest.set_packet_callback(on_opportunistic);
        dest.set_link_established_callback(on_link_established);
        dest.accepts_links(true);
        Serial.println("LxmfInbox: answering /status /battery /help");
    }
    catch (const std::exception& e) {
        Serial.print("LxmfInbox: attach exception: ");
        Serial.println(e.what());
        s_dest = nullptr;
    }
}

void tick() {
    if (!s_dest || !rlr::radio::online()) return;
    try {
        uint32_t now = millis();

        // Reap idle links so they stop costing keepalive airtime.
        for (auto& l : s_links) {
            if (!l) continue;
            if (l.status() == RNS::Type::Link::CLOSED) { l = RNS::Link{RNS::Type::NONE}; continue; }
            if (l.inactive_for() > LINK_IDLE_S) l.teardown();
        }
        update_link_admission();

        if (s_last_reply_ms != 0 && (uint32_t)(now - s_last_reply_ms) < REPLY_SPACING_MS) return;
        for (auto& p : s_pending) {
            if (!p.used) continue;
            if (service(p, now)) { s_last_reply_ms = now; return; }   // one transmission per pass
        }
    }
    catch (const std::exception& e) {
        Serial.print("LxmfInbox: tick exception: ");
        Serial.println(e.what());
    }
}

}} // namespace rlr::lxmf_inbox
