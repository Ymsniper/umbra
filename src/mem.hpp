#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Process memory reads. Prefers the kernel module (/dev/suite_kmod)
// and falls back to process_vm_readv when it is not loaded.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/uio.h>
#include <sys/types.h>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <cmath>
#include <sys/ioctl.h>
#include <cerrno>
#include "../kmod/suite_kmod.h"   // optional kernel-module read backend

// Linux process memory reader  (replaces VMMDLL / DMA entirely)

struct MemRegion {
    uintptr_t base;
    size_t    size;
};

class Mem {
public:
    pid_t      pid       = -1;
    uintptr_t  modbase   = 0;   // lowest mapped address of the game module
    uintptr_t  modend    = 0;   // highest mapped address of the game module
    // Section ranges of the LIVE image, parsed from its PE headers in memory.
    // vtables live in .rdata; a real vtable's slots point into .text.
    uintptr_t  textStart = 0, textEnd  = 0;
    uintptr_t  rdataStart= 0, rdataEnd = 0;
    uintptr_t  dataStart = 0, dataEnd  = 0;
    bool       ok        = false;

    // ── optional kernel-module read backend
    mutable int  kmodFd    = -1;
    mutable bool kmodTried = false;
    // Live tally so you can SEE the module is actually carrying the reads, not
    // silently falling back. kmodOk = reads the module handled; kmodFellBack =
    // reads where the ioctl errored and process_vm_readv was used instead. If
    // kmodFellBack stays 0 while using the module, nothing fell back.
    mutable uint64_t kmodOk       = 0;
    mutable uint64_t kmodFellBack = 0;

    bool     usingKmod() const { return kmodFd >= 0; }

    bool kmodReady() const {
        if (!kmodTried) {
            kmodTried = true;
            kmodFd = ::open(SUITE_DEVICE_PATH, O_RDWR);
            if (kmodFd >= 0)
                printf("[mem] kernel backend: %s "
                       "(access_process_vm, ptrace_scope-independent)\n",
                       SUITE_DEVICE_PATH);
            else
                printf("[mem] no kernel backend at %s -- using "
                       "process_vm_readv\n", SUITE_DEVICE_PATH);
        }
        return kmodFd >= 0;
    }

    // Read up to sz bytes via the module. Returns bytes read (>=0), or -1 if the
    // ioctl itself failed (device gone/odd) so the caller can fall back.
    long kmodRead(uintptr_t addr, void* buf, size_t sz) const {
        struct suite_read_req req;
        req.target_pid = pid;
        req.address    = (SUITE_U64)addr;
        req.size       = (SUITE_U64)sz;
        req.out_ptr    = (SUITE_U64)(uintptr_t)buf;
        long r = ::ioctl(kmodFd, SUITE_READ_MEM, &req);
        return r;   // >=0: bytes read; <0: ioctl error
    }

