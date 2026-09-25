// src/Msgpack.cpp — see Msgpack.h. Emit-only msgpack encoder tuned for
// LXMF payloads and the Sideband Telemeter dict.

#include "Msgpack.h"

namespace rlr { namespace msgpack {

void Writer::be16(uint16_t v) {
    put((uint8_t)(v >> 8));
    put((uint8_t)(v));
}

void Writer::be32(uint32_t v) {
    put((uint8_t)(v >> 24));
    put((uint8_t)(v >> 16));
    put((uint8_t)(v >> 8));
    put((uint8_t)(v));
}

void Writer::be64(uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) put((uint8_t)(v >> s));
}

void Writer::array_header(size_t n) {
    if (n < 16) {
        put((uint8_t)(0x90 | (n & 0x0f)));        // fixarray
    } else if (n < 65536) {
        put(0xdc); be16((uint16_t)n);             // array16
    } else {
        put(0xdd); be32((uint32_t)n);             // array32
    }
}

void Writer::map_header(size_t n) {
    if (n < 16) {
        put((uint8_t)(0x80 | (n & 0x0f)));        // fixmap
    } else if (n < 65536) {
        put(0xde); be16((uint16_t)n);             // map16
    } else {
        put(0xdf); be32((uint32_t)n);             // map32
    }
}

void Writer::nil()           { put(0xc0); }
void Writer::boolean(bool v) { put(v ? 0xc3 : 0xc2); }

void Writer::uint(uint64_t v) {
    if (v < 0x80) {
        put((uint8_t)v);                          // positive fixint
    } else if (v <= 0xff) {
        put(0xcc); put((uint8_t)v);               // uint8
    } else if (v <= 0xffff) {
        put(0xcd); be16((uint16_t)v);             // uint16
    } else if (v <= 0xffffffffULL) {
        put(0xce); be32((uint32_t)v);             // uint32
    } else {
        put(0xcf); be64(v);                       // uint64
    }
}

void Writer::integer(int64_t v) {
    if (v >= 0) { uint((uint64_t)v); return; }
    if (v >= -32) {
        put((uint8_t)(0xe0 | (v & 0x1f)));        // negative fixint
    } else if (v >= -128) {
        put(0xd0); put((uint8_t)(int8_t)v);       // int8
    } else if (v >= -32768) {
        put(0xd1); be16((uint16_t)(int16_t)v);    // int16
    } else if (v >= -2147483648LL) {
        put(0xd2); be32((uint32_t)(int32_t)v);    // int32
    } else {
        put(0xd3); be64((uint64_t)v);             // int64
    }
}

void Writer::float64(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));              // IEEE-754, host order
    put(0xcb);
    be64(bits);                                   // big-endian on the wire
}

void Writer::str(const char* s, size_t len) {
    if (len < 32) {
        put((uint8_t)(0xa0 | (len & 0x1f)));      // fixstr
    } else if (len < 256) {
        put(0xd9); put((uint8_t)len);             // str8
    } else if (len < 65536) {
        put(0xda); be16((uint16_t)len);           // str16
    } else {
        put(0xdb); be32((uint32_t)len);           // str32
    }
    if (len) _buf.insert(_buf.end(), s, s + len);
}

void Writer::bin(const uint8_t* data, size_t len) {
    if (len < 256) {
        put(0xc4); put((uint8_t)len);             // bin8
    } else if (len < 65536) {
        put(0xc5); be16((uint16_t)len);           // bin16
    } else {
        put(0xc6); be32((uint32_t)len);           // bin32
    }
    if (len) _buf.insert(_buf.end(), data, data + len);
}

void Writer::append(const uint8_t* data, size_t len) {
    if (len) _buf.insert(_buf.end(), data, data + len);
}

// ---- Reader -------------------------------------------------------

bool Reader::take(size_t n, const uint8_t*& out) {
    if ((size_t)(_end - _p) < n) return false;
    out = _p;
    _p += n;
    return true;
}

bool Reader::be(size_t n, uint32_t& out) {
    const uint8_t* b;
    if (!take(n, b)) return false;
    out = 0;
    for (size_t i = 0; i < n; i++) out = (out << 8) | b[i];
    return true;
}

bool Reader::array_header(size_t& n) {
    const uint8_t* t;
    if (!take(1, t)) return false;
    uint32_t v;
    if ((*t & 0xf0) == 0x90) { n = *t & 0x0f; return true; }
    if (*t == 0xdc) { if (!be(2, v)) return false; n = v; return true; }
    if (*t == 0xdd) { if (!be(4, v)) return false; n = v; return true; }
    return false;
}

