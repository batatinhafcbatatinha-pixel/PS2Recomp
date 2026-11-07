# Bounded instrumentation of the observed boot stall, in a local source copy.
set(original "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel/EeScheduler.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
file(READ "${original}" content)
set(needle "        if (checkpointDue(kGuestDispatchCycles))")
set(replacement [=[        const bool bokuCheckpoint = checkpointDue(kGuestDispatchCycles);
        if (context.pc == 0x2338dcu) {
            static uint64_t bokuSamples = 0;
            ++bokuSamples;
            if ((bokuSamples & (bokuSamples - 1u)) == 0u) {
                std::cerr << "[boku:callback-resume] samples=" << bokuSamples
                          << " checkpoint=" << bokuCheckpoint
                          << " cycle=" << m_eeCycle
                          << " deadline=" << m_nextDeadlineCycle.load()
                          << " reschedule=" << m_rescheduleRequested
                          << " depth=" << running->invocations.size()
                          << std::hex << " sp=0x" << getRegU32(&context, 29)
                          << " ra=0x" << getRegU32(&context, 31)
                          << " a0=0x" << getRegU32(&context, 4)
                          << " a1=0x" << getRegU32(&context, 5)
                          << std::dec << std::endl;
            }
        }
        if (bokuCheckpoint)]=])
string(FIND "${content}" "${needle}" found)
if(found LESS 0)
    message(FATAL_ERROR "Review Boku2 scheduler diagnostic: shared source changed")
endif()
string(REPLACE "${needle}" "${replacement}" content "${content}")
function(boku_patch from to)
    string(FIND "${content}" "${from}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Review Boku2 scheduler patch: shared EeScheduler.cpp changed")
    endif()
    string(REPLACE "${from}" "${to}" patched "${content}")
    set(content "${patched}" PARENT_SCOPE)
endfunction()
# Diagnostic: a due-by-cycle event whose host deadline is far away holds every guest thread back.
boku_patch([=[            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(]=]
           [=[            {
                // pacing statistics every 5 s: waits, total wait, dispatch checkpoints
                static uint64_t waits = 0, waitNs = 0;
                static auto last = now;
                if (now < pacingDeadline) { ++waits; waitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(pacingDeadline - now).count()); }
                if (now - last > std::chrono::seconds(5))
                {
                    std::cerr << "[boku:pacing] waits=" << waits << " wait_ms=" << waitNs / 1000000u << " items=" << m_deadlines.size() << std::endl;
                    waits = 0; waitNs = 0; last = now;
                }
            }
            if (now < pacingDeadline && pacingDeadline - now > std::chrono::milliseconds(200))
            {
                static int bokuLongPacing = 0;
                if (++bokuLongPacing <= 40)
                {
                    std::cerr << "[boku:long-pacing] wait_ms="
                              << std::chrono::duration_cast<std::chrono::milliseconds>(pacingDeadline - now).count()
                              << " cycle=" << m_eeCycle << " items=" << m_deadlines.size();
                    for (const ScheduledEvent &item : m_deadlines)
                        std::cerr << " [type=" << static_cast<int>(item.event.type) << " id=" << item.event.id
                                  << " value=" << item.event.value << " dc=" << (int64_t)(item.deadlineCycle - m_eeCycle)
                                  << " hms=" << std::chrono::duration_cast<std::chrono::milliseconds>(item.hostDeadline - now).count() << "]";
                    std::cerr << std::endl;
                }
            }
            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(]=])
set(destination "${BOKU2_LOCAL_RUNTIME_DIR}/EeScheduler.cpp")
file(CONFIGURE OUTPUT "${destination}" CONTENT "${content}" @ONLY)
get_target_property(sources ps2_runtime SOURCES)
list(REMOVE_ITEM sources "src/lib/Kernel/EeScheduler.cpp" "${original}")
list(APPEND sources "${destination}")
set_property(TARGET ps2_runtime PROPERTY SOURCES ${sources})
set_source_files_properties("${destination}" TARGET_DIRECTORY ps2_runtime PROPERTIES
    INCLUDE_DIRECTORIES "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel")
# PS2X_TRACE_PRIORITY: trace every change, not only the first 400 (the boot stall comes later).
set(original "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
file(READ "${original}" content)
string(FIND "${content}" "traced++ < 400u" found)
if(found LESS 0)
    message(FATAL_ERROR "Review Boku2 priority trace: shared source changed")
endif()
string(REPLACE "traced++ < 400u" "traced++ < 4000000u" content "${content}")
set(destination "${BOKU2_LOCAL_RUNTIME_DIR}/Thread.cpp")
file(CONFIGURE OUTPUT "${destination}" CONTENT "${content}" @ONLY)
get_target_property(sources ps2_runtime SOURCES)
list(REMOVE_ITEM sources "src/lib/Kernel/Syscalls/Thread.cpp" "${original}")
list(APPEND sources "${destination}")
set_property(TARGET ps2_runtime PROPERTY SOURCES ${sources})
set_source_files_properties("${destination}" TARGET_DIRECTORY ps2_runtime PROPERTIES
    INCLUDE_DIRECTORIES "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls")
