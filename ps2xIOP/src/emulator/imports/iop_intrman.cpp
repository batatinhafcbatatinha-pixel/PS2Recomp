#include "iop_intrman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace ps2x::iop::detail
{
    IopIntrman::IopIntrman(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopIntrman::reset()
    {
        m_handlers.clear();
        m_enabled.clear();
        m_latched.clear();
    }

    bool IopIntrman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const uint32_t a3 = cpu.gpr[7];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 3:
            setV0(0u);
            return true;
        case 4: // RegisterIntrHandler
            m_handlers[static_cast<int>(a0)] = {a2, a3, cpu.gpr[28]};
            setV0(0u);
            return true;
        case 5: // ReleaseIntrHandler
            m_handlers.erase(static_cast<int>(a0));
            m_latched.erase(static_cast<int>(a0));
            setV0(0u);
            return true;
        case 6: // EnableIntr
            m_enabled[static_cast<int>(a0)] = true;
            if (a0 < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() | (1u << a0));
            setV0(0u);
            return true;
        case 7: // DisableIntr
            if (a1 != 0u)
                m_memory.write32(a1, a0);
            if (a0 < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() & ~(1u << a0));
            m_enabled[static_cast<int>(a0)] = false;
            setV0(0u);
            return true;
        case 8: // CpuDisableIntr
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 9: // CpuEnableIntr
            m_memory.setInterruptControl(1u);
            setV0(0u);
            return true;
        case 14:
            setV0(a0 != 0u
                      ? executor.executeGuestFunctionWithBudget(a0, a1, a2, a3, 0u, cpu.gpr[28], 100000u)
                      : 0u);
            return true;
        case 15:
        case 16:
        case 23:
        case 24:
        case 25:
        case 28:
        case 30:
            setV0(0u);
            return true;
        case 17:
            if (a0 != 0u)
                m_memory.write32(a0, m_memory.interruptControl());
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 18:
            m_memory.setInterruptControl(a0 != 0u ? 1u : 0u);
            setV0(0u);
            return true;
        default:
            return false;
        }
    }

    bool IopIntrman::dispatchInterrupt(int irq, IopGuestExecutor &executor) const
    {
        // PS2X_IOP_INTR_TRACE=1: log every delivered, latched or dropped IOP interrupt.
        static const bool trace = std::getenv("PS2X_IOP_INTR_TRACE") != nullptr;
        static const bool noLatch = std::getenv("PS2X_IOP_INTR_NO_LATCH") != nullptr;
        const auto enabled = m_enabled.find(irq);
        const auto handler = m_handlers.find(irq);
        const bool haveHandler = handler != m_handlers.end() && handler->second.function != 0u;
        if (enabled == m_enabled.end() || !enabled->second)
        {
            // A disabled line keeps the request: the handler runs when EnableIntr turns it back on (libsd
            // disables the SPU DMA lines around BlockTrans setup, and a completion in that window was lost).
            if (!noLatch && haveHandler)
                m_latched[irq] = true;
            if (trace)
                std::cerr << "[iop-intr] irq=" << irq << (haveHandler && !noLatch ? " latched (disabled)" : " dropped (disabled)") << std::endl;
            return false;
        }
        if (!haveHandler)
        {
            if (trace)
                std::cerr << "[iop-intr] irq=" << irq << " dropped (no handler)" << std::endl;
            return false;
        }
        if (trace)
            std::cerr << "[iop-intr] irq=" << irq << std::hex << " handler=0x" << handler->second.function << " arg=0x"
                      << handler->second.argument << std::dec << std::endl;

        (void)executor.executeGuestFunctionWithBudget(handler->second.function,
                                                      handler->second.argument,
                                                      0u,
                                                      0u,
                                                      0u,
                                                      handler->second.gp,
                                                      100000u);
        return true;
    }

    bool IopIntrman::hasDeliverableLatched() const noexcept
    {
        for (const auto &[irq, pending] : m_latched)
        {
            const auto enabled = m_enabled.find(irq);
            if (pending && enabled != m_enabled.end() && enabled->second)
                return true;
        }
        return false;
    }

    void IopIntrman::serviceLatched(IopGuestExecutor &executor)
    {
        if (m_latched.empty())
            return;
        std::vector<int> due;
        for (auto it = m_latched.begin(); it != m_latched.end();)
        {
            const auto enabled = m_enabled.find(it->first);
            if (enabled != m_enabled.end() && enabled->second)
            {
                due.push_back(it->first);
                it = m_latched.erase(it);
            }
            else
                ++it;
        }
        for (const int irq : due)
            (void)dispatchInterrupt(irq, executor);
    }
}
