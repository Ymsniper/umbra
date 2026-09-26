#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Reaches the engine's object array, so every live object can be found by
// reading rather than by searching memory for it.
//
// The game hides the array's address, and the arithmetic it hides it behind has
// changed with every patch: inlined word shuffles and a rotate, a called
// routine mixing with carry-less multiplication, an inline xor, rotate and add.
// Carrying constants for one of those shapes means a patch leaves this decoding
// garbage until someone reads the new instructions.
//
// So no shape is assumed here. The update tool finds the instructions the game
// itself reaches its object array with, proves them by walking the array, and
// writes them to gobjects.code beside offsets.cfg, with the register to read
// afterwards. This runs those bytes.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include "mem.hpp"
#include "gobjemu.hpp"
#include "runtime_offsets.hpp"

struct GObjectsView {
    uintptr_t chunks  = 0;      // the chunk pointer array
    int32_t   count   = 0;      // NumElements, where the code held one
    bool      ok      = false;
};

namespace godirect {

// What the update tool wrote: where the instructions sit, the instructions, and
// what to read once they have run.
struct CodeRecipe {
    uintptr_t rva = 0;
    int chunksReg = -1, countReg = -1;
    uintptr_t entryBase = 0, stride = 0, indexOff = 0;
    int chunkShift = 0;
    std::vector<unsigned char> code;
    bool ok = false;
};

// Beside offsets.cfg: the working directory, or one level up when the binary is
// launched from its build directory.
inline std::string recipePath() {
    for (const char* c : {"gobjects.code", "../gobjects.code"}) {
        std::ifstream probe(c);
        if (probe) return c;
    }
    return "gobjects.code";
}

inline const CodeRecipe& recipe() {
    static CodeRecipe r = [] {
        CodeRecipe out;
        std::ifstream f(recipePath());
        if (!f) return out;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            if (k == "rva") out.rva = strtoull(v.c_str(), nullptr, 0);
            else if (k == "chunks_reg") out.chunksReg = atoi(v.c_str());
            else if (k == "count_reg") out.countReg = atoi(v.c_str());
            else if (k == "entry_base") out.entryBase = strtoull(v.c_str(), nullptr, 0);
            else if (k == "stride") out.stride = strtoull(v.c_str(), nullptr, 0);
            else if (k == "index_off") out.indexOff = strtoull(v.c_str(), nullptr, 0);
            else if (k == "chunk_shift") out.chunkShift = atoi(v.c_str());
            else if (k == "code") {
                for (size_t i = 0; i + 1 < v.size(); i += 2)
                    out.code.push_back((unsigned char)strtoul(v.substr(i, 2).c_str(), nullptr, 16));
            }
        }
        out.ok = out.rva && out.chunksReg >= 0 && out.stride && out.chunkShift &&
                 !out.code.empty();
        return out;
    }();
    return r;
}

inline GObjectsView resolve(const Mem& mem) {
    GObjectsView g;
    const CodeRecipe& r = recipe();
    if (!r.ok) return g;

    // The instructions are run where the game runs them, so anything they load
    // relative to the instruction pointer lands on the right address.
    EmuState st;
    Emu emu(mem);
    emu.run(r.code.data(), r.code.size(), mem.modbase + r.rva, st);
    if (st.unsupported || !st.gpr[r.chunksReg].known) return g;

    g.chunks = st.gpr[r.chunksReg].lo;
    if (!g.chunks || (g.chunks & 7) || g.chunks > 0x7FFFFFFFFFFFull) { g.chunks = 0; return g; }
    if (r.countReg >= 0 && st.gpr[r.countReg].known) g.count = (int32_t)st.gpr[r.countReg].lo;
    if (g.count < 100 || g.count > 20000000) g.count = 0;      // walk the chunks instead
    g.ok = true;
    return g;
}

// entry = chunks[idx >> shift] + base + (idx & mask) * stride
inline uintptr_t objectAt(const Mem& mem, const GObjectsView& g, int32_t idx) {
    const CodeRecipe& r = recipe();
    if (!g.ok || idx < 0 || (g.count && idx >= g.count)) return 0;
    const int32_t per = 1 << r.chunkShift;
    const uintptr_t chunk = mem.readPtr(g.chunks + (uintptr_t)(idx / per) * 8);
    if (!chunk) return 0;
    return mem.readPtr(chunk + r.entryBase + (uintptr_t)(idx % per) * r.stride);
}

// Every object pointer, read in blocks. Where the code held no count, the
// chunks are walked until one is missing.
inline void allObjects(const Mem& mem, const GObjectsView& g, std::vector<uintptr_t>& out) {
    out.clear();
    if (!g.ok) return;
    const CodeRecipe& r = recipe();
    const int32_t perChunk = 1 << r.chunkShift;
    const int32_t nChunks = g.count ? (g.count + perChunk - 1) / perChunk : 64;
    std::vector<unsigned char> blk;
    for (int32_t c = 0; c < nChunks; c++) {
        const uintptr_t chunk = mem.readPtr(g.chunks + (uintptr_t)c * 8);
        if (!chunk || (chunk & 7)) break;
        int32_t n = perChunk;
        if (g.count) {
            n = g.count - c * perChunk;
            if (n > perChunk) n = perChunk;
            if (n <= 0) break;
        }
        const size_t bytes = (size_t)n * r.stride + r.entryBase;
        blk.resize(bytes);
        size_t got = 0;
        if (!mem.read_raw_partial(chunk, blk.data(), bytes, got) ||
            got < r.entryBase + r.stride)
            break;
        const size_t usable = (got - r.entryBase) / r.stride;
        for (size_t i = 0; i < usable; i++) {
            uintptr_t o;
            memcpy(&o, blk.data() + r.entryBase + i * r.stride, sizeof o);
            out.push_back(o);
        }
    }
}

// The object at index i has to report index i, which is what the update tool
// accepted these instructions on, checked again here against the running game.
inline bool verify(const Mem& mem, const GObjectsView& g, int samples = 64) {
    const CodeRecipe& r = recipe();
    if (!g.ok || !r.indexOff) return false;
    int agree = 0, tested = 0;
    for (int i = 0; i < samples; i++) {
        const uintptr_t o = objectAt(mem, g, i);
        if (!o || !mem.vtableInModule(mem.readPtr(o))) continue;
        tested++;
        if (mem.read<int32_t>(o + r.indexOff) == i) agree++;
    }
    return tested >= 8 && agree >= tested * 9 / 10;
}

}  // namespace godirect
