#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include "ps2x/iop/iop_host.h"

#include <cstdio>
#include <algorithm>
#include <vector>

namespace ps2x::iop::detail
{
    IopIoman::IopIoman(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        for (const auto &[fd, file] : m_files)
            m_host.closeHostFile(file.handle);
        m_files.clear();
        m_nextFile = 3;
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // open(name, flags, mode): read-only access through the host VFS
        {
            const std::string name = m_memory.readString(a0, 256u);
            if ((cpu.gpr[5] & 3u) != 1u) // only O_RDONLY
            {
                m_host.log(LogLevel::Warning, "[IOP ioman] write open refused: " + name);
                setV0(static_cast<uint32_t>(-5));
                return true;
            }
            const uint64_t handle = m_host.openHostFile(m_host.translateGuestPath(name));
            uint64_t size = 0u;
            if (handle == 0u || !m_host.hostFileSize(handle, size))
            {
                if (handle != 0u)
                    m_host.closeHostFile(handle);
                {
                    char where[64];
                    std::snprintf(where, sizeof where, " (name@0x%X ra=0x%X)", a0, cpu.gpr[31]);
                    m_host.log(LogLevel::Warning, "[IOP ioman] open failed: " + name + where);
                }
                setV0(static_cast<uint32_t>(-2)); // ENOENT
                return true;
            }
            const int32_t fd = m_nextFile++;
            m_files[fd] = File{handle, size, 0u};
            m_host.log(LogLevel::Info, "[IOP ioman] open " + name + " fd=" + std::to_string(fd) + " size=" + std::to_string(size));
            setV0(static_cast<uint32_t>(fd));
            return true;
        }
        case 5: // close(fd)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(static_cast<uint32_t>(-9)); // EBADF
                return true;
            }
            m_host.closeHostFile(file->second.handle);
            m_files.erase(file);
            setV0(0u);
            return true;
        }
        case 6: // read(fd, buffer, size)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(static_cast<uint32_t>(-9));
                return true;
            }
            File &state = file->second;
            const uint64_t available = state.position < state.size ? state.size - state.position : 0u;
            const uint32_t wanted = static_cast<uint32_t>(std::min<uint64_t>(cpu.gpr[6], available));
            std::vector<uint8_t> data(wanted);
            size_t bytesRead = 0u;
            if (wanted != 0u && !m_host.readHostFile(state.handle, state.position, data.data(), wanted, bytesRead))
                bytesRead = 0u;
            if (bytesRead != 0u)
                (void)m_memory.writeRam(cpu.gpr[5], data.data(), bytesRead);
            state.position += bytesRead;
            setV0(static_cast<uint32_t>(bytesRead));
            return true;
        }
        case 7: // write(fd, buffer, size): files are read-only
            setV0(static_cast<uint32_t>(-9));
            return true;
        case 8: // lseek(fd, offset, whence)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(static_cast<uint32_t>(-9));
                return true;
            }
            File &state = file->second;
            const int64_t offset = static_cast<int32_t>(cpu.gpr[5]);
            const int64_t base = cpu.gpr[6] == 0u ? 0 : cpu.gpr[6] == 1u ? static_cast<int64_t>(state.position)
                                                                         : static_cast<int64_t>(state.size);
            const int64_t target = base + offset;
            if (cpu.gpr[6] > 2u || target < 0)
            {
                setV0(static_cast<uint32_t>(-22)); // EINVAL
                return true;
            }
            state.position = static_cast<uint64_t>(target);
            setV0(static_cast<uint32_t>(target));
            return true;
        }
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }
}
