#include "HookGl.h"

#include <d3d11.h>

#include <cstdio>

namespace rf::hook::gl {
namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLchar = char;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;
constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLint GL_LINEAR = 0x2601;
constexpr GLint GL_CLAMP_TO_EDGE = 0x812F;

constexpr GLenum WGL_ACCESS_READ_ONLY_NV = 0x00000000;

using PFN_glGetIntegerv = void(WINAPI*)(GLenum, GLint*);
using PFN_glEnable = void(WINAPI*)(GLenum);
using PFN_glDisable = void(WINAPI*)(GLenum);
using PFN_glIsEnabled = GLboolean(WINAPI*)(GLenum);
using PFN_glViewport = void(WINAPI*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glBindTexture = void(WINAPI*)(GLenum, GLuint);
using PFN_glGenTextures = void(WINAPI*)(GLsizei, GLuint*);
using PFN_glDeleteTextures = void(WINAPI*)(GLsizei, const GLuint*);
using PFN_glDrawArrays = void(WINAPI*)(GLenum, GLint, GLsizei);
using PFN_glGetError = GLenum(WINAPI*)();
using PFN_glTexParameteri = void(WINAPI*)(GLenum, GLenum, GLint);

struct Gl {
    PFN_glGetIntegerv GetIntegerv = nullptr;
    PFN_glEnable Enable = nullptr;
    PFN_glDisable Disable = nullptr;
    PFN_glIsEnabled IsEnabled = nullptr;
    PFN_glViewport Viewport = nullptr;
    PFN_glBindTexture BindTexture = nullptr;
    PFN_glGenTextures GenTextures = nullptr;
    PFN_glDeleteTextures DeleteTextures = nullptr;
    PFN_glDrawArrays DrawArrays = nullptr;
    PFN_glGetError GetError = nullptr;
    PFN_glTexParameteri TexParameteri = nullptr;

    void(WINAPI* ActiveTexture)(GLenum) = nullptr;
    void(WINAPI* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
    void(WINAPI* BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLuint(WINAPI* CreateShader)(GLenum) = nullptr;
    void(WINAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void(WINAPI* CompileShader)(GLuint) = nullptr;
    void(WINAPI* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void(WINAPI* DeleteShader)(GLuint) = nullptr;
    GLuint(WINAPI* CreateProgram)() = nullptr;
    void(WINAPI* AttachShader)(GLuint, GLuint) = nullptr;
    void(WINAPI* LinkProgram)(GLuint) = nullptr;
    void(WINAPI* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void(WINAPI* UseProgram)(GLuint) = nullptr;
    void(WINAPI* DeleteProgram)(GLuint) = nullptr;
    GLint(WINAPI* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void(WINAPI* Uniform1i)(GLint, GLint) = nullptr;
    void(WINAPI* GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void(WINAPI* BindVertexArray)(GLuint) = nullptr;
    void(WINAPI* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;

    HANDLE(WINAPI* DXOpenDeviceNV)(void*) = nullptr;
    BOOL(WINAPI* DXCloseDeviceNV)(HANDLE) = nullptr;
    HANDLE(WINAPI* DXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum) = nullptr;
    BOOL(WINAPI* DXUnregisterObjectNV)(HANDLE, HANDLE) = nullptr;
    BOOL(WINAPI* DXLockObjectsNV)(HANDLE, GLint, HANDLE*) = nullptr;
    BOOL(WINAPI* DXUnlockObjectsNV)(HANDLE, GLint, HANDLE*) = nullptr;
    BOOL(WINAPI* DXSetResourceShareHandleNV)(void*, HANDLE) = nullptr;
} g_gl;

HMODULE g_opengl = nullptr;
bool g_resolved = false;
bool g_unavailable = false;

ID3D11Device* g_device = nullptr;
ID3D11Texture2D* g_texture = nullptr;
HANDLE g_interop_device = nullptr;
HANDLE g_interop_object = nullptr;
GLuint g_gl_texture = 0;
GLuint g_program = 0;
GLuint g_vao = 0;
std::uint32_t g_serial = 0;

void Log(const char* fmt, ...) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, _TRUNCATE, L"%s\\Reframe\\logs\\hook.log", dir) < 0) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"at") != 0 || !f) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    const int len = _vsnprintf_s(line, _TRUNCATE, fmt, args);
    va_end(args);
    if (len > 0) fprintf(f, "[%lu] gl: %s\n", GetCurrentProcessId(), line);
    fclose(f);
}

template <typename T>
bool LoadWgl(T& fn, const char* name) {
    using Proc = PROC(WINAPI*)(LPCSTR);
    static Proc get_proc = nullptr;
    if (!get_proc)
        get_proc = reinterpret_cast<Proc>(GetProcAddress(g_opengl, "wglGetProcAddress"));
    if (!get_proc) return false;
    fn = reinterpret_cast<T>(get_proc(name));
    return fn != nullptr;
}

template <typename T>
bool LoadCore(T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(g_opengl, name));
    return fn != nullptr;
}

bool Resolve() {
    if (g_resolved) return !g_unavailable;
    g_resolved = true;

    g_opengl = GetModuleHandleW(L"opengl32.dll");
    if (!g_opengl) {
        g_unavailable = true;
        return false;
    }

    const bool core = LoadCore(g_gl.GetIntegerv, "glGetIntegerv") &&
                      LoadCore(g_gl.Enable, "glEnable") && LoadCore(g_gl.Disable, "glDisable") &&
                      LoadCore(g_gl.IsEnabled, "glIsEnabled") &&
                      LoadCore(g_gl.Viewport, "glViewport") &&
                      LoadCore(g_gl.BindTexture, "glBindTexture") &&
                      LoadCore(g_gl.GenTextures, "glGenTextures") &&
                      LoadCore(g_gl.DeleteTextures, "glDeleteTextures") &&
                      LoadCore(g_gl.DrawArrays, "glDrawArrays") &&
                      LoadCore(g_gl.GetError, "glGetError") &&
                      LoadCore(g_gl.TexParameteri, "glTexParameteri");

    const bool modern =
        LoadWgl(g_gl.ActiveTexture, "glActiveTexture") &&
        LoadWgl(g_gl.BlendFuncSeparate, "glBlendFuncSeparate") &&
        LoadWgl(g_gl.BindFramebuffer, "glBindFramebuffer") &&
        LoadWgl(g_gl.CreateShader, "glCreateShader") &&
        LoadWgl(g_gl.ShaderSource, "glShaderSource") &&
        LoadWgl(g_gl.CompileShader, "glCompileShader") &&
        LoadWgl(g_gl.GetShaderiv, "glGetShaderiv") && LoadWgl(g_gl.DeleteShader, "glDeleteShader") &&
        LoadWgl(g_gl.GetShaderInfoLog, "glGetShaderInfoLog") &&
        LoadWgl(g_gl.CreateProgram, "glCreateProgram") &&
        LoadWgl(g_gl.AttachShader, "glAttachShader") &&
        LoadWgl(g_gl.LinkProgram, "glLinkProgram") &&
        LoadWgl(g_gl.GetProgramiv, "glGetProgramiv") && LoadWgl(g_gl.UseProgram, "glUseProgram") &&
        LoadWgl(g_gl.DeleteProgram, "glDeleteProgram") &&
        LoadWgl(g_gl.GetUniformLocation, "glGetUniformLocation") &&
        LoadWgl(g_gl.Uniform1i, "glUniform1i") &&
        LoadWgl(g_gl.GenVertexArrays, "glGenVertexArrays") &&
        LoadWgl(g_gl.BindVertexArray, "glBindVertexArray") &&
        LoadWgl(g_gl.DeleteVertexArrays, "glDeleteVertexArrays");

    const bool interop = LoadWgl(g_gl.DXOpenDeviceNV, "wglDXOpenDeviceNV") &&
                         LoadWgl(g_gl.DXCloseDeviceNV, "wglDXCloseDeviceNV") &&
                         LoadWgl(g_gl.DXRegisterObjectNV, "wglDXRegisterObjectNV") &&
                         LoadWgl(g_gl.DXUnregisterObjectNV, "wglDXUnregisterObjectNV") &&
                         LoadWgl(g_gl.DXLockObjectsNV, "wglDXLockObjectsNV") &&
                         LoadWgl(g_gl.DXUnlockObjectsNV, "wglDXUnlockObjectsNV") &&
                         LoadWgl(g_gl.DXSetResourceShareHandleNV, "wglDXSetResourceShareHandleNV");

    if (const auto get_string = reinterpret_cast<const char*(WINAPI*)(GLenum)>(
            GetProcAddress(g_opengl, "glGetString"))) {
        constexpr GLenum kVersion = 0x1F02, kRenderer = 0x1F01, kGlsl = 0x8B8C;
        const char* version = get_string(kVersion);
        const char* renderer = get_string(kRenderer);
        const char* glsl = get_string(kGlsl);
        Log("context: %s | %s | GLSL %s", version ? version : "?", renderer ? renderer : "?",
            glsl ? glsl : "?");
    }

    if (!core || !modern || !interop) {
        Log("unsupported context (core=%d modern=%d interop=%d)", core, modern, interop);
        g_unavailable = true;
        return false;
    }
    return true;
}

const char* kVertexSrc = R"(#version 150 core
out vec2 vUv;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

const char* kFragmentSrc = R"(#version 150 core
in vec2 vUv;
out vec4 colour;
uniform sampler2D uOverlay;
void main() {
    colour = texture(uOverlay, vUv);
}
)";

GLuint Compile(GLenum stage, const char* source) {
    const GLuint shader = g_gl.CreateShader(stage);
    g_gl.ShaderSource(shader, 1, &source, nullptr);
    g_gl.CompileShader(shader);
    GLint ok = 0;
    g_gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512]{};
        if (g_gl.GetShaderInfoLog) g_gl.GetShaderInfoLog(shader, sizeof(info) - 1, nullptr, info);
        Log("shader stage 0x%x did not compile: %s", stage, info);
        g_gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool EnsureProgram() {
    if (g_program) return true;

    const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexSrc);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs) return false;

    g_program = g_gl.CreateProgram();
    g_gl.AttachShader(g_program, vs);
    g_gl.AttachShader(g_program, fs);
    g_gl.LinkProgram(g_program);
    g_gl.DeleteShader(vs);
    g_gl.DeleteShader(fs);

    GLint ok = 0;
    g_gl.GetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        Log("overlay program did not link");
        g_gl.DeleteProgram(g_program);
        g_program = 0;
        return false;
    }