bool Reader::bytes(const uint8_t*& out, size_t& len) {
    const uint8_t* t;
    if (!take(1, t)) return false;
    uint32_t v;
    if ((*t & 0xe0) == 0xa0)            v = *t & 0x1f;              // fixstr
    else if (*t == 0xd9 || *t == 0xc4) { if (!be(1, v)) return false; }
    else if (*t == 0xda || *t == 0xc5) { if (!be(2, v)) return false; }
    else if (*t == 0xdb || *t == 0xc6) { if (!be(4, v)) return false; }
    else return false;
    len = v;
    return take(len, out);
}

bool Reader::number(double& out) {
    const uint8_t* t;
    if (!take(1, t)) return false;
    const uint8_t c = *t;
    uint32_t hi, lo;
    if (c <= 0x7f) { out = c; return true; }                        // positive fixint
    if (c >= 0xe0) { out = (int8_t)c; return true; }                // negative fixint
    switch (c) {
        case 0xcb: {                                                // float64
            if (!be(4, hi) || !be(4, lo)) return false;
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            double d; memcpy(&d, &bits, sizeof(d)); out = d; return true;
        }
        case 0xca: {                                                // float32
            if (!be(4, lo)) return false;
            float f; memcpy(&f, &lo, sizeof(f)); out = f; return true;
        }
        case 0xcc: if (!be(1, lo)) return false; out = lo; return true;
        case 0xcd: if (!be(2, lo)) return false; out = lo; return true;
        case 0xce: if (!be(4, lo)) return false; out = lo; return true;
        case 0xcf: if (!be(4, hi) || !be(4, lo)) return false;
                   out = (double)(((uint64_t)hi << 32) | lo); return true;
        case 0xd0: if (!be(1, lo)) return false; out = (int8_t)lo;  return true;
        case 0xd1: if (!be(2, lo)) return false; out = (int16_t)lo; return true;
        case 0xd2: if (!be(4, lo)) return false; out = (int32_t)lo; return true;
        case 0xd3: if (!be(4, hi) || !be(4, lo)) return false;
                   out = (double)(int64_t)(((uint64_t)hi << 32) | lo); return true;
    }
    return false;
}

bool Reader::skip() { return skip_depth(0); }

bool Reader::skip_depth(int depth) {
    // Nesting cap keeps a hostile payload from recursing off the stack.
    if (depth > 8) return false;
    const uint8_t* t;
    if (!take(1, t)) return false;
    const uint8_t c = *t;
    const uint8_t* dummy;
    uint32_t v;

    if (c <= 0x7f || c >= 0xe0) return true;                       // fixint
    if ((c & 0xe0) == 0xa0) return take(c & 0x1f, dummy);           // fixstr
    if ((c & 0xf0) == 0x90 || (c & 0xf0) == 0x80) {                 // fixarray / fixmap
        size_t n = (size_t)(c & 0x0f) * ((c & 0xf0) == 0x80 ? 2 : 1);
        for (size_t i = 0; i < n; i++) if (!skip_depth(depth + 1)) return false;
        return true;
    }
    switch (c) {
        case 0xc0: case 0xc2: case 0xc3: return true;               // nil / bool
        case 0xcc: case 0xd0: return take(1, dummy);
        case 0xcd: case 0xd1: return take(2, dummy);
        case 0xca: case 0xce: case 0xd2: return take(4, dummy);
        case 0xcb: case 0xcf: case 0xd3: return take(8, dummy);
        case 0xd4: return take(2, dummy);                           // fixext 1
        case 0xd5: return take(3, dummy);
        case 0xd6: return take(5, dummy);
        case 0xd7: return take(9, dummy);
        case 0xd8: return take(17, dummy);
        case 0xc4: case 0xd9: return be(1, v) && take(v, dummy);
        case 0xc5: case 0xda: return be(2, v) && take(v, dummy);
        case 0xc6: case 0xdb: return be(4, v) && take(v, dummy);
        case 0xc7: return be(1, v) && take(v + 1, dummy);           // ext 8
        case 0xc8: return be(2, v) && take(v + 1, dummy);
        case 0xc9: return be(4, v) && take(v + 1, dummy);
        case 0xdc: case 0xdd: case 0xde: case 0xdf: {
            if (!be((c == 0xdc || c == 0xde) ? 2 : 4, v)) return false;
            size_t n = (c >= 0xde) ? (size_t)v * 2 : (size_t)v;
            // Every element is at least one byte, so a count larger
            // than what is left is malformed — reject before looping.
            if (n > remaining()) return false;
            for (size_t i = 0; i < n; i++) if (!skip_depth(depth + 1)) return false;
            return true;
        }
    }
    return false;
}

} } // namespace rlr::msgpack
