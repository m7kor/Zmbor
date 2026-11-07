#include "runtime/zmbor_trace.h"
#include "runtime/ps2_byte_fingerprint.h"

namespace
{
    std::string readGuestCStringBounded(const uint8_t *rdram, uint32_t guestAddr, size_t maxBytes)
    {
        std::string out;
        if (!rdram || guestAddr == 0 || maxBytes == 0)
        {
            return out;
        }

        out.reserve(maxBytes);
        for (size_t i = 0; i < maxBytes; ++i)
        {
            const char ch = static_cast<char>(rdram[(guestAddr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            out.push_back(ch);
        }
        return out;
    }

    std::string normalizeSifModulePathKey(const std::string &path)
    {
        return toLowerAscii(normalizePs2PathSuffix(path));
    }

    uint64_t hashGuestBytesFnv1a64(const uint8_t *rdram, uint32_t guestAddr, size_t byteCount)
    {
        constexpr uint64_t kOffset = 1469598103934665603ull;
        constexpr uint64_t kPrime = 1099511628211ull;

        if (!rdram || guestAddr == 0 || byteCount == 0)
        {
            return 0ull;
        }

        uint64_t hash = kOffset;
        for (size_t i = 0; i < byteCount; ++i)
        {
            const uint8_t b = rdram[(guestAddr + static_cast<uint32_t>(i)) & PS2_RAM_MASK];
            hash ^= static_cast<uint64_t>(b);
            hash *= kPrime;
        }
        return hash;
    }

    bool copyGuestBytesBounded(const uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t byteCount,
                               uint32_t maxBytes,
                               std::vector<uint8_t> &out)
    {
        out.clear();
        if (byteCount == 0u)
        {
            return true;
        }
        if (!rdram || guestAddr == 0u || byteCount > maxBytes)
        {
            return false;
        }

        out.resize(byteCount);
        for (uint32_t i = 0; i < byteCount; ++i)
        {
            const uint8_t *src = getConstMemPtr(rdram, guestAddr + i);
            if (!src)
            {
                out.clear();
                return false;
            }
            out[i] = *src;
        }
        return true;
    }

    std::string makeSifModuleBufferTag(const uint8_t *rdram, uint32_t bufferAddr)
    {
        char key[96] = {};
        const uint64_t hash = hashGuestBytesFnv1a64(rdram, bufferAddr, kSifModuleBufferProbeBytes);
        std::snprintf(key, sizeof(key), "iopbuf:fnv64:%016llx", static_cast<unsigned long long>(hash));
        return std::string(key);
    }

    void logSifModuleAction(const char *op, int32_t moduleId, const std::string &path, uint32_t refCount)
    {
        if (!op)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        if (g_sif_module_log_count >= kMaxSifModuleLogs)
        {
            return;
        }

        RUNTIME_LOG("[SIF module] " << op
                  << " id=" << moduleId
                  << " ref=" << refCount
                  << " path=\"" << path << "\""
                  << std::endl);
        ++g_sif_module_log_count;
    }

    int32_t trackSifModuleLoadExternal(const std::string &path, int32_t moduleId)
    {
        if (path.empty() || moduleId <= 0)
        {
            return -1;
        }

        const std::string pathKey = normalizeSifModulePathKey(path);
        if (pathKey.empty())
        {
            return -1;
        }

        std::lock_guard<std::mutex> lock(g_sif_module_mutex);

        auto idIt = g_sif_modules_by_id.find(moduleId);
        if (idIt != g_sif_modules_by_id.end())
        {
            SifModuleRecord &record = idIt->second;
            if (record.pathKey == pathKey)
            {
                record.loaded = true;
                ++record.refCount;
                return moduleId;
            }

            if (!record.pathKey.empty())
            {
                auto oldPathIt = g_sif_module_id_by_path.find(record.pathKey);
                if (oldPathIt != g_sif_module_id_by_path.end() && oldPathIt->second == moduleId)
                {
                    g_sif_module_id_by_path.erase(oldPathIt);
                }
            }
        }

        auto pathIt = g_sif_module_id_by_path.find(pathKey);
        if (pathIt != g_sif_module_id_by_path.end() && pathIt->second != moduleId)
        {
            auto oldIt = g_sif_modules_by_id.find(pathIt->second);
            if (oldIt != g_sif_modules_by_id.end())
            {
                oldIt->second.loaded = false;
                oldIt->second.refCount = 0;
            }
        }

        SifModuleRecord record;
        record.id = moduleId;
        record.path = path;
        record.pathKey = pathKey;
        record.refCount = 1;
        record.loaded = true;
        g_sif_module_id_by_path[pathKey] = moduleId;
        g_sif_modules_by_id[moduleId] = std::move(record);
        if (moduleId >= g_next_sif_module_id)
        {
            g_next_sif_module_id = moduleId + 1;
        }
        return moduleId;
    }

    bool trackSifModuleStop(int32_t moduleId, uint32_t *remainingRefs = nullptr)
    {
        if (moduleId <= 0)
        {
            if (remainingRefs)
            {
                *remainingRefs = 0;
            }
            return false;
        }

        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        auto it = g_sif_modules_by_id.find(moduleId);
        if (it == g_sif_modules_by_id.end())
        {
            if (remainingRefs)
            {
                *remainingRefs = 0;
            }
            return false;
        }

        SifModuleRecord &record = it->second;
        if (record.refCount > 0)
        {
            --record.refCount;
        }
        record.loaded = (record.refCount != 0);

        if (remainingRefs)
        {
            *remainingRefs = record.refCount;
        }
        return true;
    }

    bool readFileBlockAt(std::ifstream &file, uint64_t offset, void *dst, size_t byteCount)
    {
        if (!dst || byteCount == 0)
        {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file)
        {
            return false;
        }

        file.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(byteCount));
        return file.gcount() == static_cast<std::streamsize>(byteCount);
    }

    bool tryExtractElfGpValue(std::ifstream &file, const Elf32Header &header, uint32_t &gpOut)
    {
        uint8_t regInfo[24] = {};

        for (uint32_t i = 0; i < header.phnum; ++i)
        {
            Elf32ProgramHeader ph{};
            const uint64_t phOffset = static_cast<uint64_t>(header.phoff) + static_cast<uint64_t>(i) * header.phentsize;
            if (!readFileBlockAt(file, phOffset, &ph, sizeof(ph)))
            {
                return false;
            }

            if (ph.type == kElfPtMipsRegInfo && ph.filesz >= sizeof(regInfo))
            {
                if (!readFileBlockAt(file, ph.offset, regInfo, sizeof(regInfo)))
                {
                    return false;
                }
                std::memcpy(&gpOut, regInfo + 20u, sizeof(gpOut));
                return true;
            }
        }

        for (uint32_t i = 0; i < header.shnum; ++i)
        {
            Elf32SectionHeader sh{};
            const uint64_t shOffset = static_cast<uint64_t>(header.shoff) + static_cast<uint64_t>(i) * header.shentsize;
            if (!readFileBlockAt(file, shOffset, &sh, sizeof(sh)))
            {
                return false;
            }

            if (sh.type == kElfShtMipsRegInfo && sh.size >= sizeof(regInfo))
            {
                if (!readFileBlockAt(file, sh.offset, regInfo, sizeof(regInfo)))
                {
                    return false;
                }
                std::memcpy(&gpOut, regInfo + 20u, sizeof(gpOut));
                return true;
            }
        }

        return false;
    }

    bool loadElfIntoGuestMemory(const std::string &hostPath,
                                uint8_t *rdram,
                                PS2Runtime *runtime,
                                const std::string &sectionName,
                                uint32_t guestPc,
                                GuestExecData &execDataOut,
                                std::string &errorOut)
    {
        if (!rdram || hostPath.empty())
        {
            errorOut = "invalid path or RDRAM pointer";
            return false;
        }

        std::ifstream file(hostPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            errorOut = "failed to open ELF";
            return false;
        }

        const std::streamoff fileSize = file.tellg();
        Elf32Header header{};
        if (fileSize < static_cast<std::streamoff>(sizeof(header)) ||
            !readFileBlockAt(file, 0, &header, sizeof(header)))
        {
            errorOut = "failed to read ELF header";
            return false;
        }

        if (header.magic != kElfMagic || header.machine != kElfMachineMips || header.type != kElfTypeExec)
        {
            errorOut = "not a MIPS executable ELF";
            return false;
        }
        if (header.elfClass != 1u || header.endianness != 1u)
        {
            errorOut = "unsupported ELF format (expected 32-bit little-endian)";
            return false;
        }

        const uint64_t programTableEnd = static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(header.phnum) * header.phentsize;
        if (header.phnum != 0u &&
            (header.phentsize < sizeof(Elf32ProgramHeader) || programTableEnd > static_cast<uint64_t>(fileSize)))
        {
            errorOut = "ELF program-header table is out of range";
            return false;
        }

        const bool loadAll = sectionName.empty() || toLowerAscii(sectionName) == "all";
        static uint32_t secFilterLogCount = 0;
        if (!loadAll && secFilterLogCount < 8u)
        {
            RUNTIME_LOG("[SifLoadElfPart] section filter \"" << sectionName
                      << "\" requested; loading PT_LOAD segments only." << std::endl);
            ++secFilterLogCount;
        }

        struct LoadSegment
        {
            Elf32ProgramHeader header;
            uint8_t *destination;
        };
        std::vector<LoadSegment> segments;
        segments.reserve(header.phnum);

        // Resolve and validate the complete load plan before changing guest memory.
        for (uint32_t i = 0; i < header.phnum; ++i)
        {
            Elf32ProgramHeader ph{};
            const uint64_t phOffset = static_cast<uint64_t>(header.phoff) + static_cast<uint64_t>(i) * header.phentsize;
            if (!readFileBlockAt(file, phOffset, &ph, sizeof(ph)))
            {
                errorOut = "failed to read ELF program headers";
                return false;
            }
            if (ph.type != kElfPtLoad)
            {
                continue;
            }
            if (ph.filesz > ph.memsz)
            {
                errorOut = "ELF segment filesz > memsz";
                return false;
            }
            if (ph.memsz == 0u)
            {
                continue;
            }
            if (ph.filesz != 0u && static_cast<uint64_t>(ph.offset) + ph.filesz > static_cast<uint64_t>(fileSize))
            {
                errorOut = "ELF segment exceeds file bounds";
                return false;
            }

            if (runtime && ps2IsScratchpadAddress(ph.vaddr))
            {
                const uint32_t scratchOffset = ps2ScratchpadOffset(ph.vaddr);
                if (static_cast<uint64_t>(scratchOffset) + ph.memsz > PS2_SCRATCHPAD_SIZE)
                {
                    errorOut = "ELF scratchpad segment out of range";
                    return false;
                }

                uint8_t *scratch = runtime->memory().getScratchpad();
                if (!scratch)
                {
                    errorOut = "ELF scratchpad segment has no memory backing";
                    return false;
                }
                segments.push_back({ph, scratch + scratchOffset});
                continue;
            }

            uint32_t physAddr;
            try
            {
                physAddr = runtime ? runtime->memory().translateAddress(ph.vaddr) : (ph.vaddr & PS2_RAM_MASK);
            }
            catch (const Ps2TlbMissError &e)
            {
                errorOut = e.what();
                return false;
            }
            const uint64_t end = static_cast<uint64_t>(physAddr) + ph.memsz;
            if (end > PS2_RAM_SIZE)
            {
                errorOut = "ELF RDRAM segment out of range";
                return false;
            }
            const std::string overlap = PS2Runtime::describeRuntimeReservedOverlap(physAddr, end);
            if (!overlap.empty())
            {
                errorOut = "ELF segment overlaps the runtime's reserved EE kernel area (" + overlap + ")";
                return false;
            }
            segments.push_back({ph, rdram + physAddr});
        }

        if (segments.empty())
        {
            errorOut = "ELF has no loadable segments";
            return false;
        }

        const bool traceLoads = zmbor_trace::enabled(zmbor_trace::kLoad);
        for (const auto &[ph, destination] : segments)
        {
            if (ph.filesz != 0u && !readFileBlockAt(file, ph.offset, destination, ph.filesz))
            {
                errorOut = "failed to read ELF segment payload";
                return false;
            }
            if (ph.memsz > ph.filesz)
            {
                std::memset(destination + ph.filesz, 0, ph.memsz - ph.filesz);
            }
            if (traceLoads)
            {
                const bool scratch = runtime && ps2IsScratchpadAddress(ph.vaddr);
                const auto *base = scratch ? runtime->memory().getScratchpad() : rdram;
                const auto offset = static_cast<uint32_t>(destination - base);
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                              "vaddr=0x%08x region=%s offset=0x%08x filesz=%u memsz=%u fnv1a64=%016llx",
                              ph.vaddr, scratch ? "scratchpad" : "rdram", offset, ph.filesz, ph.memsz,
                              static_cast<unsigned long long>(ps2ByteFingerprint(destination, ph.memsz)));
                zmbor_trace::record(zmbor_trace::kLoad, "elf:sif-segment", guestPc, detail);
            }
        }

        execDataOut.epc = header.entry;
        execDataOut.gp = 0u;
        execDataOut.sp = 0u;
        execDataOut.dummy = 0u;

        uint32_t gpValue = 0u;
        if (tryExtractElfGpValue(file, header, gpValue))
        {
            execDataOut.gp = gpValue;
        }

        return true;
    }

    int32_t runSifLoadElfPart(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              uint32_t pathAddr,
                              const std::string &sectionName,
                              uint32_t execDataAddr)
    {
        if (!rdram || !ctx)
        {
            return -1;
        }

        const std::string ps2Path = readGuestCStringBounded(rdram, pathAddr, kLoadfilePathMaxBytes);
        if (ps2Path.empty())
        {
            return -1;
        }

        const std::string hostPath = translatePs2Path(ps2Path.c_str());
        if (hostPath.empty())
        {
            return -1;
        }

        GuestExecData execData{};
        std::string loadError;
        if (!loadElfIntoGuestMemory(hostPath, rdram, runtime, sectionName, ctx->pc, execData, loadError))
        {
            static uint32_t logCount = 0;
            if (logCount < 16u)
            {
                std::cerr << "[SifLoadElfPart] failed path=\"" << ps2Path << "\" host=\"" << hostPath
                          << "\" reason=" << loadError << std::endl;
                ++logCount;
            }
            return -1;
        }

        if (execData.gp == 0u)
        {
            execData.gp = getRegU32(ctx, 28);
        }
        execData.sp = getRegU32(ctx, 29);

        if (execDataAddr != 0u)
        {
            GuestExecData *guestExec = reinterpret_cast<GuestExecData *>(getMemPtr(rdram, execDataAddr));
            if (!guestExec)
            {
                return -1;
            }
            std::memcpy(guestExec, &execData, sizeof(execData));
        }

        static uint32_t successLogs = 0;
        if (successLogs < 16u)
        {
            RUNTIME_LOG("[SifLoadElfPart] loaded \"" << ps2Path << "\" epc=0x"
                      << std::hex << execData.epc << " gp=0x" << execData.gp << std::dec << std::endl);
            ++successLogs;
        }

        return 0;
    }
}