    g_gl.GenVertexArrays(1, &g_vao);
    return g_vao != 0;
}

void ReleaseSurface() {
    if (g_interop_object) {
        g_gl.DXUnlockObjectsNV(g_interop_device, 1, &g_interop_object);
        g_gl.DXUnregisterObjectNV(g_interop_device, g_interop_object);
        g_interop_object = nullptr;
    }
    if (g_gl_texture) {
        g_gl.DeleteTextures(1, &g_gl_texture);
        g_gl_texture = 0;
    }
    if (g_texture) {
        g_texture->Release();
        g_texture = nullptr;
    }
    g_serial = 0;
}

bool EnsureSurface(std::uint32_t shared_handle, std::uint32_t serial) {
    if (g_interop_object && g_serial == serial) return true;
    ReleaseSurface();
    if (!shared_handle) return false;

    if (!g_device) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                     ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_device, nullptr,
                                     nullptr))) {
            Log("could not create the interop D3D11 device");
            return false;
        }
    }
    if (!g_interop_device) {
        g_interop_device = g_gl.DXOpenDeviceNV(g_device);
        if (!g_interop_device) {
            Log("wglDXOpenDeviceNV failed");
            return false;
        }
    }

    const HANDLE share = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(shared_handle));
    if (FAILED(g_device->OpenSharedResource(share, __uuidof(ID3D11Texture2D),
                                            (void**)&g_texture))) {
        Log("could not open the overlay texture");
        return false;
    }

    g_gl.GenTextures(1, &g_gl_texture);

    g_gl.DXSetResourceShareHandleNV(g_texture, share);
    g_interop_object = g_gl.DXRegisterObjectNV(g_interop_device, g_texture, g_gl_texture,
                                               GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!g_interop_object) {
        Log("wglDXRegisterObjectNV failed");
        ReleaseSurface();
        return false;
    }

    g_serial = serial;
    Log("overlay surface registered with the GL context");
    return true;
}

}

