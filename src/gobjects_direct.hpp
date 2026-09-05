#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Decodes the game's obfuscated FUObjectArray so every live object
// can be enumerated without scanning the heap.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "mem.hpp"
#include "runtime_offsets.hpp"

// GObjects, straight from the game's own routine.

struct GObjectsView {
    uintptr_t base    = 0;      // the decoded FUObjectArray
    uintptr_t chunks  = 0;      // the chunk pointer array
    int32_t   count   = 0;      // NumElements
    bool      ok      = false;
};

namespace godirect {
inline uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

// pshuflw: reorder the four 16-bit words of the low quadword by imm
inline uint64_t shuflw(uint64_t v, uint8_t imm) {
    uint16_t w[4] = { uint16_t(v), uint16_t(v >> 16),
                      uint16_t(v >> 32), uint16_t(v >> 48) };
    uint64_t out = 0;
    for (int i = 0; i < 4; i++)
        out |= uint64_t(w[(imm >> (2 * i)) & 3]) << (16 * i);
    return out;
}

inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

inline GObjectsView resolve(const Mem& mem) {
    GObjectsView g;
    if (!g_off.GObjects_RVA) return g;

    uint64_t raw = mem.read<uint64_t>(mem.modbase + g_off.GObjects_RVA);
    if (!raw) return g;

    // shuf(imm1) -> ror32 per lane -> shuf(imm2) -> XOR, every part from the
    // config because every part was read out of the game's own instructions
    uint64_t v = shuflw(raw, goImm1());
    uint32_t lo = ror32(uint32_t(v), goShift()),
             hi = ror32(uint32_t(v >> 32), goShift());
    v = (uint64_t(hi) << 32) | lo;
    v = shuflw(v, goImm2());
    g.base = uintptr_t(v ^ g_off.GObjects_Key);

    if (g.base < 0x10000 || g.base > 0x7FFFFFFFFFFFull) return g;

    g.count  = int32_t(bswap32(mem.read<uint32_t>(g.base + goNumOff())
                               ^ uint32_t(g_off.GObjects_NumKey)));
    g.chunks = uintptr_t(bswap64(mem.read<uint64_t>(g.base + goObjOff())
                                 ^ uint64_t(g_off.GObjects_ObjKey)));

    if (g.count < 100 || g.count > 20000000) return g;
    if (!g.chunks || (g.chunks & 7)) return g;
    g.ok = true;
    return g;
}

// entry = chunks[idx >> 16] + 8 + (idx & 0xFFFF) * 0x18
inline uintptr_t objectAt(const Mem& mem, const GObjectsView& g, int32_t idx) {
    if (!g.ok || idx < 0 || idx >= g.count) return 0;
    uintptr_t chunk = mem.readPtr(g.chunks + uintptr_t(idx >> 16) * 8);
    if (!chunk) return 0;
    return mem.readPtr(chunk + 8 + uintptr_t(idx & 0xFFFF) * 0x18);
}

// Every object pointer, read in BLOCKS.
inline void allObjects(const Mem& mem, const GObjectsView& g,
                       std::vector<uintptr_t>& out) {
    out.clear();
    if (!g.ok) return;
    out.reserve(size_t(g.count));
    const int32_t perChunk = 65536;
    const int32_t nChunks = (g.count + perChunk - 1) / perChunk;
    std::vector<unsigned char> blk;
    for (int32_t c = 0; c < nChunks; c++) {
        uintptr_t chunk = mem.readPtr(g.chunks + uintptr_t(c) * 8);
        if (!chunk) continue;
        int32_t first = c * perChunk;
        int32_t n = g.count - first;
        if (n > perChunk) n = perChunk;
        size_t bytes = size_t(n) * 0x18 + 8;
        blk.resize(bytes);
        size_t got = 0;
        if (!mem.read_raw_partial(chunk, blk.data(), bytes, got) || got < 0x20)
            continue;
        size_t usable = (got - 8) / 0x18;
        for (size_t i = 0; i < usable; i++) {
            uintptr_t o;
            memcpy(&o, blk.data() + 8 + i * 0x18, sizeof(o));
            out.push_back(o);
        }
        // The two read paths MUST agree. objectAt() found the real controller
        // and allObjects() then found two garbage cycles from the same array --
        // a disagreement that went unnoticed because nothing compared them.
        if (c == 0 && usable > 4) {
            for (int t = 0; t < 4; t++) {
                uintptr_t viaOne = objectAt(mem, g, t);
                uintptr_t viaBlk = out[size_t(t)];
                if (viaOne != viaBlk) {
                    printf("[gobjects] READ PATHS DISAGREE at index %d: "
                           "objectAt 0x%lx  block 0x%lx\n",
                           t, (unsigned long)viaOne, (unsigned long)viaBlk);
                    break;
                }
            }
        }
    }
}

inline bool verify(const Mem& mem, const GObjectsView& g, int samples = 64) {
    if (!g.ok) return false;
    int agree = 0, tested = 0;
    for (int i = 0; i < samples && i < g.count; i++) {
        uintptr_t o = objectAt(mem, g, i);
        if (!o || !mem.vtableInModule(mem.readPtr(o))) continue;
        tested++;
        if (mem.read<int32_t>(o + 0x0C) == i) agree++;
    }
    return tested >= 8 && agree >= tested * 9 / 10;
}

}  // namespace godirect
