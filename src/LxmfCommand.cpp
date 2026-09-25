// src/LxmfCommand.cpp — see LxmfCommand.h. No Arduino dependency.

#include "LxmfCommand.h"
#include "Msgpack.h"
#include "Battery.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace rlr { namespace lxmf_command {

bool unpack(const uint8_t* data, size_t len, Message& out) {
    const size_t header = HASH_LEN * 2 + SIG_LEN;
    if (data == nullptr || len <= header) return false;

    out.dest_hash   = data;
    out.source_hash = data + HASH_LEN;
    out.content[0]  = '\0';

    msgpack::Reader r(data + header, len - header);
    size_t n;
    if (!r.array_header(n) || n < 4) return false;   // [ts, title, content, fields(, stamp)]
    if (!r.skip()) return false;                     // timestamp

    const uint8_t* p;
    size_t plen;
    if (!r.bytes(p, plen)) return false;             // title (ignored)
    if (!r.bytes(p, plen)) return false;             // content

    // Trim whitespace on both ends, then bound the copy.
    while (plen > 0 && isspace(p[0]))        { p++; plen--; }
    while (plen > 0 && isspace(p[plen - 1])) { plen--; }
    if (plen > MAX_CONTENT) plen = MAX_CONTENT;
    memcpy(out.content, p, plen);
    out.content[plen] = '\0';
    return true;
}

static bool ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

Command match(const char* content) {
    if (content == nullptr) return Command::NONE;
    if (*content == '/') content++;
    if (ieq(content, "status"))  return Command::STATUS;
    if (ieq(content, "battery")) return Command::BATTERY;
    if (ieq(content, "bat"))     return Command::BATTERY;
    if (ieq(content, "help"))    return Command::HELP;
    return Command::NONE;
}

static int battery_line(const Stats& s, char* buf, size_t n) {
    if (s.battery_mv == 0) return snprintf(buf, n, "Battery: no reading");
    // Integer maths only — %f support in embedded printf is unreliable.
    int pct10 = (int)(battery::percent(s.battery_mv) * 10.0f + 0.5f);
    return snprintf(buf, n, "Battery %u.%02u V (%d.%d%%)",
                    (unsigned)(s.battery_mv / 1000),
                    (unsigned)((s.battery_mv % 1000) / 10),
                    pct10 / 10, pct10 % 10);
}

size_t format_reply(Command cmd, const Stats& s, char* buf, size_t n) {
    if (buf == nullptr || n == 0) return 0;
    buf[0] = '\0';
    int w = 0;

    switch (cmd) {
    case Command::STATUS: {
        uint32_t d = s.uptime_s / 86400;
        uint32_t h = (s.uptime_s % 86400) / 3600;
        uint32_t m = (s.uptime_s % 3600) / 60;
        char bat[48];
        battery_line(s, bat, sizeof(bat));
        w = snprintf(buf, n,
            "%s %s\n"
            "Up %lud %luh %lum\n"
            "Radio %s, TX %s, %lu.%03lu MHz SF%u BW%lu\n"
            "Packets in %lu / out %lu, paths %lu\n"
            "%s",
            s.display_name, s.version,
            (unsigned long)d, (unsigned long)h, (unsigned long)m,
            s.radio_up ? "up" : "DOWN", s.tx_enabled ? "on" : "off",
            (unsigned long)(s.freq_hz / 1000000),
            (unsigned long)((s.freq_hz % 1000000) / 1000),
            (unsigned)s.sf, (unsigned long)(s.bw_hz / 1000),
            (unsigned long)s.packets_in, (unsigned long)s.packets_out,
            (unsigned long)s.paths,
            bat);
        break;
    }
    case Command::BATTERY:
        w = battery_line(s, buf, n);
        break;
    case Command::HELP:
        w = snprintf(buf, n, "Commands: /status /battery /help");
        break;
    case Command::NONE:
        return 0;
    }
    if (w < 0) { buf[0] = '\0'; return 0; }
    return ((size_t)w < n) ? (size_t)w : n - 1;
}

}} // namespace rlr::lxmf_command
