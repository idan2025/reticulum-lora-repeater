// src/LxmfPresence.cpp — periodic LXMF presence announces so
// MeshChat / Sideband / NomadNet show this node by name in their
// network visualizers. Ported from the sibling project's
// announce_lxmf_presence(); display name now comes from the runtime
// Config instead of BAKED_LXMF_DISPLAY_NAME.
//
// Wire format is confirmed against the reference implementation in
// micropython-reticulum/firmware/urns/lxmf.py:
//
//   APP_NAME = "lxmf"
//   Destination(identity, IN, SINGLE, APP_NAME, "delivery")
//   app_data = umsgpack.packb([display_name.encode("utf-8"), stamp_cost])
//
// msgpack layout for a name up to 255 bytes:
//
//   0x92                 fixarray, 2 elements
//   0xc4 <len> <bytes>   bin8, display_name as UTF-8
//   0xc0                 nil (stamp_cost)

#include "LxmfPresence.h"
#include "Radio.h"

#include <Arduino.h>
#include <string.h>

#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Bytes.h>

namespace rlr { namespace lxmf_presence {

static RNS::Destination s_dest(RNS::Type::NONE);
static bool             s_ready    = false;
static uint32_t         s_last_ms  = 0;
static constexpr uint32_t FIRST_MS = 15000UL;   // 15 s after init

bool init(const Config& cfg) {
    (void)cfg;
    try {
        s_dest = RNS::Destination(
            RNS::Transport::identity(),
            RNS::Type::Destination::IN,
            RNS::Type::Destination::SINGLE,
            "lxmf", "delivery");
        s_ready = true;
        Serial.print("LxmfPresence: destination hash ");
        Serial.println(s_dest.hash().toHex().c_str());
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("LxmfPresence::init exception: ");
        Serial.println(e.what());
        s_ready = false;
        return false;
    }
}

void announce_now(const Config& cfg) {
    if (!s_ready) return;

    const char* name = cfg.display_name;
    size_t name_len  = strlen(name);
    // Cap to 200 so the whole msgpack blob fits in <=205 bytes and
    // leaves headroom inside the announce's app_data framing.
    if (name_len > 200) name_len = 200;

    uint8_t buf[224];
    size_t i = 0;
    buf[i++] = 0x92;                   // fixarray, 2 elements
    buf[i++] = 0xc4;                   // bin8
    buf[i++] = (uint8_t)name_len;
    memcpy(buf + i, name, name_len);
    i += name_len;
    buf[i++] = 0xc0;                   // nil (stamp_cost)

    Serial.print("LxmfPresence: announcing as \"");
    Serial.print(name);
    Serial.println("\"");
    try {
        s_dest.announce(RNS::Bytes(buf, i));
    }
    catch (const std::exception& e) {
        Serial.print("LxmfPresence::announce exception: ");
        Serial.println(e.what());
    }
}

void tick(const Config& cfg) {
    if (!s_ready) return;
    if ((cfg.flags & CONFIG_FLAG_LXMF) == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Serial.println("LxmfPresence: lxmf is disabled in config — presence announces are OFF");
        }
        return;
    }
    if (!rlr::radio::online()) return;

    // An interval of 0 means "off". Without this the subtraction below
    // is always >= 0, so the task would fire on every single loop
    // iteration and flood the mesh. Existing configs saved before the
    // input floor existed can still carry a 0 here, so the guard lives
    // at the point of use rather than only in set_field().
    if (cfg.lxmf_interval_ms == 0) {
        // Say it once. Returning in silence here looks identical to a
        // node that is announcing fine, which is the whole problem the
        // old always-fire behaviour hid.
        static bool warned = false;
        if (!warned) {
            warned = true;
            Serial.println("LxmfPresence: lxmf_interval_ms is 0 — presence announces are OFF");
        }
        return;
    }

    uint32_t now = millis();
    bool due;
    if (s_last_ms == 0) {
        due = (now >= FIRST_MS);
    } else {
        due = ((now - s_last_ms) >= cfg.lxmf_interval_ms);
    }
    if (!due) return;

    s_last_ms = now;
    announce_now(cfg);
}

}} // namespace rlr::lxmf_presence
