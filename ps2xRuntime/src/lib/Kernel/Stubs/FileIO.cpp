#include "Common.h"
#include "FileIO.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <vector>

namespace ps2_stubs
{
    void sceFsDbChk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsDbChk", rdram, ctx, runtime);
    }

    void sceFsIntrSigSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsIntrSigSema", rdram, ctx, runtime);
    }

    void sceFsSemExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSemExit", rdram, ctx, runtime);
    }

    void sceFsSemInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSemInit", rdram, ctx, runtime);
    }

    void sceFsSigSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSigSema", rdram, ctx, runtime);
    }

    void close(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioClose(rdram, ctx, runtime);
    }

    void fstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t statAddr = getRegU32(ctx, 5);
        if (uint8_t *statBuf = getMemPtr(rdram, statAddr))
        {
            std::memset(statBuf, 0, 128);
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, -1);
    }

    void lseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioLseek(rdram, ctx, runtime);
    }

    void open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioOpen(rdram, ctx, runtime);
    }

    void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioRead(rdram, ctx, runtime);
    }

    void sceClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioClose(rdram, ctx, runtime);
    }

    // Directory listing (sceDopen/sceDread/sceDclose) over the VFS host folder. The SDK sends these to the IOP's
    // FILEIO server, which this runtime does not bind on the EE side. Like the CD file system, entries start with
    // "." and "..", and file names on cdrom0: carry the ISO9660 version suffix ";1".
    namespace
    {
        struct GuestDirEntry
        {
            std::string name;
            bool directory = false;
            uint64_t size = 0u;
        };
        struct GuestDir
        {
            std::vector<GuestDirEntry> entries;
            size_t next = 0u;
        };
        std::mutex g_guestDirMutex;
        std::map<int32_t, GuestDir> g_guestDirs;
        int32_t g_nextGuestDir = 0x40;
    }

    void sceDopen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const std::string path = readPs2CStringBounded(rdram, runtime, getRegU32(ctx, 4), 260);
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
        std::filesystem::path host;
        std::error_code error;
        if (!runtime || path.empty() || !runtime->vfs().resolveHostPath(path, mounts, host) ||
            !std::filesystem::is_directory(host, error))
        {
            RUNTIME_LOG("[sceDopen] \"" << sanitizeForLog(path) << "\" -> not a directory" << std::endl);
            setReturnS32(ctx, -2); // ENOENT
            return;
        }
        std::string device = path.substr(0, path.find(':'));
        std::transform(device.begin(), device.end(), device.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool cd = device.rfind("cdrom", 0) == 0;

        GuestDir dir;
        dir.entries.push_back({".", true, 0u});
        dir.entries.push_back({"..", true, 0u});
        std::vector<GuestDirEntry> found;
        for (const auto &item : std::filesystem::directory_iterator(host, error))
        {
            std::string name = item.path().filename().string();
            if (name.empty() || name[0] == '.')
                continue; // host-only entries such as .disc
            GuestDirEntry entry;
            entry.directory = item.is_directory(error);
            entry.size = entry.directory ? 0u : static_cast<uint64_t>(item.file_size(error));
            if (cd)
            {
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (!entry.directory)
                    name += ";1";
            }
            entry.name = name;
            found.push_back(std::move(entry));
        }
        std::sort(found.begin(), found.end(), [](const GuestDirEntry &a, const GuestDirEntry &b) { return a.name < b.name; });
        dir.entries.insert(dir.entries.end(), found.begin(), found.end());

        std::lock_guard<std::mutex> lock(g_guestDirMutex);
        const int32_t fd = g_nextGuestDir++;
        RUNTIME_LOG("[sceDopen] \"" << sanitizeForLog(path) << "\" -> fd=" << fd << " entries=" << dir.entries.size() << std::endl);
        g_guestDirs[fd] = std::move(dir);
        setReturnS32(ctx, fd);
    }

    void sceDread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        uint8_t *dirent = getMemPtr(rdram, getRegU32(ctx, 5));
        std::lock_guard<std::mutex> lock(g_guestDirMutex);
        auto it = g_guestDirs.find(fd);
        if (it == g_guestDirs.end() || !dirent)
        {
            setReturnS32(ctx, -9); // EBADF
            return;
        }
        GuestDir &dir = it->second;
        if (dir.next >= dir.entries.size())
        {
            setReturnS32(ctx, 0);
            return;
        }
        const GuestDirEntry &entry = dir.entries[dir.next++];
        // sce_dirent: sce_stat (mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6]) = 0x40 bytes,
        // then name[256] and a private pointer.
        std::memset(dirent, 0, 0x40 + 256 + 4);
        const uint32_t mode = entry.directory ? 0x1000u | 0x16Du : 0x2000u | 0x124u; // FIO_S_IFDIR / FIO_S_IFREG
        const uint32_t size = static_cast<uint32_t>(entry.size);
        const uint32_t hisize = static_cast<uint32_t>(entry.size >> 32);
        std::memcpy(dirent + 0x00, &mode, 4);
        std::memcpy(dirent + 0x08, &size, 4);
        std::memcpy(dirent + 0x24, &hisize, 4);
        const size_t length = std::min<size_t>(entry.name.size(), 255u);
        std::memcpy(dirent + 0x40, entry.name.data(), length);
        setReturnS32(ctx, static_cast<int32_t>(length));
    }

    void sceDclose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_guestDirMutex);
        setReturnS32(ctx, g_guestDirs.erase(static_cast<int32_t>(getRegU32(ctx, 4))) ? 0 : -9);
    }

    void sceFsInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceFsReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceIoctl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t cmd = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t argAddr = getRegU32(ctx, 6);

        // HTCI wait paths poll sceIoctl(fd, 1, &state) and expect state to move
        // away from 1 once host-side I/O is no longer busy.
        if (cmd == 1 && argAddr != 0u)
        {
            uint8_t *argPtr = getMemPtr(rdram, argAddr);
            if (!argPtr)
            {
                setReturnS32(ctx, -1);
                return;
            }

            const uint32_t ready = 0u;
            std::memcpy(argPtr, &ready, sizeof(ready));
        }

        setReturnS32(ctx, 0);
    }

    void sceLseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioLseek(rdram, ctx, runtime);
    }

    void sceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioOpen(rdram, ctx, runtime);
    }

    void sceRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioRead(rdram, ctx, runtime);
    }

    void sceWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioWrite(rdram, ctx, runtime);
    }

    void stat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t statAddr = getRegU32(ctx, 5);
        uint8_t *statBuf = getMemPtr(rdram, statAddr);
        if (!statBuf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        // Minimal fake stat payload: zeroed structure indicates a valid, readable file.
        std::memset(statBuf, 0, 128);
        setReturnS32(ctx, 0);
    }

    void write(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioWrite(rdram, ctx, runtime);
    }

    void cvFsSetDefDev(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub cvFsSetDefDev");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }
}
