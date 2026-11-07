# Boku2 requests a 1 MiB stack at RAM top. Leave a separate 1 MiB band
# below it for the runtime arena and callback stacks, and report its start
# through EndOfHeap. Generate local copies; never edit the shared runtime.
get_target_property(BOKU2_RUNTIME_SOURCES ps2_runtime SOURCES)
get_target_property(BOKU2_RUNTIME_SOURCE_DIR ps2_runtime SOURCE_DIR)
set(BOKU2_LOCAL_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/boku2-runtime")
file(MAKE_DIRECTORY "${BOKU2_LOCAL_RUNTIME_DIR}")
foreach(relative IN ITEMS "src/lib/ps2_runtime.cpp" "src/lib/Kernel/Syscalls/System.cpp")
    set(original "${BOKU2_RUNTIME_SOURCE_DIR}/${relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" content)
    if(NOT content MATCHES "0x01F00000u")
        message(FATAL_ERROR "Review Boku2 heap adaptation: shared source changed: ${relative}")
    endif()
    string(REPLACE "0x01F00000u" "0x01E00000u" content "${content}")
    # Address errors are otherwise silent (load returns 0): log the first few with the guest pc/ra.
    string(REPLACE "        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);"
"        { static int bokuBadLoads = 0; if (bokuBadLoads++ < 16) std::cerr << \"[boku:bad-load] addr=0x\" << std::hex << vaddr << \" pc=0x\" << ctx->pc << \" ra=0x\" << getRegU32(ctx, 31) << \" a0=0x\" << getRegU32(ctx, 4) << \" s2=0x\" << getRegU32(ctx, 18) << \" sp=0x\" << getRegU32(ctx, 29) << std::dec << std::endl;
          if (bokuBadLoads == 1) { const uint32_t s = getRegU32(ctx, 29) & 0x1FFFFF0u; std::cerr << \"[boku:bad-load] stack\" << std::hex; for (uint32_t i = 0; i < 64; ++i) std::cerr << ' ' << *reinterpret_cast<const uint32_t*>(rdram + ((s + i * 4u) & 0x1FFFFFCu)); std::cerr << std::dec << std::endl; } }
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);" content "${content}")
    get_filename_component(name "${relative}" NAME)
    set(destination "${BOKU2_LOCAL_RUNTIME_DIR}/${name}")
    file(CONFIGURE OUTPUT "${destination}" CONTENT "${content}" @ONLY)
    list(REMOVE_ITEM BOKU2_RUNTIME_SOURCES "${relative}" "${original}")
    list(APPEND BOKU2_RUNTIME_SOURCES "${destination}")
    get_filename_component(include_dir "${original}" DIRECTORY)
    set_source_files_properties("${destination}" TARGET_DIRECTORY ps2_runtime PROPERTIES INCLUDE_DIRECTORIES "${include_dir}")
endforeach()
set_property(TARGET ps2_runtime PROPERTY SOURCES ${BOKU2_RUNTIME_SOURCES})
option(BOKU2_MPEG_DIAGNOSTICS "Enable the optional local MPEG frame dump patch" OFF)
if(BOKU2_MPEG_DIAGNOSTICS)
# BOKU2_MPEG_DUMP=<dir>: write every picture sceMpegGetPicture serves as <dir>/mpeg_<n>_<w>x<h>.ppm (raster RGB) and the
# guest buffer it fills as <dir>/mpeg_<n>_guest.bin, to check decode vs. layout of the backgrounds.
set(original "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/MPEG.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
file(READ "${original}" content)
set(needle "        if (haveFrame)
        {
            writeDecodedFrameToGuest(rdram, imageAddr, frame, byteLimit);
        }")
string(FIND "${content}" "${needle}" found)
if(found LESS 0)
    message(FATAL_ERROR "Review Boku2 MPEG dump: shared MPEG.cpp changed")
endif()
string(REPLACE "${needle}" "        if (haveFrame)
        {
            writeDecodedFrameToGuest(rdram, imageAddr, frame, byteLimit);
            if (const char *dumpDir = std::getenv(\"BOKU2_MPEG_DUMP\"))
            {
                static uint32_t dumped = 0;
                if (dumped < 64u)
                {
                    const std::string stem = std::string(dumpDir) + \"/mpeg_\" + std::to_string(dumped++);
                    if (FILE *ppm = std::fopen((stem + \"_\" + std::to_string(frame.width) + \"x\" + std::to_string(frame.height) + \".ppm\").c_str(), \"wb\"))
                    {
                        std::fprintf(ppm, \"P6\\n%d %d\\n255\\n\", frame.width, frame.height);
                        for (size_t i = 0; i + 3 < frame.rgba.size(); i += 4)
                            std::fwrite(frame.rgba.data() + i, 1, 3, ppm);
                        std::fclose(ppm);
                    }
                    if (FILE *bin = std::fopen((stem + \"_guest.bin\").c_str(), \"wb\"))
                    {
                        if (const uint8_t *guest = getConstMemPtr(rdram, imageAddr))
                            std::fwrite(guest, 1, std::min<size_t>(byteLimit, PS2_RAM_SIZE - (imageAddr & PS2_RAM_MASK)), bin);
                        std::fclose(bin);
                    }
                    std::cerr << \"[boku:mpeg-dump] \" << stem << \" \" << frame.width << \"x\" << frame.height << \" mb=\" << mbCount
                              << \" dest=0x\" << std::hex << imageAddr << std::dec << std::endl;
                }
            }
        }
        else
        {
            static uint32_t noFrameLogs = 0;
            if (noFrameLogs++ < 16u)
                std::cerr << \"[boku:mpeg-noframe] mpeg=0x\" << std::hex << mpegAddr << \" dest=0x\" << imageAddr << std::dec
                          << \" served=\" << frameCount << \" mb=\" << mbCount << std::endl;
        }" content "${content}")
string(REPLACE "        else if (frameCount == 0u)
        {
            writeBlankMpegFrame" "        if (!haveFrame && frameCount == 0u)
        {
            writeBlankMpegFrame" content "${content}")
set(destination "${BOKU2_LOCAL_RUNTIME_DIR}/MPEG.cpp")
file(CONFIGURE OUTPUT "${destination}" CONTENT "${content}" @ONLY)
get_target_property(sources ps2_runtime SOURCES)
list(REMOVE_ITEM sources "src/lib/Kernel/Stubs/MPEG.cpp" "${original}")
list(APPEND sources "${destination}")
set_property(TARGET ps2_runtime PROPERTY SOURCES ${sources})
set_source_files_properties("${destination}" TARGET_DIRECTORY ps2_runtime PROPERTIES
    INCLUDE_DIRECTORIES "${BOKU2_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs")

endif()
