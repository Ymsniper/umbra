#pragma once
// A small x86-64 interpreter, for the arithmetic this game hides the address of
// its object array behind.
//
// That arithmetic has changed shape with every patch: inlined word shuffles and
// a rotate, then a called routine mixing with carry-less multiplication, then an
// inline xor, rotate and add against constants in .rdata. A matcher written for
// one of them breaks on the next, and each break costs a session of reading
// disassembly before anything works again.
//
// What does not change is that the game computes the address itself, in a short
// run of straight-line instructions, out of values that are all readable: a
// module global and constants beside it. So this runs those instructions rather
// than recognising them. The deriver emulates them to find the array and prove
// it, and hands the same bytes to the ship repo to run at startup.
//
// Only the operations these sequences are built from are implemented. Anything
// else stops the run rather than being guessed at, and says which opcode it
// was, so the gap is a line to add and not a session to spend. A register whose
// value was never established stays unknown rather than quietly reading as
// zero, so an answer can never rest on one.
#include "mem.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct EmuReg {
    uint64_t lo = 0, hi = 0;          // hi is the upper half of an xmm register
    bool known = false;
};

struct EmuState {
    EmuReg gpr[16];
    EmuReg xmm[16];
    unsigned char lastOpcode = 0;     // what stopped the run, when it stopped
    bool unsupported = false;
};

inline uint32_t emuRor32(uint32_t v, int n) {
    n &= 31;
    return n ? (uint32_t)((v >> n) | (v << (32 - n))) : v;
}
inline uint64_t emuRor64(uint64_t v, int n) {
    n &= 63;
    return n ? ((v >> n) | (v << (64 - n))) : v;
}
inline uint64_t emuClmulLo(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    while (b) { if (b & 1) r ^= a; a <<= 1; b >>= 1; }
    return r;
}

// The lanes an SSE operation works on, so each one is written once.
struct EmuLanes { uint32_t d[4]; uint16_t w[8]; };

inline void emuSplit(const EmuReg& r, EmuLanes& l) {
    for (int i = 0; i < 4; i++)
        l.d[i] = (uint32_t)((i < 2 ? r.lo : r.hi) >> (32 * (i & 1)));
    for (int i = 0; i < 8; i++)
        l.w[i] = (uint16_t)((i < 4 ? r.lo : r.hi) >> (16 * (i & 3)));
}
inline void emuJoinD(const EmuLanes& l, EmuReg& r) {
    r.lo = (uint64_t)l.d[0] | ((uint64_t)l.d[1] << 32);
    r.hi = (uint64_t)l.d[2] | ((uint64_t)l.d[3] << 32);
}
inline void emuJoinW(const EmuLanes& l, EmuReg& r) {
    r.lo = r.hi = 0;
    for (int i = 0; i < 4; i++) r.lo |= (uint64_t)l.w[i] << (16 * i);
    for (int i = 0; i < 4; i++) r.hi |= (uint64_t)l.w[4 + i] << (16 * i);
}

class Emu {
public:
    explicit Emu(const Mem& mem) : m_(mem) {}

    // Runs [code, code+n), which sits at `va`, and returns how far it got. A
    // call into a short leaf function is followed, because an obfuscation that
    // lives in one is the same arithmetic wearing a call.
    // A jump may go backwards, so the run is bounded by a count of instructions
    // as well as by the end of the bytes. A sequence that loops is one this was
    // never given the constants to finish, and hanging startup is a worse
    // answer than falling back to the sweep.
    static constexpr int kMaxSteps = 8192;

    size_t run(const unsigned char* code, size_t n, uintptr_t va, EmuState& st, int depth = 0) {
        size_t off = 0;
        for (int steps = 0; off < n; steps++) {
            if (steps >= kMaxSteps) { st.unsupported = true; break; }
            int64_t jump = 0;
            const size_t len = step(code + off, n - off, va + off, st, depth, jump);
            if (!len) break;
            if (jump) {
                const int64_t to = (int64_t)off + (int64_t)len + jump;
                if (to < 0 || (size_t)to >= n) break;
                off = (size_t)to;
                continue;
            }
            off += len;
        }
        return off;
    }