void DrawOverlay(HDC hdc, std::uint32_t shared_handle, std::uint32_t serial, unsigned width,
                 unsigned height) {
    static int reported = -1;
    auto report = [&](int stage) {
        if (reported != stage) {
            reported = stage;
            Log("draw stopped at stage %d", stage);
        }
    };

    if (!Resolve()) return report(1);
    if (!EnsureProgram()) return report(2);
    if (!EnsureSurface(shared_handle, serial)) return report(3);
    if (!g_gl.DXLockObjectsNV(g_interop_device, 1, &g_interop_object)) return report(4);
    report(0);

    GLint saved_program = 0, saved_vao = 0, saved_texture = 0, saved_unit = 0, saved_fbo = 0;
    GLint saved_viewport[4] = {0, 0, 0, 0};
    GLint saved_src_rgb = 0, saved_dst_rgb = 0, saved_src_alpha = 0, saved_dst_alpha = 0;
    g_gl.GetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    g_gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    g_gl.GetIntegerv(GL_ACTIVE_TEXTURE, &saved_unit);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
    g_gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_fbo);
    g_gl.GetIntegerv(GL_VIEWPORT, saved_viewport);
    g_gl.GetIntegerv(GL_BLEND_SRC_RGB, &saved_src_rgb);
    g_gl.GetIntegerv(GL_BLEND_DST_RGB, &saved_dst_rgb);
    g_gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &saved_src_alpha);
    g_gl.GetIntegerv(GL_BLEND_DST_ALPHA, &saved_dst_alpha);
    const GLboolean had_blend = g_gl.IsEnabled(GL_BLEND);
    const GLboolean had_depth = g_gl.IsEnabled(GL_DEPTH_TEST);
    const GLboolean had_cull = g_gl.IsEnabled(GL_CULL_FACE);
    const GLboolean had_scissor = g_gl.IsEnabled(GL_SCISSOR_TEST);

    RECT client{};
    if (HWND wnd = WindowFromDC(hdc); wnd && GetClientRect(wnd, &client)) {
        width = static_cast<unsigned>(client.right - client.left);
        height = static_cast<unsigned>(client.bottom - client.top);
    }

    g_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g_gl.Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    g_gl.UseProgram(g_program);
    g_gl.BindVertexArray(g_vao);
    g_gl.ActiveTexture(GL_TEXTURE0);
    g_gl.BindTexture(GL_TEXTURE_2D, g_gl_texture);

    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gl.Uniform1i(g_gl.GetUniformLocation(g_program, "uOverlay"), 0);
    g_gl.DrawArrays(GL_TRIANGLES, 0, 3);

    g_gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_texture));
    g_gl.ActiveTexture(static_cast<GLenum>(saved_unit));
    g_gl.BindVertexArray(static_cast<GLuint>(saved_vao));
    g_gl.UseProgram(static_cast<GLuint>(saved_program));
    g_gl.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(saved_fbo));
    g_gl.Viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    g_gl.BlendFuncSeparate(static_cast<GLenum>(saved_src_rgb), static_cast<GLenum>(saved_dst_rgb),
                           static_cast<GLenum>(saved_src_alpha),
                           static_cast<GLenum>(saved_dst_alpha));
    if (!had_blend) g_gl.Disable(GL_BLEND);
    if (had_depth) g_gl.Enable(GL_DEPTH_TEST);
    if (had_cull) g_gl.Enable(GL_CULL_FACE);
    if (had_scissor) g_gl.Enable(GL_SCISSOR_TEST);

    if (const GLenum err = g_gl.GetError(); err != 0) {
        static GLenum last = 0;
        if (err != last) {
            last = err;
            Log("GL error 0x%x after the overlay draw", err);
        }
    }

    g_gl.DXUnlockObjectsNV(g_interop_device, 1, &g_interop_object);
}

void Release() {
    if (!g_resolved || g_unavailable) return;
    ReleaseSurface();
    if (g_vao) {
        g_gl.DeleteVertexArrays(1, &g_vao);
        g_vao = 0;
    }
    if (g_program) {
        g_gl.DeleteProgram(g_program);
        g_program = 0;
    }
    if (g_interop_device) {
        g_gl.DXCloseDeviceNV(g_interop_device);
        g_interop_device = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

}
