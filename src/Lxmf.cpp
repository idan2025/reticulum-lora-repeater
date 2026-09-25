// src/Lxmf.cpp — opportunistic LXMF message TX (SPEC §5.1/§5.3/§5.5).
//
// Wire recap (opportunistic, single Reticulum DATA packet):
//
//   body       = source_hash(16) || signature(64) || msgpack_payload
//   payload    = msgpack [ timestamp_f64, title_bin, content_bin, fields_map ]
//   signed     = dest_hash(16) || source_hash(16) || payload
//   signature  = Ed25519(signed || SHA256(signed_part))     (see §5.5)
//
//   source_hash = OUR lxmf.delivery destination hash (NOT identity hash — §5.4)
//   dest_hash   = collector's lxmf.delivery destination hash (the configured
//                 16-byte collector_hash; also the outer packet destination)
//
// The body is then Token-encrypted to the collector's identity by the
// normal Reticulum SINGLE-destination send path and transmitted as one
// DATA packet. We hand it to a fresh OUT/SINGLE Destination built from
// the recalled collector identity.

#include "Lxmf.h"
#include "Msgpack.h"

#include <Arduino.h>
#include <vector>

#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Packet.h>
#include <microReticulum/Link.h>
#include <microReticulum/Bytes.h>

namespace rlr { namespace lxmf {

static constexpr size_t HASH_LEN = 16;   // truncated destination hash
static constexpr size_t SIG_LEN  = 64;   // Ed25519 signature

bool pack(const uint8_t* dest_hash,
          const char* content,
          const uint8_t* fields_msgpack, size_t fields_len,
          std::vector<uint8_t>& out) {
    if (dest_hash == nullptr) return false;

    // --- our own lxmf.delivery destination hash = source_hash (§5.4) ---
    const RNS::Identity& self = RNS::Transport::identity();
    RNS::Bytes src_hash = RNS::Destination::hash(self, "lxmf", "delivery");
    if (src_hash.size() != HASH_LEN) {
        Serial.println("Lxmf: unexpected source_hash length");
        return false;
    }

    // --- msgpack payload: [ts_f64, title_bin, content_bin, fields_map] ---
    const char* body_text = content ? content : "";
    size_t      body_len  = strlen(body_text);

    msgpack::Writer payload;
    payload.reserve(32 + body_len + fields_len);
    payload.array_header(4);
    // No wall clock on this hardware (no RTC/GPS time) — emit a
    // monotonic uptime-seconds value. Sideband renders the message
    // against its own receive clock, so this is metadata only.
    payload.float64((double)millis() / 1000.0);
    payload.bin(nullptr, 0);                       // empty title (bin8 len 0)
    payload.bin((const uint8_t*)body_text, body_len);
    if (fields_msgpack && fields_len > 0) {
        payload.append(fields_msgpack, fields_len);  // pre-encoded map
    } else {
        payload.map_header(0);                       // empty fields {}
    }

    if (payload.size() > MAX_OPPORTUNISTIC_PAYLOAD) {
        Serial.print("Lxmf: payload too large for single-packet delivery (");
        Serial.print(payload.size());
        Serial.print(" > ");
        Serial.print(MAX_OPPORTUNISTIC_PAYLOAD);
        Serial.println(")");
        return false;
    }

    // --- signed_data = dest||src||payload || SHA256(dest||src||payload) ---
    std::vector<uint8_t> hashed;
    hashed.reserve(HASH_LEN * 2 + payload.size());
    hashed.insert(hashed.end(), dest_hash, dest_hash + HASH_LEN);
    hashed.insert(hashed.end(), src_hash.data(), src_hash.data() + HASH_LEN);
    hashed.insert(hashed.end(), payload.data(), payload.data() + payload.size());

    RNS::Bytes msg_hash = RNS::Identity::full_hash(RNS::Bytes(hashed.data(), hashed.size()));

    std::vector<uint8_t> signed_data = hashed;
    signed_data.insert(signed_data.end(), msg_hash.data(), msg_hash.data() + msg_hash.size());

    RNS::Bytes signature = self.sign(RNS::Bytes(signed_data.data(), signed_data.size()));
    if (signature.size() != SIG_LEN) {
        Serial.print("Lxmf: unexpected signature length ");
        Serial.println(signature.size());
        return false;
    }

    // --- full message = dest || src || signature || payload ---
    out.clear();
    out.reserve(HASH_LEN * 2 + SIG_LEN + payload.size());
    out.insert(out.end(), dest_hash, dest_hash + HASH_LEN);
    out.insert(out.end(), src_hash.data(), src_hash.data() + HASH_LEN);
    out.insert(out.end(), signature.data(), signature.data() + SIG_LEN);
    out.insert(out.end(), payload.data(), payload.data() + payload.size());
    return true;
}

bool send_opportunistic(const uint8_t* collector_hash,
                        const char* content,
                        const uint8_t* fields_msgpack, size_t fields_len) {
    if (collector_hash == nullptr) return false;

    try {
        // --- recipient identity (must have been heard via announce) ---
        RNS::Bytes dest_hash(collector_hash, HASH_LEN);
        RNS::Identity collector_id = RNS::Identity::recall(dest_hash);
        if (!collector_id) {
            // No public key for this destination yet — Reticulum hasn't
            // heard the recipient's announce, so we can't encrypt to it.
            Serial.println("Lxmf: recipient identity unknown (no announce heard yet) — skipping");
            return false;
        }

        std::vector<uint8_t> msg;
        if (!pack(collector_hash, content, fields_msgpack, fields_len, msg)) return false;

        // Opportunistic body omits the destination hash — it is the
        // outer packet's destination (§5.1).
        RNS::Destination dest(collector_id,
                              RNS::Type::Destination::OUT,
                              RNS::Type::Destination::SINGLE,
                              "lxmf", "delivery");
        RNS::Packet packet(dest, RNS::Bytes(msg.data() + HASH_LEN, msg.size() - HASH_LEN));
        packet.send();

        Serial.print("Lxmf: sent opportunistic message to ");
        Serial.print(dest_hash.toHex().c_str());
        Serial.print(" (");
        Serial.print((unsigned)(msg.size() - HASH_LEN));
        Serial.println(" body bytes)");
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("Lxmf::send_opportunistic exception: ");
        Serial.println(e.what());
        return false;
    }
}

bool send_over_link(const RNS::Link& link, const uint8_t* dest_hash, const char* content) {
    try {
        std::vector<uint8_t> msg;
        if (!pack(dest_hash, content, nullptr, 0, msg)) return false;
        // Over a link the whole message, destination hash included, is
        // the packet data — the receiver's delivery_packet() takes it as-is.
        RNS::Packet packet(link, RNS::Bytes(msg.data(), msg.size()));
        packet.send();
        Serial.print("Lxmf: sent message over link (");
        Serial.print((unsigned)msg.size());
        Serial.println(" bytes)");
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("Lxmf::send_over_link exception: ");
        Serial.println(e.what());
        return false;
    }
}

} } // namespace rlr::lxmf