    // One instruction. Returns its length, or 0 to stop. `jump` is a relative
    // displacement to take afterwards.
    size_t step(const unsigned char* p, size_t n, uintptr_t va, EmuState& st, int depth,
                int64_t& jump);

private:
    const Mem& m_;

    bool rd(uintptr_t a, void* b, size_t sz) const { return a && m_.read_raw(a, b, sz); }
};

// One instruction, decoded and performed.
//
// The decoding is only as complete as it has to be: prefixes, ModRM with SIB
// and rip-relative addressing, and the operations these sequences use. Flags
// are not modelled, so a conditional jump falls through, which is what the
// bounds check around an object lookup does when the index is in range.
inline size_t Emu::step(const unsigned char* p, size_t n, uintptr_t va, EmuState& st,
                        int depth, int64_t& jump) {
    jump = 0;
    size_t i = 0;
    bool op66 = false, opF2 = false, opF3 = false;
    int rex = 0;
    while (i < n) {                                  // prefixes
        const unsigned char b = p[i];
        if (b == 0x66) { op66 = true; i++; continue; }
        if (b == 0xF2) { opF2 = true; i++; continue; }
        if (b == 0xF3) { opF3 = true; i++; continue; }
        if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) { i++; continue; }
        if (b >= 0x40 && b <= 0x4F) { rex = b; i++; continue; }
        break;
    }
    if (i >= n) return 0;
    const bool w = (rex & 8) != 0;
    const int rexR = (rex & 4) ? 8 : 0, rexX = (rex & 2) ? 8 : 0, rexB = (rex & 1) ? 8 : 0;

    // ModRM, when the opcode has one. Returns the length consumed after it.
    int reg = 0, rmReg = 0;
    bool rmIsReg = false, addrKnown = false;
    uintptr_t addr = 0;
    size_t mrmLen = 0;
    auto modrm = [&](size_t at) -> bool {
        if (at >= n) return false;
        const unsigned char m = p[at];
        const int mod = m >> 6, rm = m & 7;
        reg = ((m >> 3) & 7) | rexR;
        size_t len = 1;
        if (mod == 3) { rmIsReg = true; rmReg = rm | rexB; mrmLen = len; return true; }
        rmIsReg = false;
        uintptr_t base = 0;
        bool known = true;
        if (rm == 4) {                                // SIB
            if (at + 1 >= n) return false;
            const unsigned char sib = p[at + 1];
            len++;
            const int idx = ((sib >> 3) & 7) | rexX, bs = (sib & 7) | rexB;
            if (idx != 4) {
                known = known && st.gpr[idx].known;
                base += st.gpr[idx].lo << (sib >> 6);
            }
            if ((sib & 7) == 5 && mod == 0) {
                if (at + len + 4 > n) return false;
                int32_t d; memcpy(&d, p + at + len, 4); len += 4;
                base += (uint64_t)(int64_t)d;
            } else {
                known = known && st.gpr[bs].known;
                base += st.gpr[bs].lo;
            }
        } else if (rm == 5 && mod == 0) {             // rip-relative
            if (at + len + 4 > n) return false;
            int32_t d; memcpy(&d, p + at + len, 4); len += 4;
            base = 0;                                  // filled in below, needs the length
            addrKnown = true;
            mrmLen = len;
            addr = (uintptr_t)((int64_t)d);            // displacement only for now
            rmReg = -1;                                // marks rip-relative
            return true;
        } else {
            known = known && st.gpr[rm | rexB].known;
            base += st.gpr[rm | rexB].lo;
        }
        if (mod == 1) {
            if (at + len >= n) return false;
            base += (uint64_t)(int64_t)(int8_t)p[at + len];
            len += 1;
        } else if (mod == 2) {
            if (at + len + 4 > n) return false;
            int32_t d; memcpy(&d, p + at + len, 4); len += 4;
            base += (uint64_t)(int64_t)d;
        }
        addr = base;
        addrKnown = known;
        mrmLen = len;
        return true;
    };
    // rip-relative addresses are only final once the instruction's length is,
    // so this is called at the end of decoding.
    auto ripFix = [&](size_t total) {
        if (!rmIsReg && rmReg == -1) { addr = va + total + (int64_t)addr; rmReg = 0; }
    };

    auto loadRm = [&](size_t total, size_t sz, EmuReg& out) -> bool {
        if (rmIsReg) { out = (sz == 16) ? st.xmm[rmReg] : st.gpr[rmReg]; return true; }
        ripFix(total);
        out = EmuReg{};
        if (!addrKnown) return true;                   // unknown, but not an error
        unsigned char buf[16] = {0};
        if (!rd(addr, buf, sz == 16 ? 16 : sz)) return true;
        memcpy(&out.lo, buf, 8 > sz ? sz : 8);
        if (sz == 16) memcpy(&out.hi, buf + 8, 8);
        out.known = true;
        return true;
    };

    const unsigned char op = p[i];

    // ── two-byte opcodes ──────────────────────────────────────────────────
    if (op == 0x0F) {
        if (i + 1 >= n) return 0;
        const unsigned char o2 = p[i + 1];
        size_t at = i + 2;

        if (o2 >= 0xC8 && o2 <= 0xCF) {                // bswap
            const int r = (o2 - 0xC8) | rexB;
            if (st.gpr[r].known)
                st.gpr[r].lo = w ? __builtin_bswap64(st.gpr[r].lo)
                                 : __builtin_bswap32((uint32_t)st.gpr[r].lo);
            return at - 0;
        }
        if (o2 == 0x1F) { if (!modrm(at)) return 0; return at + mrmLen; }   // nop
        if (o2 >= 0x80 && o2 <= 0x8F) { return at + 4; }                    // jcc: not taken
        if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF) {         // movzx / movsx
            if (!modrm(at)) return 0;
            const size_t total = at + mrmLen;
            EmuReg src;
            loadRm(total, (o2 & 1) ? 2 : 1, src);
            uint64_t v = src.lo & ((o2 & 1) ? 0xFFFF : 0xFF);
            if (o2 >= 0xBE) {                           // sign extend
                v = (o2 & 1) ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(int64_t)(int8_t)v;
            }
            st.gpr[reg] = EmuReg{v, 0, src.known};
            return total;
        }
        if (o2 == 0xAF) {                              // imul r, rm
            if (!modrm(at)) return 0;
            const size_t total = at + mrmLen;
            EmuReg src;
            loadRm(total, w ? 8 : 4, src);
            const bool k = src.known && st.gpr[reg].known;
            uint64_t v = st.gpr[reg].lo * src.lo;
            if (!w) v = (uint32_t)v;
            st.gpr[reg] = EmuReg{v, 0, k};
            return total;
        }
        if (o2 == 0x38 || o2 == 0x3A) {                // three-byte
            if (i + 2 >= n) return 0;
            const unsigned char o3 = p[i + 2];
            at = i + 3;
            if (!modrm(at)) return 0;
            const size_t total = at + mrmLen + (o2 == 0x3A ? 1 : 0);
            EmuReg src;
            loadRm(total, 16, src);
            EmuReg& d = st.xmm[reg];
            if (o2 == 0x38 && o3 == 0x00) {            // pshufb
                EmuLanes a, b;
                emuSplit(d, a); emuSplit(src, b);
                unsigned char av[16], bv[16], ov[16];
                memcpy(av, &d.lo, 8); memcpy(av + 8, &d.hi, 8);
                memcpy(bv, &src.lo, 8); memcpy(bv + 8, &src.hi, 8);
                for (int k = 0; k < 16; k++) ov[k] = (bv[k] & 0x80) ? 0 : av[bv[k] & 15];
                memcpy(&d.lo, ov, 8); memcpy(&d.hi, ov + 8, 8);
                d.known = d.known && src.known;
                return total;
            }
            if (o2 == 0x3A && o3 == 0x44) {            // pclmulqdq
                const unsigned char imm = p[total - 1];
                const uint64_t a = (imm & 1) ? d.hi : d.lo;
                const uint64_t b = (imm & 0x10) ? src.hi : src.lo;
                const bool k = d.known && src.known;
                d.lo = emuClmulLo(a, b);
                d.hi = 0;                               // the high half is unused here
                d.known = k;
                return total;
            }
            st.unsupported = true; st.lastOpcode = o3;
            return 0;
        }
        // SSE with a ModRM operand
        if (!modrm(at)) return 0;
        size_t total = at + mrmLen;
        const bool hasImm = (o2 == 0x70 || (o2 >= 0x71 && o2 <= 0x73));
        if (hasImm) total += 1;
        EmuReg src;
        const size_t width = (o2 == 0x7E && opF3) ? 8 : (o2 == 0x6E ? (w ? 8 : 4) : 16);
        loadRm(total, width, src);

        switch (o2) {
            case 0x10: case 0x28: case 0x6F: st.xmm[reg] = src; return total;   // loads
            case 0x11: case 0x29: case 0x7F: return total;                      // stores
            case 0x6E: st.xmm[reg] = EmuReg{src.lo, 0, src.known}; return total;
            case 0xD6: return total;                                            // movq store
            case 0x7E:
                if (opF3) { st.xmm[reg] = EmuReg{src.lo, 0, src.known}; return total; }
                if (rmIsReg) st.gpr[rmReg] = EmuReg{st.xmm[reg].lo, 0, st.xmm[reg].known};
                return total;
            case 0xEF: case 0xEB: case 0xDB: case 0xDF: {                       // pxor/por/pand
                EmuReg& d = st.xmm[reg];
                const bool k = d.known && src.known;
                if (o2 == 0xEF) { d.lo ^= src.lo; d.hi ^= src.hi; }
                else if (o2 == 0xEB) { d.lo |= src.lo; d.hi |= src.hi; }
                else if (o2 == 0xDB) { d.lo &= src.lo; d.hi &= src.hi; }
                else { d.lo = ~d.lo & src.lo; d.hi = ~d.hi & src.hi; }
                d.known = k;
                return total;
            }
            case 0xFE: case 0xFA: case 0xD4: case 0xFB: {                       // padd/psub
                EmuReg& d = st.xmm[reg];
                const bool k = d.known && src.known;
                if (o2 == 0xD4 || o2 == 0xFB) {
                    d.lo = (o2 == 0xD4) ? d.lo + src.lo : d.lo - src.lo;
                    d.hi = (o2 == 0xD4) ? d.hi + src.hi : d.hi - src.hi;
                } else {
                    EmuLanes a, b;
                    emuSplit(d, a); emuSplit(src, b);
                    for (int k2 = 0; k2 < 4; k2++)
                        a.d[k2] = (o2 == 0xFE) ? a.d[k2] + b.d[k2] : a.d[k2] - b.d[k2];
                    emuJoinD(a, d);
                }
                d.known = k;
                return total;
            }
            case 0x6C: {                                                        // punpcklqdq
                EmuReg& d = st.xmm[reg];
                d.hi = src.lo;
                d.known = d.known && src.known;
                return total;
            }
            case 0x6D: {                                                        // punpckhqdq
                EmuReg& d = st.xmm[reg];
                d.lo = d.hi; d.hi = src.hi;
                d.known = d.known && src.known;
                return total;
            }
            case 0x70: {                                                        // pshuf*
                const unsigned char imm = p[total - 1];
                EmuReg& d = st.xmm[reg];
                EmuLanes a; emuSplit(src, a);
                EmuLanes o{};
                if (op66) {                                                     // pshufd
                    for (int k2 = 0; k2 < 4; k2++) o.d[k2] = a.d[(imm >> (2 * k2)) & 3];
                    emuJoinD(o, d);
                } else {
                    for (int k2 = 0; k2 < 8; k2++) o.w[k2] = a.w[k2];
                    if (opF2) for (int k2 = 0; k2 < 4; k2++) o.w[k2] = a.w[(imm >> (2 * k2)) & 3];
                    if (opF3) for (int k2 = 0; k2 < 4; k2++) o.w[4 + k2] = a.w[4 + ((imm >> (2 * k2)) & 3)];
                    emuJoinW(o, d);
                }
                d.known = src.known;
                return total;
            }
            case 0x71: case 0x72: case 0x73: {                                  // shifts by imm
                const unsigned char imm = p[total - 1];
                const int sub = (p[at] >> 3) & 7;
                EmuReg& d = st.xmm[rmIsReg ? rmReg : reg];
                EmuLanes a; emuSplit(d, a);
                if (o2 == 0x72) {                                               // dwords
                    for (int k2 = 0; k2 < 4; k2++)
                        a.d[k2] = (sub == 2) ? (imm > 31 ? 0 : a.d[k2] >> imm)
                                : (sub == 6) ? (imm > 31 ? 0 : a.d[k2] << imm)
                                : (uint32_t)((int32_t)a.d[k2] >> (imm > 31 ? 31 : imm));
                    emuJoinD(a, d);
                } else if (o2 == 0x71) {                                        // words
                    for (int k2 = 0; k2 < 8; k2++)
                        a.w[k2] = (sub == 2) ? (imm > 15 ? 0 : a.w[k2] >> imm)
                                            : (imm > 15 ? 0 : a.w[k2] << imm);
                    emuJoinW(a, d);
                } else {                                                        // quad / byte-wise
                    if (sub == 2) { d.lo = imm > 63 ? 0 : d.lo >> imm; d.hi = imm > 63 ? 0 : d.hi >> imm; }
                    else if (sub == 6) { d.lo = imm > 63 ? 0 : d.lo << imm; d.hi = imm > 63 ? 0 : d.hi << imm; }
                    else if (sub == 3) {                                        // psrldq
                        unsigned char v[16], o2b[16] = {0};
                        memcpy(v, &d.lo, 8); memcpy(v + 8, &d.hi, 8);
                        for (int k2 = 0; k2 + imm < 16; k2++) o2b[k2] = v[k2 + imm];
                        memcpy(&d.lo, o2b, 8); memcpy(&d.hi, o2b + 8, 8);
                    } else if (sub == 7) {                                      // pslldq
                        unsigned char v[16], o2b[16] = {0};
                        memcpy(v, &d.lo, 8); memcpy(v + 8, &d.hi, 8);
                        for (int k2 = imm; k2 < 16; k2++) o2b[k2] = v[k2 - imm];
                        memcpy(&d.lo, o2b, 8); memcpy(&d.hi, o2b + 8, 8);
                    }
                }
                return total;
            }
            default:
                st.unsupported = true; st.lastOpcode = o2;
                return 0;
        }
    }

    // ── one-byte opcodes ──────────────────────────────────────────────────
    if (op >= 0x50 && op <= 0x5F) return i + 1;                  // push / pop
    if (op == 0x90) return i + 1;                                // nop
    if (op == 0xC3 || op == 0xC2) return 0;                      // return: the slice ends
    if (op >= 0x70 && op <= 0x7F) return i + 2;                  // jcc: not taken
    if (op == 0xEB) {                                            // jmp short
        if (i + 1 >= n) return 0;
        jump = (int8_t)p[i + 1];
        return i + 2;
    }
    if (op == 0xE9) {                                            // jmp near
        if (i + 5 > n) return 0;
        int32_t d; memcpy(&d, p + i + 1, 4);
        jump = d;
        return i + 5;
    }
    if (op == 0xE8) {                                            // call
        if (i + 5 > n || depth >= 2) return 0;
        int32_t d; memcpy(&d, p + i + 1, 4);
        const uintptr_t target = va + (i + 5) + (int64_t)d;
        // A leaf that does arithmetic and returns is the same obfuscation
        // wearing a call, so it is followed. Anything longer is not.
        unsigned char body[0x80];
        if (!rd(target, body, sizeof body)) return 0;
        size_t len = 0;
        for (; len < sizeof body; len++) if (body[len] == 0xC3) { len++; break; }
        if (len == 0 || len >= sizeof body) return 0;
        EmuState sub = st;
        Emu(m_).run(body, len, target, sub, depth + 1);
        if (sub.unsupported) { st.unsupported = true; st.lastOpcode = sub.lastOpcode; return 0; }
        st = sub;
        return i + 5;
    }
    if (op >= 0xB8 && op <= 0xBF) {                              // mov reg, imm
        const int r = (op - 0xB8) | rexB;
        if (w) {
            if (i + 9 > n) return 0;
            uint64_t v; memcpy(&v, p + i + 1, 8);
            st.gpr[r] = EmuReg{v, 0, true};
            return i + 9;
        }
        if (i + 5 > n) return 0;
        uint32_t v; memcpy(&v, p + i + 1, 4);
        st.gpr[r] = EmuReg{v, 0, true};
        return i + 5;
    }

    const bool isAlu = (op == 0x01 || op == 0x03 || op == 0x09 || op == 0x0B ||
                        op == 0x21 || op == 0x23 || op == 0x29 || op == 0x2B ||
                        op == 0x31 || op == 0x33 || op == 0x89 || op == 0x8B ||
                        op == 0x8D || op == 0x63 || op == 0x39 || op == 0x3B ||
                        op == 0x85 || op == 0x84 || op == 0x88 || op == 0x8A);
    if (isAlu) {
        if (!modrm(i + 1)) return 0;
        const size_t total = i + 1 + mrmLen;
        if (op == 0x39 || op == 0x3B || op == 0x85 || op == 0x84) return total;   // cmp / test
        if (op == 0x8D) {                                                        // lea
            ripFix(total);
            st.gpr[reg] = EmuReg{addr, 0, addrKnown};
            return total;
        }
        EmuReg src;
        const size_t sz = (op == 0x88 || op == 0x8A) ? 1 : (w ? 8 : 4);
        const bool toReg = (op & 2) != 0;                     // ...01 forms write memory
        if (toReg) loadRm(total, sz, src);
        else src = st.gpr[reg];
        if (!toReg) {                                          // writing to memory: no model
            if (!rmIsReg) return total;
            EmuReg& d = st.gpr[rmReg];
            const bool k = src.known;
            uint64_t a = d.lo, b = src.lo;
            switch (op) {
                case 0x01: a += b; break;
                case 0x09: a |= b; break;
                case 0x21: a &= b; break;
                case 0x29: a -= b; break;
                case 0x31: a ^= b; break;
                case 0x88: case 0x89: a = b; break;
                default: break;
            }
            if (!w) a = (uint32_t)a;
            d = EmuReg{a, 0, (op == 0x89 || op == 0x88) ? k : (k && d.known)};
            return total;
        }
        EmuReg& d = st.gpr[reg];
        uint64_t a = d.lo, b = src.lo;
        const bool k = src.known && (op == 0x8B || op == 0x8A || op == 0x63 ? true : d.known);
        switch (op) {
            case 0x03: a += b; break;
            case 0x0B: a |= b; break;
            case 0x23: a &= b; break;
            case 0x2B: a -= b; break;
            case 0x33: a ^= b; break;
            case 0x8A: a = (a & ~0xFFull) | (b & 0xFF); break;
            case 0x8B: a = b; break;
            case 0x63: a = (uint64_t)(int64_t)(int32_t)b; break;
            default: break;
        }
        if (!w && op != 0x63) a = (uint32_t)a;
        d = EmuReg{a, 0, k};
        return total;
    }

    if (op == 0xC7 || op == 0xC1 || op == 0xD3 || op == 0x81 || op == 0x83 ||
        op == 0x69 || op == 0x6B || op == 0xF7) {
        if (!modrm(i + 1)) return 0;
        size_t total = i + 1 + mrmLen;
        const int sub = (p[i + 1] >> 3) & 7;
        int64_t imm = 0;
        if (op == 0xC7 || op == 0x81 || op == 0x69) {
            if (total + 4 > n) return 0;
            int32_t v; memcpy(&v, p + total, 4); imm = v; total += 4;
        } else if (op == 0xC1 || op == 0x83 || op == 0x6B) {
            if (total >= n) return 0;
            imm = (int8_t)p[total]; total += 1;
        }
        if (!rmIsReg) return total;                            // memory destination: no model
        EmuReg& d = st.gpr[rmReg];
        uint64_t a = d.lo;
        bool k = d.known;
        if (op == 0xC7) { a = (uint64_t)imm; k = true; }
        else if (op == 0x69 || op == 0x6B) { a = st.gpr[reg].lo * (uint64_t)imm; k = st.gpr[reg].known; }
        else if (op == 0xC1 || op == 0xD3) {
            const int c = (op == 0xD3) ? (int)(st.gpr[1].lo & 63) : (int)(imm & 63);
            if (op == 0xD3) k = k && st.gpr[1].known;
            const int bits = w ? 64 : 32;
            const uint64_t mask = w ? ~0ull : 0xFFFFFFFFull;
            a &= mask;
            switch (sub) {
                case 0: a = w ? emuRor64(a, bits - (c & 63)) : emuRor32((uint32_t)a, (bits - (c & 31)) & 31); break;
                case 1: a = w ? emuRor64(a, c) : emuRor32((uint32_t)a, c); break;
                case 4: a = (c >= bits) ? 0 : (a << c); break;
                case 5: a = (c >= bits) ? 0 : (a >> c); break;
                case 7: a = w ? (uint64_t)((int64_t)a >> (c > 63 ? 63 : c))
                              : (uint64_t)(uint32_t)((int32_t)a >> (c > 31 ? 31 : c)); break;
                default: break;
            }
            a &= mask;
        } else if (op == 0x81 || op == 0x83) {
            switch (sub) {
                case 0: a += (uint64_t)imm; break;
                case 1: a |= (uint64_t)imm; break;
                case 4: a &= (uint64_t)imm; break;
                case 5: a -= (uint64_t)imm; break;
                case 6: a ^= (uint64_t)imm; break;
                case 7: return total;                          // cmp
                default: break;
            }
        } else if (op == 0xF7) {
            if (sub == 2) a = ~a;                              // not
            else if (sub == 3) a = (uint64_t)(-(int64_t)a);    // neg
            else return total;
        }
        if (!w) a = (uint32_t)a;
        d = EmuReg{a, 0, k};
        return total;
    }

    if (op == 0x05 || op == 0x0D || op == 0x25 || op == 0x2D || op == 0x35) {  // alu eax, imm32
        if (i + 5 > n) return 0;
        int32_t v; memcpy(&v, p + i + 1, 4);
        EmuReg& d = st.gpr[0];
        uint64_t a = d.lo;
        switch (op) {
            case 0x05: a += (uint64_t)(int64_t)v; break;
            case 0x0D: a |= (uint32_t)v; break;
            case 0x25: a &= (uint32_t)v; break;
            case 0x2D: a -= (uint64_t)(int64_t)v; break;
            case 0x35: a ^= (uint32_t)v; break;
        }
        if (!w) a = (uint32_t)a;
        d.lo = a;
        return i + 5;
    }
    if (op == 0x3D || op == 0xA8 || op == 0xA9) return i + (op == 0xA8 ? 2 : 5);  // cmp / test

    st.unsupported = true;
    st.lastOpcode = op;
    return 0;
}
