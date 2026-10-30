#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

// GS rasterizer on the host GPU (OpenGL 4.6 + fragment shader interlock).
// Local memory is kept in a GPU buffer with the real PS2 swizzle; draws, uploads,
// local copies and CLUT loads run there in submission order on a dedicated GS
// thread. Presentation and other CPU-side reads use a read-back copy in the
// external 4 MiB local-memory allocation.
class GSGpuBackend final : public GSRasterBackend
{
public:
    struct NativePresentation {
        uint32_t texture = 0, width = 0, height = 0;
        uint32_t storageWidth = 640, storageHeight = 512;
    };
    // Main-window thread only. Prepare completes preceding consumer draws
    // before the GS thread may replace the single shared scanout texture.
    // Get returns a borrowed texture; callers must not unload/delete it.
    static bool PrepareNativePresentation();
    static bool GetNativePresentation(NativePresentation &out);
    // Must be called on the thread that owns the main window (GLFW requirement).
    // Returns nullptr, with a reason on stderr, when the GPU path is unavailable.
    static std::unique_ptr<GSRasterBackend> Create(bool requireHardware = false);
    ~GSGpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    struct Impl;

private:
    GSGpuBackend();
    std::unique_ptr<Impl> m_impl;
};
