#pragma once
// =====================================================================
//  src/LxmfInbox.h — answers LXMF "/status" and "/battery" messages so
//  the node can be checked from Sideband / MeshChat on the go.
//
//  Repeating comes first. Everything here is bounded so it cannot eat
//  into forwarding:
//    - Reticulum callbacks only enqueue; replies go out from tick(),
//      at most one per call, and never closer than REPLY_SPACING_MS.
//    - Two pending replies, a per-sender cooldown, duplicate
//      suppression for LXMF retries, and at most MAX_LINKS inbound
//      links, each torn down once idle.
//    - Unrecognised text gets no reply at all.
//    - `CONFIG SET lxmf_commands 0` (then COMMIT) disables all of it:
//      no callbacks are registered and no links are accepted.
// =====================================================================

#include "Config.h"

namespace RNS { class Destination; }

namespace rlr { namespace lxmf_inbox {

// Hook the node's lxmf.delivery destination. Call once, after it has
// been created (lxmf_presence::init). No-op when commands are disabled
// or TX is inhibited.
void attach(RNS::Destination& dest, const Config& cfg);

// Called from loop(). Sends at most one queued reply and reaps idle links.
void tick();

}} // namespace rlr::lxmf_inbox
