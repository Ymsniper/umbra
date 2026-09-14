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

// GObjects, by the arithmetic the game's own decode routine performs.
//
// The module holds the array's address as a 128-bit value, and the game passes
// it to a helper with two keys:
//
//     value = clmul(KeyA, clmul(KeyB, G.low) ^ G.high) ^ G.low
//
// where clmul is a carry-less multiply and only the low half of each product is
// kept, which is what PCLMULQDQ with an immediate of zero leaves in the low
// quadword. Every constant here comes from offsets.cfg, because every one of
// them is read out of the game's instructions by the update tool: the keys and
// the global at the call sites, the member offsets and their keys at the reads
// that follow, and the entry layout from the objects themselves. Nothing is
// carried over from a previous build, so a patch that changes any of it is a
// re-derivation rather than a silent wrong answer.

struct GObjectsView {
    uintptr_t base    = 0;      // the decoded FUObjectArray
    uintptr_t chunks  = 0;      // the chunk pointer array
    int32_t   count   = 0;      // NumElements
    bool      ok      = false;
};

namespace godirect {

inline uint64_t clmulLo(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

inline bool haveKeys() {
    return g_off.GObjects_RVA && g_off.GObjects_KeyA && g_off.GObjects_KeyB &&
           g_off.GObjects_NumKey && g_off.GObjects_ObjKey && g_off.GObjects_Stride &&
           g_off.GObjects_ChunkShift;
}

inline GObjectsView resolve(const Mem& mem) {
    GObjectsView g;
    if (!haveKeys()) return g;

    unsigned char raw[16];
    if (!mem.read_raw(mem.modbase + g_off.GObjects_RVA, raw, sizeof raw)) return g;
    uint64_t lo, hi;
    memcpy(&lo, raw, 8);
    memcpy(&hi, raw + 8, 8);
    if (!lo && !hi) return g;
    const uint64_t x = clmulLo((uint64_t)g_off.GObjects_KeyB, lo) ^ hi;
    g.base = (uintptr_t)(clmulLo((uint64_t)g_off.GObjects_KeyA, x) ^ lo);

    if (g.base < 0x10000 || g.base > 0x7FFFFFFFFFFFull || (g.base & 7)) { g.base = 0; return g; }

    g.count = (int32_t)__builtin_bswap32(
        mem.read<uint32_t>(g.base + g_off.GObjects_NumOff) ^ (uint32_t)g_off.GObjects_NumKey);
    g.chunks = (uintptr_t)__builtin_bswap64(
        mem.read<uint64_t>(g.base + g_off.GObjects_ObjOff) ^ (uint64_t)g_off.GObjects_ObjKey);

    if (g.count < 100 || g.count > 20000000) return g;
    if (!g.chunks || (g.chunks & 7)) return g;
    g.ok = true;
    return g;
}

// entry = chunks[idx >> ChunkShift] + EntryBase + (idx & mask) * Stride
inline uintptr_t objectAt(const Mem& mem, const GObjectsView& g, int32_t idx) {
    if (!g.ok || idx < 0 || idx >= g.count) return 0;
    const int32_t per = 1 << g_off.GObjects_ChunkShift;
    uintptr_t chunk = mem.readPtr(g.chunks + (uintptr_t)(idx / per) * 8);
    if (!chunk) return 0;
    return mem.readPtr(chunk + g_off.GObjects_EntryBase + (uintptr_t)(idx % per) * g_off.GObjects_Stride);
}

// Every object pointer, read in BLOCKS.
inline void allObjects(const Mem& mem, const GObjectsView& g,
                       std::vector<uintptr_t>& out) {
    out.clear();
    if (!g.ok) return;
    out.reserve(size_t(g.count));
    const int32_t perChunk = 1 << g_off.GObjects_ChunkShift;
    const int32_t nChunks = (g.count + perChunk - 1) / perChunk;
    const uintptr_t stride = g_off.GObjects_Stride;
    std::vector<unsigned char> blk;
    for (int32_t c = 0; c < nChunks; c++) {
        uintptr_t chunk = mem.readPtr(g.chunks + uintptr_t(c) * 8);
        if (!chunk) continue;
        int32_t first = c * perChunk;
        int32_t n = g.count - first;
        if (n > perChunk) n = perChunk;
        size_t bytes = size_t(n) * stride + g_off.GObjects_EntryBase;
        blk.resize(bytes);
        size_t got = 0;
        if (!mem.read_raw_partial(chunk, blk.data(), bytes, got) ||
            got < g_off.GObjects_EntryBase + stride)
            continue;
        size_t usable = (got - g_off.GObjects_EntryBase) / stride;
        for (size_t i = 0; i < usable; i++) {
            uintptr_t o;
            memcpy(&o, blk.data() + g_off.GObjects_EntryBase + i * stride, sizeof(o));
            out.push_back(o);
        }
        // Both paths index the same entries, so a disagreement means one of
        // them has the entry layout wrong and everything read through it
        // belongs to something else.
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
    if (!g.ok || !g_off.GObjects_IndexOff) return false;
    int agree = 0, tested = 0;
    for (int i = 0; i < samples && i < g.count; i++) {
        uintptr_t o = objectAt(mem, g, i);
        if (!o || !mem.vtableInModule(mem.readPtr(o))) continue;
        tested++;
        if (mem.read<int32_t>(o + g_off.GObjects_IndexOff) == i) agree++;
    }
    return tested >= 8 && agree >= tested * 9 / 10;
}

}  // namespace godirect