    // ── find the game PID by exe name
    static pid_t findPID(const char* name) {
        DIR* d = opendir("/proc");
        if (!d) return -1;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_type != DT_DIR) continue;
            char path[320];
            snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;
            char comm[256] = {};
            fgets(comm, sizeof(comm), f);
            fclose(f);
            size_t l = strlen(comm);
            if (l && comm[l-1] == '\n') comm[l-1] = 0;
            if (strncmp(comm, name, strlen(name)) == 0) {
                closedir(d);
                return (pid_t)atoi(e->d_name);
            }
        }
        closedir(d);
        return -1;
    }

    // ── find PID by scanning /proc/*/maps for a path substring
    static pid_t findPIDByMaps(const char* modname) {
        DIR* d = opendir("/proc");
        if (!d) return -1;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_type != DT_DIR) continue;
            // Only numeric entries are process dirs
            bool numeric = true;
            for (const char* p = e->d_name; *p; ++p)
                if (*p < '0' || *p > '9') { numeric = false; break; }
            if (!numeric) continue;

            char path[320];
            snprintf(path, sizeof(path), "/proc/%s/maps", e->d_name);
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.find(modname) != std::string::npos) {
                    closedir(d);
                    return (pid_t)atoi(e->d_name);
                }
            }
        }
        closedir(d);
        return -1;
    }

    // ── get base address of a mapped module from /proc/<pid>/maps
    static uintptr_t getModuleBase(pid_t pid, const char* modname) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);
        std::ifstream f(path);
        std::string line;
        uintptr_t lowest = UINT64_MAX;
        bool found = false;
        while (std::getline(f, line)) {
            if (line.find(modname) == std::string::npos) continue;
            uintptr_t base = strtoull(line.c_str(), nullptr, 16);
            if (base && base < lowest) { lowest = base; found = true; }
        }
        return found ? lowest : 0;
    }

    // ── get full [base, end) span of the LIVE module image
    static void getModuleRange(pid_t pid, const char* modname,
                               uintptr_t& base_out, uintptr_t& end_out) {
        base_out = 0;
        end_out  = 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        int memfd = open(path, O_RDONLY);
        auto rd = [&](uintptr_t a, void* b, size_t n) -> bool {
            if (memfd < 0) return false;
            return pread(memfd, b, n, (off_t)a) == (ssize_t)n;
        };

        // Record the encrypted decoy so it can be excluded.
        uintptr_t encLo = 0, encHi = 0;
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);
        {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.find(modname) == std::string::npos) continue;
                uintptr_t b = strtoull(line.c_str(), nullptr, 16);
                if (!encLo || b < encLo) encLo = b;
            }
            if (encLo) encHi = encLo + 0x14000000;   // sections are anonymous
        }

        // Score every mapping start that holds a decrypted PE.
        uintptr_t bestBase = 0; uint32_t bestSize = 0; int bestScore = -1;
        std::ifstream f(path);
        std::string line;
        uintptr_t prev = 0;
        while (std::getline(f, line)) {
            uintptr_t b = strtoull(line.c_str(), nullptr, 16);
            if (!b || (b & 0xFFF) || b == prev) continue;
            prev = b;
            if (encLo && b >= encLo && b < encHi) continue;
            auto sp = line.find(' ');
            if (sp == std::string::npos || line[sp + 1] != 'r') continue;

            unsigned char mz[2];
            if (!rd(b, mz, 2) || mz[0] != 'M' || mz[1] != 'Z') continue;
            uint32_t lfanew = 0;
            if (!rd(b + 0x3C, &lfanew, 4) || lfanew < 0x40 || lfanew > 0x1000) continue;
            unsigned char sig[4];
            if (!rd(b + lfanew, sig, 4) || memcmp(sig, "PE\0\0", 4) != 0) continue;

            uint32_t sizeOfImage = 0; uint16_t chars = 0; uint64_t imgBase = 0;
            rd(b + lfanew + 24 + 0x38, &sizeOfImage, 4);
            rd(b + lfanew + 24 + 0x18, &imgBase, 8);
            rd(b + lfanew + 4 + 18,    &chars, 2);

            // The encrypted copy has a valid header too, so require real code:
            // decrypted x86-64 is ~6.0-6.6 bits/byte, ciphertext ~7.95.
            bool decrypted = false;
            unsigned char pg[4096];
            for (uintptr_t off : { (uintptr_t)0x1000, (uintptr_t)0x400000,
                                   (uintptr_t)0x4000000, (uintptr_t)0x8000000 }) {
                if (!rd(b + off, pg, sizeof(pg))) continue;
                unsigned cnt[256] = {0};
                bool nz = false;
                for (size_t i = 0; i < sizeof(pg); i++) { cnt[pg[i]]++; if (pg[i]) nz = true; }
                if (!nz) continue;
                double e = 0.0;
                for (int i = 0; i < 256; i++) if (cnt[i]) {
                    double p = (double)cnt[i] / (double)sizeof(pg);
                    e -= p * log2(p);
                }
                if (e < 7.2) { decrypted = true; break; }
            }
            if (!decrypted) continue;

            bool isExe      = (chars & 0x2000) == 0;             // IMAGE_FILE_DLL
            bool atPrefBase = (imgBase && b == (uintptr_t)imgBase);
            int  score      = (isExe ? 4 : 0) + (atPrefBase ? 2 : 0);
            if (score > bestScore || (score == bestScore && sizeOfImage > bestSize)) {
                bestScore = score; bestBase = b; bestSize = sizeOfImage;
            }
        }
        if (memfd >= 0) close(memfd);

        // Wine maps ~129 PEs of its own; anything this small is not the game.
        if (bestBase && bestSize >= (16u << 20)) {
            base_out = bestBase;
            end_out  = bestBase + bestSize;
        }
                               }

                               // ── initialise: find PID, module base, and module end
                               bool init(const char* procName, const char* moduleName) {
                                   pid = findPID(procName);
                                   if (pid < 0) {
                                       // /proc/comm didn't match - try maps-path scan (works under Proton)
                                       pid = findPIDByMaps(moduleName);
                                   }
                                   if (pid < 0) { printf("[mem] process '%s' not found\n", procName); return false; }
                                   printf("[mem] found PID %d\n", pid);

                                   getModuleRange(pid, moduleName, modbase, modend);
                                   if (!modbase) { printf("[mem] module '%s' not found in maps\n", moduleName); return false; }
                                   printf("[mem] modbase = 0x%lx  modend = 0x%lx (live decrypted image)\n", modbase, modend);
                                   ok = true;
                                   parseSections();
                                   return true;
                               }

                               // ── initialise with a known PID (Proton / Wine)
                               bool initByPID(pid_t given_pid, const char* moduleName) {
                                   pid = given_pid;
                                   printf("[mem] using PID %d directly\n", pid);

                                   getModuleRange(pid, moduleName, modbase, modend);
                                   if (!modbase) { printf("[mem] module '%s' not found in maps for PID %d\n", moduleName, pid); return false; }
                                   printf("[mem] modbase = 0x%lx  modend = 0x%lx (live decrypted image)\n", modbase, modend);
                                   ok = true;
                                   parseSections();
                                   return true;
                               }

                               // ── vtable validation
                               bool vtableInModule(uintptr_t vtable) const {
                                   if (vtable & 7) return false;          // vtables are pointer-aligned
                                   // Prefer the tight .rdata window. The full module span is ~312 MB
                                   // and its base has a constant high32, so packed (x,1) int32 pairs
                                   // pass a whole-module range check trivially.
                                   if (rdataEnd > rdataStart)
                                       return vtable >= rdataStart && vtable < rdataEnd;
                                   return modend > 0 && vtable >= modbase && vtable < modend;
                               }

                               // ── vtable CONTENT check
                               bool vtableIsReal(uintptr_t vtable) const {
                                   if (!vtableInModule(vtable)) return false;
                                   if (textEnd <= textStart)    return true;   // unknown .text: cannot judge
                                   uintptr_t slots[24];
                                   if (!read_raw(vtable, slots, sizeof(slots))) return false;
                                   int good = 0;
                                   for (int i = 0; i < 24; i++)
                                       if (slots[i] >= textStart && slots[i] < textEnd) good++;
                                   return good >= 20;
                               }

                               // ── parse the live image's section table
                               void parseSections() {
                                   textStart = textEnd = rdataStart = rdataEnd = dataStart = dataEnd = 0;
                                   if (!modbase) return;
                                   uint32_t lfanew = 0;
                                   if (!read_raw(modbase + 0x3C, &lfanew, 4)) return;
                                   uint16_t nSect = 0, optSz = 0;
                                   read_raw(modbase + lfanew + 6,  &nSect, 2);
                                   read_raw(modbase + lfanew + 20, &optSz, 2);
                                   if (!nSect || nSect > 96) return;
                                   uintptr_t tbl = modbase + lfanew + 24 + optSz;
                                   for (uint16_t i = 0; i < nSect; i++) {
                                       unsigned char sh[40];
                                       if (!read_raw(tbl + (uintptr_t)i * 40, sh, sizeof(sh))) continue;
                                       char nm[9] = {0}; memcpy(nm, sh, 8);
                                       uint32_t vsize = 0, vaddr = 0;
                                       memcpy(&vsize, sh + 8, 4); memcpy(&vaddr, sh + 12, 4);
                                       if (!vaddr || !vsize) continue;
                                       uintptr_t s = modbase + vaddr, e = s + vsize;
                                       if      (!strcmp(nm, ".text")) { textStart = s; textEnd = e; }
                                       else if (!strcmp(nm, ".data")) { dataStart = s; dataEnd = e; }
                                       else if (!strcmp(nm, ".rdata")) {   // two .rdata sections: take the union
                                           if (!rdataStart || s < rdataStart) rdataStart = s;
                                           if (e > rdataEnd)                  rdataEnd   = e;
                                       }
                                   }
                                   printf("[mem] .text 0x%lx-0x%lx  .rdata 0x%lx-0x%lx  .data 0x%lx-0x%lx\n",
                                          textStart, textEnd, rdataStart, rdataEnd, dataStart, dataEnd);
                               }

                               // ── core read: kernel module if present, else process_vm_readv
                               bool read_raw(uintptr_t addr, void* buf, size_t sz) const {
                                   if (!addr || !buf || !sz) return false;
                                   if (kmodReady()) {
                                       long r = kmodRead(addr, buf, sz);
                                       if (r >= 0) { kmodOk++; return r == (long)sz; }
                                       kmodFellBack++;   // ioctl failed - use pvr
                                   }
                                   struct iovec local  = { buf,         sz };
                                   struct iovec remote = { (void*)addr, sz };
                                   ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
                                   return n == (ssize_t)sz;
                               }

                               // ── partial read: returns however many bytes were readable
                               bool read_raw_partial(uintptr_t addr, void* buf, size_t sz, size_t& got) const {
                                   got = 0;
                                   if (!addr || !buf || !sz) return false;
                                   if (kmodReady()) {
                                       // access_process_vm short-reads at the first unmapped page
                                       // and returns the count, which is exactly this contract.
                                       long r = kmodRead(addr, buf, sz);
                                       if (r >= 0) { kmodOk++; got = (size_t)r; return got > 0; }
                                       kmodFellBack++;   // ioctl error - use the pvr page-walk
                                   }
                                   struct iovec local  = { buf,         sz };
                                   struct iovec remote = { (void*)addr, sz };
                                   ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
                                   if (n > 0) { got = (size_t)n; return true; }
                                   // Full-range read faulted immediately - walk page by page.
                                   static constexpr size_t PG = 4096;
                                   uint8_t* out = (uint8_t*)buf;
                                   size_t pos = 0;
                                   // Align first step to page boundary
                                   while (pos < sz) {
                                       size_t step = std::min(PG, sz - pos);
                                       struct iovec l = { out + pos,        step };
                                       struct iovec r = { (void*)(addr+pos), step };
                                       ssize_t m = process_vm_readv(pid, &l, 1, &r, 1, 0);
                                       if (m == (ssize_t)step) { pos += step; got = pos; }
                                       else break;
                                   }
                                   return got > 0;
                               }

                               // ── typed helpers
                               template<typename T>
                               T read(uintptr_t addr) const {
                                   T val{};
                                   read_raw(addr, &val, sizeof(T));
                                   return val;
                               }

                               template<typename T>
                               T read(uintptr_t base, ptrdiff_t off) const {
                                   return read<T>(base + off);
                               }

                               // ── pointer reads
                               uintptr_t readPtr(uintptr_t addr) const {
                                   return read<uintptr_t>(addr);
                               }

                               uintptr_t readPtr(uintptr_t base, ptrdiff_t off) const {
                                   return readPtr(base + off);
                               }

                               // ── pattern scan
                               uintptr_t patternScan(uintptr_t start, size_t len,
                                                     const uint8_t* pat, const char* mask) const {
                                                         size_t patLen = strlen(mask);
                                                         std::vector<uint8_t> buf(len);
                                                         if (!read_raw(start, buf.data(), len)) return 0;
                                                         for (size_t i = 0; i + patLen <= len; ++i) {
                                                             bool found = true;
                                                             for (size_t j = 0; j < patLen; ++j)
                                                                 if (mask[j] == 'x' && buf[i+j] != pat[j]) { found = false; break; }
                                                             if (found) return start + i;
                                                         }
                                                         return 0;
                                                     }

                                                     // ── get executable regions for this module
                                                     std::vector<MemRegion> getExecRegions(const char* modname) const {
                                                         std::vector<MemRegion> out;
                                                         char path[64];
                                                         snprintf(path, sizeof(path), "/proc/%d/maps", pid);
                                                         std::ifstream f(path);
                                                         std::string line;
                                                         while (std::getline(f, line)) {
                                                             if (line.find(modname) == std::string::npos) continue;
                                                             if (line.find("r-xp") == std::string::npos &&
                                                                 line.find("rwxp") == std::string::npos) continue;
                                                             uintptr_t b = strtoull(line.c_str(), nullptr, 16);
                                                             auto dash = line.find('-');
                                                             uintptr_t e = strtoull(line.c_str() + dash + 1, nullptr, 16);
                                                             out.push_back({b, e - b});
                                                         }
                                                         return out;
                                                     }
};

// ── global singleton
inline Mem g_mem;
