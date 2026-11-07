// GTA: Liberty City Stories: what is still specific to this game in the generic pipeline (tools/ps2port, port.toml).
// Included by tools/ps2port/project/main.cpp after the table helpers (PortSlot).
//
// 0x2B6B20 ends a movie: once the PSS is demuxed it writes a sequence_end_code into the game's video ES ring and sets
// the player state to 2. The native MPEG decoder never reads that ring, so tell it here that the stream is over;
// without this sceMpegIsEnd never turns true and the game waits on the Rockstar logo forever.
// route: {0x2b6b20u, LcsMovieDataEnd}
namespace ps2_stubs { void notifyMpegCdStreamEof(PS2Runtime* runtime); }

static PS2Runtime::RecompiledFunction g_lcsMovieEndOriginal = nullptr;

static void LcsMovieDataEnd(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_lcsMovieEndOriginal(rdram, ctx, runtime);
    if (getRegU32(ctx, 2) != 0) ps2_stubs::notifyMpegCdStreamEof(runtime);
}

static void PortProfileInstall() {
    if (auto* slot = PortSlot(0x2b6b20u); slot && *slot) {
        g_lcsMovieEndOriginal = *slot;
        *slot = LcsMovieDataEnd;
    }
}
