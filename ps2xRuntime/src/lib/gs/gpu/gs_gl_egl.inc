// Native Linux GS context. Presentation uses the existing RAM readback path:
// the frontend may own a GLX context, which cannot share objects with EGL.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

namespace {
struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
};
constexpr EGLint contextAttributes[] = {
    EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6,
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
constexpr EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};

void destroyEgl(EglState* state, bool terminate) {
    if (!state) return;
    if (state->display != EGL_NO_DISPLAY) {
        if (state->context != EGL_NO_CONTEXT && eglGetCurrentContext() == state->context)
            eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state->context != EGL_NO_CONTEXT) eglDestroyContext(state->display, state->context);
        if (state->surface != EGL_NO_SURFACE) eglDestroySurface(state->display, state->surface);
        // EGL displays are process-wide and may also be used by the frontend.
        // Do not terminate a display here: that would invalidate its contexts.
        (void)terminate;
    }
    delete state;
}
template <typename T>
bool load(T& target, const char* name, std::string& missing) {
    target = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!target) {
        static void* lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
        if (lib) target = reinterpret_cast<T>(dlsym(lib, name));
    }
    if (!target) {
        if (!missing.empty()) missing += ", ";
        missing += name;
    }
    return target != nullptr;
}
}
namespace GSGL {
bool PublishShared(const Api&, GLuint, uint32_t, uint32_t, uint64_t,
                   uint32_t, uint32_t, bool, bool, uint64_t, GLuint, GLint) { return false; }
bool SharedAvailable(const Api&) { return false; }
bool PublishSharedTexture(const Api&, GLuint, uint32_t, uint32_t, uint64_t,
                          uint32_t, uint32_t, uint64_t) { return false; }
void DestroyShared(const Api&) {}
Context::~Context() { Destroy(); }
bool Context::Create(std::string& error) {
    Destroy();
    auto* state = new EglState;
    m_window = state;
    // EGL_DEFAULT_DISPLAY supports the user's display; Mesa surfaceless also
    // permits the GS worker to run independently of the frontend window.
    state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (state->display == EGL_NO_DISPLAY || !eglInitialize(state->display, nullptr, nullptr)) {
        auto platformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        state->display = platformDisplay
            ? platformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : EGL_NO_DISPLAY;
        if (state->display == EGL_NO_DISPLAY || !eglInitialize(state->display, nullptr, nullptr)) {
            error = "Unable to initialize an EGL display";
            Destroy();
            return false;
        }
    }
    const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    if (!eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(state->display, configAttributes, &state->config, 1, &count) || !count) {
        error = "EGL desktop OpenGL configuration unavailable";
        Destroy();
        return false;
    }
    state->surface = eglCreatePbufferSurface(state->display, state->config, surfaceAttributes);
    state->context = eglCreateContext(state->display, state->config, EGL_NO_CONTEXT, contextAttributes);
    if (state->surface == EGL_NO_SURFACE || state->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(state->display, state->surface, state->surface, state->context)) {
        error = "EGL OpenGL 4.6 core context unavailable";
        Destroy();
        return false;
    }
    m_context = state->context;
    std::string missing;
    Api& a = m_api;
#include "gs_gl_load.inc"
    if (!missing.empty()) {
        error = "Missing GL functions: " + missing;
        Destroy();
        return false;
    }
    return true;
}
void* Context::CreateWorker() const {
    auto* owner = static_cast<EglState*>(m_window);
    if (!owner || !m_context) return nullptr;
    auto* worker = new EglState;
    worker->display = owner->display;
    worker->config = owner->config;
    if (!eglBindAPI(EGL_OPENGL_API)) { delete worker; return nullptr; }
    worker->surface = eglCreatePbufferSurface(worker->display, worker->config, surfaceAttributes);
    worker->context = eglCreateContext(worker->display, worker->config, owner->context, contextAttributes);
    if (worker->surface == EGL_NO_SURFACE || worker->context == EGL_NO_CONTEXT) {
        destroyEgl(worker, false);
        return nullptr;
    }
    return worker;
}
bool Context::MakeWorkerCurrent(void* handle) const {
    auto* worker = static_cast<EglState*>(handle);
    return worker && eglBindAPI(EGL_OPENGL_API) &&
        eglMakeCurrent(worker->display, worker->surface, worker->surface, worker->context);
}
void Context::DestroyWorker(void* handle) { destroyEgl(static_cast<EglState*>(handle), false); }
void Context::Destroy() {
    destroyEgl(static_cast<EglState*>(m_window), true);
    m_window = m_dc = m_context = nullptr;
    m_api = {};
    m_shared = false;
}
}
namespace GSSharedPresent {
void CaptureHostContext() {}
void RestoreHostContext() {}
bool Active() { return false; }
bool Acquire(Frame&) { return false; }
void ShutdownHost() {}
}
