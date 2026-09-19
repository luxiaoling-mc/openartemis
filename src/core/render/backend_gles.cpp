#include "core/render/backend_gles.h"

#include <SDL3/SDL_video.h> // SDL_Window / SDL_GL_*（只含视频面，无 SDL_Render）

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// 原生 OpenGL ES 后端：GLES 全部原语手写（无 SDL_Render）。
// 渲染数学对照本机 sdl 线实际生效的 SDL3 "opengl" 渲染驱动逐项复刻，目标 =
// 同一批真实旅程上 GLES↔sdl 帧逐字节一致（或 ≤ 已知取整级差的量化差异）。
// GL 入口点经 SDL_GL_GetProcAddress 加载（本文件内自声明类型/常量，无
// <GLES3/gl3.h> 依赖）；只使用 GLES 3.0 核心函数。

// ---------------------------------------------------------------------------
// GLES 3.0 最小 API 面（类型 + 常量 + 函数指针表，create() 时加载）
// ---------------------------------------------------------------------------
namespace {

typedef unsigned int GLenum_;
typedef unsigned int GLuint_;
typedef int GLint_;
typedef int GLsizei_;
typedef intptr_t GLsizeiptr_;
typedef unsigned char GLubyte_;
typedef char GLchar_;
typedef float GLfloat_;
typedef unsigned char GLboolean_;
typedef unsigned int GLbitfield_;

// 只用到的 GL 常量（GLES3 官方数值）
enum : unsigned int {
    GL_FALSE_ = 0, GL_TRUE_ = 1, GL_NO_ERROR_ = 0,
    GL_ZERO_ = 0, GL_ONE_ = 1,
    GL_SRC_COLOR_ = 0x0300, GL_SRC_ALPHA_ = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA_ = 0x0303, GL_DST_ALPHA_ = 0x0304,
    GL_BLEND_ = 0x0BE2, GL_SCISSOR_TEST_ = 0x0C11, GL_DEPTH_TEST_ = 0x0B71,
    GL_CULL_FACE_ = 0x0B44,
    GL_FUNC_ADD_ = 0x8006,
    GL_ARRAY_BUFFER_ = 0x8892, GL_DYNAMIC_DRAW_ = 0x88E8,
    GL_FRAGMENT_SHADER_ = 0x8B30, GL_VERTEX_SHADER_ = 0x8B31,
    GL_COMPILE_STATUS_ = 0x8B81, GL_LINK_STATUS_ = 0x8B82,
    GL_INFO_LOG_LENGTH_ = 0x8B84,
    GL_TEXTURE0_ = 0x84C0, GL_TEXTURE1_ = 0x84C1,
    GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803, GL_CLAMP_TO_EDGE_ = 0x812F,
    GL_LINEAR_ = 0x2601,
    GL_UNPACK_ALIGNMENT_ = 0x0CF5, GL_UNPACK_ROW_LENGTH_ = 0x0CF2,
    GL_PACK_ALIGNMENT_ = 0x0D05,
    GL_RGBA_ = 0x1908, GL_RGBA8_ = 0x8058, GL_UNSIGNED_BYTE_ = 0x1401,
    GL_FLOAT_ = 0x1406, GL_TRIANGLES_ = 0x0004,
    GL_COLOR_BUFFER_BIT_ = 0x00004000,
    GL_FRAMEBUFFER_ = 0x8D40, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5,
    GL_VERSION_ = 0x1F02, GL_RENDERER_ = 0x1F01,
};

struct GlProcs {
#define OA_GLP(type, name, params) type (*name) params = nullptr;
    OA_GLP(void, ActiveTexture, (GLenum_))
    OA_GLP(void, AttachShader, (GLuint_, GLuint_))
    OA_GLP(void, BindBuffer, (GLenum_, GLuint_))
    OA_GLP(void, BindFramebuffer, (GLenum_, GLuint_))
    OA_GLP(void, BindTexture, (GLenum_, GLuint_))
    OA_GLP(void, BindVertexArray, (GLuint_))
    OA_GLP(void, BlendEquation, (GLenum_))
    OA_GLP(void, BlendFuncSeparate, (GLenum_, GLenum_, GLenum_, GLenum_))
    OA_GLP(void, BufferData, (GLenum_, GLsizeiptr_, const void*, GLenum_))
    OA_GLP(void, BufferSubData, (GLenum_, GLsizeiptr_, GLsizeiptr_, const void*))
    OA_GLP(GLenum_, CheckFramebufferStatus, (GLenum_))
    OA_GLP(void, Clear, (GLbitfield_))
    OA_GLP(void, ClearColor, (GLfloat_, GLfloat_, GLfloat_, GLfloat_))
    OA_GLP(void, CompileShader, (GLuint_))
    OA_GLP(GLuint_, CreateProgram, ())
    OA_GLP(GLuint_, CreateShader, (GLenum_))
    OA_GLP(void, DeleteBuffers, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteFramebuffers, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteProgram, (GLuint_))
    OA_GLP(void, DeleteShader, (GLuint_))
    OA_GLP(void, DeleteTextures, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteVertexArrays, (GLsizei_, const GLuint_*))
    OA_GLP(void, Disable, (GLenum_))
    OA_GLP(void, DisableVertexAttribArray, (GLuint_))
    OA_GLP(void, DrawArrays, (GLenum_, GLint_, GLsizei_))
    OA_GLP(void, Enable, (GLenum_))
    OA_GLP(void, EnableVertexAttribArray, (GLuint_))
    OA_GLP(void, FramebufferTexture2D,
           (GLenum_, GLenum_, GLenum_, GLuint_, GLint_))
    OA_GLP(void, GenBuffers, (GLsizei_, GLuint_*))
    OA_GLP(void, GenFramebuffers, (GLsizei_, GLuint_*))
    OA_GLP(void, GenTextures, (GLsizei_, GLuint_*))
    OA_GLP(void, GenVertexArrays, (GLsizei_, GLuint_*))
    OA_GLP(GLenum_, GetError, ())
    OA_GLP(void, GetIntegerv, (GLenum_, GLint_*))
    OA_GLP(const GLubyte_*, GetString, (GLenum_))
    OA_GLP(void, GetProgramiv, (GLuint_, GLenum_, GLint_*))
    OA_GLP(void, GetShaderiv, (GLuint_, GLenum_, GLint_*))
    OA_GLP(void, GetShaderInfoLog, (GLuint_, GLsizei_, GLsizei_*, GLchar_*))
    OA_GLP(void, GetProgramInfoLog, (GLuint_, GLsizei_, GLsizei_*, GLchar_*))
    OA_GLP(GLint_, GetUniformLocation, (GLuint_, const GLchar_*))
    OA_GLP(void, LinkProgram, (GLuint_))
    OA_GLP(void, PixelStorei, (GLenum_, GLint_))
    OA_GLP(void, ReadPixels, (GLint_, GLint_, GLsizei_, GLsizei_, GLenum_,
                              GLenum_, void*))
    OA_GLP(void, Scissor, (GLint_, GLint_, GLsizei_, GLsizei_))
    OA_GLP(void, ShaderSource, (GLuint_, GLsizei_, const GLchar_* const*,
                                const GLint_*))
    OA_GLP(void, TexImage2D, (GLenum_, GLint_, GLint_, GLsizei_, GLsizei_,
                              GLint_, GLenum_, GLenum_, const void*))
    OA_GLP(void, TexParameteri, (GLenum_, GLenum_, GLint_))
    OA_GLP(void, TexSubImage2D, (GLenum_, GLint_, GLint_, GLint_, GLsizei_,
                                 GLsizei_, GLenum_, GLenum_, const void*))
    OA_GLP(void, Uniform1i, (GLint_, GLint_))
    OA_GLP(void, Uniform1f, (GLint_, GLfloat_))
    OA_GLP(void, Uniform2f, (GLint_, GLfloat_, GLfloat_))
    OA_GLP(void, UseProgram, (GLuint_))
    OA_GLP(void, VertexAttribPointer,
           (GLuint_, GLint_, GLenum_, GLboolean_, GLsizei_, const void*))
    OA_GLP(void, Viewport, (GLint_, GLint_, GLsizei_, GLsizei_))
#undef OA_GLP
    /// 加载全部入口点；失败返回 false 并写明缺哪个。
    bool load(const char** missing) {
#define OA_GLL(name)                                                        \
    name = (decltype(name))SDL_GL_GetProcAddress("gl" #name);               \
    if (!name) { *missing = #name; return false; }
        OA_GLL(ActiveTexture) OA_GLL(AttachShader) OA_GLL(BindBuffer)
        OA_GLL(BindFramebuffer) OA_GLL(BindTexture) OA_GLL(BindVertexArray)
        OA_GLL(BlendEquation) OA_GLL(BlendFuncSeparate) OA_GLL(BufferData)
        OA_GLL(BufferSubData) OA_GLL(CheckFramebufferStatus) OA_GLL(Clear)
        OA_GLL(ClearColor) OA_GLL(CompileShader) OA_GLL(CreateProgram)
        OA_GLL(CreateShader) OA_GLL(DeleteBuffers) OA_GLL(DeleteFramebuffers)
        OA_GLL(DeleteProgram) OA_GLL(DeleteShader) OA_GLL(DeleteTextures)
        OA_GLL(DeleteVertexArrays) OA_GLL(Disable)
        OA_GLL(DisableVertexAttribArray) OA_GLL(DrawArrays) OA_GLL(Enable)
        OA_GLL(EnableVertexAttribArray) OA_GLL(FramebufferTexture2D)
        OA_GLL(GenBuffers) OA_GLL(GenFramebuffers) OA_GLL(GenTextures)
        OA_GLL(GenVertexArrays) OA_GLL(GetError) OA_GLL(GetIntegerv)
        OA_GLL(GetString) OA_GLL(GetProgramiv) OA_GLL(GetShaderiv)
        OA_GLL(GetShaderInfoLog) OA_GLL(GetProgramInfoLog)
        OA_GLL(GetUniformLocation) OA_GLL(LinkProgram) OA_GLL(PixelStorei)
        OA_GLL(ReadPixels) OA_GLL(Scissor) OA_GLL(ShaderSource)
        OA_GLL(TexImage2D)
        OA_GLL(TexParameteri) OA_GLL(TexSubImage2D) OA_GLL(Uniform1i)
        OA_GLL(Uniform1f) OA_GLL(Uniform2f) OA_GLL(UseProgram)
        OA_GLL(VertexAttribPointer)
        OA_GLL(Viewport)
#undef OA_GLL
        return true;
    }
};

GlProcs g;

// 便利别名：g.glXxx 保持源码与 GLES 调用同形
#define glActiveTexture g.ActiveTexture
#define glAttachShader g.AttachShader
#define glBindBuffer g.BindBuffer
#define glBindFramebuffer g.BindFramebuffer
#define glBindTexture g.BindTexture
#define glBindVertexArray g.BindVertexArray
#define glBlendEquation g.BlendEquation
#define glBlendFuncSeparate g.BlendFuncSeparate
#define glBufferData g.BufferData
#define glBufferSubData g.BufferSubData
#define glCheckFramebufferStatus g.CheckFramebufferStatus
#define glClear g.Clear
#define glClearColor g.ClearColor
#define glCompileShader g.CompileShader
#define glCreateProgram g.CreateProgram
#define glCreateShader g.CreateShader
#define glDeleteBuffers g.DeleteBuffers
#define glDeleteFramebuffers g.DeleteFramebuffers
#define glDeleteProgram g.DeleteProgram
#define glDeleteShader g.DeleteShader
#define glDeleteTextures g.DeleteTextures
#define glDeleteVertexArrays g.DeleteVertexArrays
#define glDisable g.Disable
#define glDisableVertexAttribArray g.DisableVertexAttribArray
#define glDrawArrays g.DrawArrays
#define glEnable g.Enable
#define glEnableVertexAttribArray g.EnableVertexAttribArray
#define glFramebufferTexture2D g.FramebufferTexture2D
#define glGenBuffers g.GenBuffers
#define glGenFramebuffers g.GenFramebuffers
#define glGenTextures g.GenTextures
#define glGenVertexArrays g.GenVertexArrays
#define glGetError g.GetError
#define glGetIntegerv g.GetIntegerv
#define glGetString g.GetString
#define glGetProgramiv g.GetProgramiv
#define glGetShaderiv g.GetShaderiv
#define glGetShaderInfoLog g.GetShaderInfoLog
#define glGetProgramInfoLog g.GetProgramInfoLog
#define glGetUniformLocation g.GetUniformLocation
#define glLinkProgram g.LinkProgram
#define glPixelStorei g.PixelStorei
#define glReadPixels g.ReadPixels
#define glScissor g.Scissor
#define glShaderSource g.ShaderSource
#define glTexImage2D g.TexImage2D
#define glTexParameteri g.TexParameteri
#define glTexSubImage2D g.TexSubImage2D
#define glUniform1i g.Uniform1i
#define glUniform1f g.Uniform1f
#define glUniform2f g.Uniform2f
#define glUseProgram g.UseProgram
#define glVertexAttribPointer g.VertexAttribPointer
#define glViewport g.Viewport

const char* kVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform vec2 uScale;  // 像素→NDC 系数（每轴；窗口目标 y 轴取负 = y-down）
uniform vec2 uOff;    // 像素→NDC 平移
out vec4 vColor;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos.x * uScale.x + uOff.x,
                       aPos.y * uScale.y + uOff.y, 0.0, 1.0);
    vColor = aColor;
    vUV = aUV;
}
)";

const char* kFragmentShader = R"(
#version 300 es
precision highp float;
in vec4 vColor;
in vec2 vUV;
uniform sampler2D uTex;
uniform int uUseTex;
out vec4 fragColor;
void main() {
    if (uUseTex == 1)
        fragColor = texture(uTex, vUV) * vColor; // sdl 线语义：texel*vcolor
    else
        fragColor = vColor;
}
)";

// [trans type=2] rule 灰度溶解 fragment（与 kVertexShader 同链接；vColor 未用）。
// 逐像素旧帧覆盖率 keep = smoothstep(t, t+band, rule.r)，
//   t = progress*(1+band) - band，band = max(vague,1)/255（vague 0-255 尺度），
// rule 按整幅舞台 UV 拉伸采样（灰度取 R 通道）。输出 = capture 的 rgb、
// alpha = cap.a*keep → 标准 blend 叠到当前场景上。端点连续：progress=0 →
// keep 全 1（只见旧帧），progress=1 → keep 全 0（只见新场景）；rule 灰度低
// 的像素先揭示（若要反向只改比较方向一行）。
const char* kRuleFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uTex;   // unit 0：旧帧 capture
uniform sampler2D uRule;  // unit 1：rule 灰度图
uniform float uProgress;
uniform float uBand;
out vec4 fragColor;
void main() {
    vec4 cap = texture(uTex, vUV);
    float t = uProgress * (1.0 + uBand) - uBand;
    float r = texture(uRule, vUV).r;
    float keep = smoothstep(t, t + uBand, r);
    fragColor = vec4(cap.rgb, cap.a * keep);
}
)";

unsigned int compile_shader(unsigned int type, const char* src, char* log,
                            size_t logsz) {
    const unsigned int sh = glCreateShader(type);
    const GLchar_* s = src;
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint_ ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
    if (!ok && log && logsz) {
        GLsizei_ len = 0;
        glGetShaderInfoLog(sh, (GLsizei_)logsz, &len, log);
    }
    return ok ? sh : 0;
}

} // namespace

namespace oa::render {

GlesRenderBackend::~GlesRenderBackend() { shutdown(); }

void GlesRenderBackend::fail(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    err_ = buf;
    SDL_SetError("%s", buf);
}

bool GlesRenderBackend::ensure_program() {
    if (program_) return true;
    char log[512] = {0};
    const unsigned int vs = compile_shader(GL_VERTEX_SHADER_, kVertexShader,
                                           log, sizeof(log));
    if (!vs) {
        fail("gles: vertex shader compile failed: %s", log);
        return false;
    }
    const unsigned int fs = compile_shader(GL_FRAGMENT_SHADER_, kFragmentShader,
                                           log, sizeof(log));
    if (!fs) {
        fail("gles: fragment shader compile failed: %s", log);
        glDeleteShader(vs);
        return false;
    }
    const unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint_ ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        GLsizei_ len = 0;
        glGetProgramInfoLog(prog, (GLsizei_)sizeof(log), &len, log);
        fail("gles: program link failed: %s", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    loc_scale_ = glGetUniformLocation(prog, "uScale");
    loc_off_ = glGetUniformLocation(prog, "uOff");
    loc_usetex_ = glGetUniformLocation(prog, "uUseTex");
    const GLint_ utex = glGetUniformLocation(prog, "uTex");
    program_ = prog;
    glUseProgram(program_);
    if (utex >= 0) glUniform1i(utex, 0);
    // VAO/VBO：交错 8 float/顶点（pos.xy + color.rgba + uv.xy）
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)(2 * sizeof(GLfloat_)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)(6 * sizeof(GLfloat_)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER_, 0);
    return true;
}

bool GlesRenderBackend::ensure_rule_program() {
    if (rule_program_) return true;
    char log[512] = {0};
    const unsigned int vs = compile_shader(GL_VERTEX_SHADER_, kVertexShader,
                                           log, sizeof(log));
    if (!vs) {
        fail("gles: rule vertex shader compile failed: %s", log);
        return false;
    }
    const unsigned int fs = compile_shader(GL_FRAGMENT_SHADER_, kRuleFragmentShader,
                                           log, sizeof(log));
    if (!fs) {
        fail("gles: rule fragment shader compile failed: %s", log);
        glDeleteShader(vs);
        return false;
    }
    const unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint_ ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        GLsizei_ len = 0;
        glGetProgramInfoLog(prog, (GLsizei_)sizeof(log), &len, log);
        fail("gles: rule program link failed: %s", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    rule_loc_scale_ = glGetUniformLocation(prog, "uScale");
    rule_loc_off_ = glGetUniformLocation(prog, "uOff");
    rule_loc_progress_ = glGetUniformLocation(prog, "uProgress");
    rule_loc_band_ = glGetUniformLocation(prog, "uBand");
    rule_loc_utex_ = glGetUniformLocation(prog, "uTex");
    rule_loc_urule_ = glGetUniformLocation(prog, "uRule");
    rule_program_ = prog;
    glUseProgram(rule_program_);
    if (rule_loc_utex_ >= 0) glUniform1i(rule_loc_utex_, 0);
    if (rule_loc_urule_ >= 0) glUniform1i(rule_loc_urule_, 1);
    glUseProgram(program_); // 状态回到主 program
    return true;
}

bool GlesRenderBackend::create(SDL_Window* window, int stage_w, int stage_h,
                               BackendInfo* info)
{
    err_.clear();
    window_ = window;
    logical_w_ = stage_w;
    logical_h_ = stage_h;
    if (!gl_context_) {
        // GLES 上下文由后端 create() 内创建
        // （SDL_GL ES profile；本机 Xvfb/radeonsi 实测 ES 3.2 可建）。
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        gl_context_ = SDL_GL_CreateContext(window);
        if (!gl_context_) {
            fail("gles: GLES context creation failed: %s", SDL_GetError());
            return false;
        }
        if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)gl_context_)) {
            fail("gles: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
            return false;
        }
        const char* missing = nullptr;
        if (!g.load(&missing)) {
            fail("gles: GL entry point '%s' unavailable", missing);
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
            return false;
        }
        const GLubyte_* ver = glGetString(GL_VERSION_);
        const GLubyte_* ren = glGetString(GL_RENDERER_);
        std::printf("[gles] context: %s (%s)\n", ver ? (const char*)ver : "?",
                    ren ? (const char*)ren : "?");
        glDisable(GL_DEPTH_TEST_);
        glDisable(GL_CULL_FACE_);
        glDisable(GL_SCISSOR_TEST_);
    }
    if (!ensure_program()) return false;
    // 输出/逻辑尺寸与 letterbox（SDL3 UpdateLogicalPresentation 数学镜像；
    // DISABLED 分支 scale=1 dst=full —— 引擎总是开 letterbox，mode=2）。
    SDL_GetWindowSizeInPixels(window, &out_w_, &out_h_);
    int ws = 0, hs = 0;
    SDL_GetWindowSize(window, &ws, &hs);
    dpi_x_ = ws > 0 ? float(out_w_) / float(ws) : 1.0f;
    dpi_y_ = hs > 0 ? float(out_h_) / float(hs) : 1.0f;
    const float ow = float(out_w_), oh = float(out_h_);
    const float lw = float(logical_w_), lh = float(logical_h_);
    const float want = lw / lh, real = ow / oh;
    if (std::fabs(want - real) < 0.0001f) {
        dst_x_ = 0; dst_y_ = 0; dst_w_ = ow; dst_h_ = oh;
    } else if (want > real) { // 宽于输出：上下黑边
        const float s = ow / lw;
        dst_x_ = 0; dst_w_ = ow;
        dst_h_ = std::floor(lh * s);
        dst_y_ = (oh - dst_h_) / 2.0f;
    } else { // 窄于输出：左右黑边
        const float s = oh / lh;
        dst_y_ = 0; dst_h_ = oh;
        dst_w_ = std::floor(lw * s);
        dst_x_ = (ow - dst_w_) / 2.0f;
    }
    cur_scale_x_ = dst_w_ / lw;
    cur_scale_y_ = dst_h_ / lh;
    if (info) {
        info->output_w = out_w_;
        info->output_h = out_h_;
        info->logical_w = stage_w;
        info->logical_h = stage_h;
        info->logical_mode = 2; // SDL_LOGICAL_PRESENTATION_LETTERBOX
    }
    created_ = true;
    return true;
}

void GlesRenderBackend::shutdown()
{
    if (!created_) {
        // 半成品上下文也要收掉（create 失败路径已自行清理，这里兜底）
        if (gl_context_) {
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
        }
        return;
    }
    if (program_) glDeleteProgram(program_);
    if (rule_program_) glDeleteProgram(rule_program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    program_ = 0; rule_program_ = 0; vao_ = 0; vbo_ = 0;
    if (gl_context_) {
        SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
        gl_context_ = nullptr;
    }
    cur_target_ = nullptr;
    created_ = false;
}

const char* GlesRenderBackend::last_error()
{
    return err_.empty() ? "" : err_.c_str();
}

// ---------------------------------------------------------------------------
// target / clip 状态
// ---------------------------------------------------------------------------

TextureRef GlesRenderBackend::current_target() { return cur_target_; }

void GlesRenderBackend::set_target(TextureRef t)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt && t) return; // 无效纹理
    if (!gl_context_) return;
    if (gt) {
        if (!gt->fbo) return; // 非 target 纹理不能绑
        cur_target_ = gt;
        glBindFramebuffer(GL_FRAMEBUFFER_, gt->fbo);
        glViewport(0, 0, gt->w, gt->h);
    } else {
        cur_target_ = nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER_, 0);
        ensure_window_ready();
    }
}

void GlesRenderBackend::ensure_window_ready()
{
    // 目标=窗口：刷新输出像素尺寸；变了就重算 letterbox（resize 语义，
    // SDL3 UpdateLogicalPresentation 同数学）。
    int ow = 0, oh = 0;
    SDL_GetWindowSizeInPixels(window_, &ow, &oh);
    if (ow == out_w_ && oh == out_h_) return;
    out_w_ = ow; out_h_ = oh;
    int ws = 0, hs = 0;
    SDL_GetWindowSize(window_, &ws, &hs);
    dpi_x_ = ws > 0 ? float(ow) / float(ws) : 1.0f;
    dpi_y_ = hs > 0 ? float(oh) / float(hs) : 1.0f;
    const float fow = float(ow), foh = float(oh);
    const float lw = float(logical_w_), lh = float(logical_h_);
    const float want = lw / lh, real = fow / foh;
    if (std::fabs(want - real) < 0.0001f) {
        dst_x_ = 0; dst_y_ = 0; dst_w_ = fow; dst_h_ = foh;
    } else if (want > real) {
        const float s = fow / lw;
        dst_x_ = 0; dst_w_ = fow;
        dst_h_ = std::floor(lh * s);
        dst_y_ = (foh - dst_h_) / 2.0f;
    } else {
        const float s = foh / lh;
        dst_y_ = 0; dst_h_ = foh;
        dst_w_ = std::floor(lw * s);
        dst_x_ = (fow - dst_w_) / 2.0f;
    }
    cur_scale_x_ = dst_w_ / lw;
    cur_scale_y_ = dst_h_ / lh;
}

bool GlesRenderBackend::clip_enabled() { return clip_enabled_; }

IRect GlesRenderBackend::clip_rect() { return clip_; }

void GlesRenderBackend::set_clip(const IRect& r)
{
    clip_enabled_ = true;
    clip_ = r;
}

void GlesRenderBackend::clear_clip()
{
    clip_enabled_ = false;
}

void GlesRenderBackend::set_draw_blend(BlendMode m) { draw_blend_ = m; }

void GlesRenderBackend::set_draw_color(uint8_t cr, uint8_t cg, uint8_t cb,
                                       uint8_t ca)
{
    draw_color_[0] = cr;
    draw_color_[1] = cg;
    draw_color_[2] = cb;
    draw_color_[3] = ca;
}

/// 每绘制的混合 + clip 状态（目标空间像素坐标）。SDL 驱动语义：
/// clip 对 target = 直接；对窗口 = 逻辑坐标缩放后翻转（画到 dst 区）。
void GlesRenderBackend::apply_draw_state(GlesTexture* tex, BlendMode blend)
{
    glUseProgram(program_);
    if (blend == BlendMode::None) {
        glDisable(GL_BLEND_);
    } else {
        glEnable(GL_BLEND_);
        switch (blend) {
            case BlendMode::Blend:
                glBlendFuncSeparate(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_,
                                    GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);
                break;
            case BlendMode::Add:
                glBlendFuncSeparate(GL_SRC_ALPHA_, GL_ONE_, GL_ZERO_, GL_ONE_);
                break;
            case BlendMode::Mod:
                glBlendFuncSeparate(GL_ZERO_, GL_SRC_COLOR_, GL_ZERO_, GL_ONE_);
                break;
            case BlendMode::AlphaMultiply:
                // dstRGB kept, dstA *= srcA (stencil mask composite)
                glBlendFuncSeparate(GL_ZERO_, GL_ONE_, GL_DST_ALPHA_, GL_ZERO_);
                break;
            case BlendMode::None: break;
        }
        glBlendEquation(GL_FUNC_ADD_);
    }
    if (cur_target_) {
        // FBO：clip = 内容坐标（SDL target 分支：不翻行）
        if (clip_enabled_ && clip_.w > 0 && clip_.h > 0) {
            glEnable(GL_SCISSOR_TEST_);
            glScissor(clip_.x, clip_.y, clip_.w, clip_.h);
        } else {
            glDisable(GL_SCISSOR_TEST_);
        }
        glUniform2f(loc_scale_, 2.0f / float(cur_target_->w),
                    2.0f / float(cur_target_->h));
        glUniform2f(loc_off_, -1.0f, -1.0f);
    } else {
        // 窗口：clip 按逻辑坐标缩放（floor/ceil，SDL UpdatePixelClipRect），
        // 落在 letterbox dst 区内（y 翻转到窗口底原点）
        if (clip_enabled_ && clip_.w > 0 && clip_.h > 0) {
            const float cx = std::floor(clip_.x * cur_scale_x_);
            const float cy = std::floor(clip_.y * cur_scale_y_);
            const float cw = std::ceil(float(clip_.w) * cur_scale_x_);
            const float ch = std::ceil(float(clip_.h) * cur_scale_y_);
            glEnable(GL_SCISSOR_TEST_);
            glScissor((GLint_)(std::floor(dst_x_) + cx),
                      (GLint_)(float(out_h_) - (std::floor(dst_y_) + cy) -
                               ch),
                      (GLsizei_)cw, (GLsizei_)ch);
        } else {
            glDisable(GL_SCISSOR_TEST_);
        }
        // y-down：内容 y=0 → NDC +1（SDL 窗口目标 glOrtho(0,w,h,0) 语义）
        const float dw = dst_w_ > 0 ? dst_w_ : 1.0f;
        const float dh = dst_h_ > 0 ? dst_h_ : 1.0f;
        glViewport((GLint_)std::floor(dst_x_),
                   (GLint_)(float(out_h_) - std::floor(dst_y_) - dst_h_),
                   (GLsizei_)std::ceil(dw), (GLsizei_)std::ceil(dh));
        glUniform2f(loc_scale_, 2.0f / dw, -2.0f / dh);
        glUniform2f(loc_off_, -1.0f, 1.0f);
    }
    (void)tex;
    glUniform1i(loc_usetex_, 0);
    // 记住本次的像素→NDC 数学（rule program 复用同一坐标约定）
    if (cur_target_) {
        last_scale_[0] = 2.0f / float(cur_target_->w);
        last_scale_[1] = 2.0f / float(cur_target_->h);
        last_off_[0] = -1.0f;
        last_off_[1] = -1.0f;
    } else {
        const float dw = dst_w_ > 0 ? dst_w_ : 1.0f;
        const float dh = dst_h_ > 0 ? dst_h_ : 1.0f;
        last_scale_[0] = 2.0f / dw;
        last_scale_[1] = -2.0f / dh;
        last_off_[0] = -1.0f;
        last_off_[1] = 1.0f;
    }
}

void GlesRenderBackend::clear()
{
    if (!gl_context_) return;
    // glClear 受 scissor 影响：SDL 驱动在 clear 时先关 scissor（全目标清，
    // SDL CLEAR 命令同语义）；后续绘制的 apply_draw_state 按需重开。
    if (clip_enabled_) glDisable(GL_SCISSOR_TEST_);
    glClearColor(draw_color_[0] / 255.0f, draw_color_[1] / 255.0f,
                 draw_color_[2] / 255.0f, draw_color_[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT_);
}

void GlesRenderBackend::emit_quad(const float x0, const float y0,
                                  const float x1, const float y1,
                                  const float u0, const float v0,
                                  const float u1, const float v1,
                                  const float col[4])
{
    // 6 顶点（SDL rect_index_order {0,1,2,0,2,3} 的两三角形布局，
    // 顶点序 (minx,miny) (maxx,miny) (maxx,maxy) (minx,maxy)）
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    GLfloat_ verts[6 * 8];
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        GLfloat_* v = verts + i * 8;
        v[0] = xy[k][0];
        v[1] = xy[k][1];
        v[2] = col[0];
        v[3] = col[1];
        v[4] = col[2];
        v[5] = col[3];
        v[6] = uv[k][0];
        v[7] = uv[k][1];
    }
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glBufferData(GL_ARRAY_BUFFER_, sizeof(verts), verts, GL_DYNAMIC_DRAW_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES_, 0, 6);
    glBindVertexArray(0);
}

void GlesRenderBackend::fill_rect(const FRect& dst)
{
    if (!gl_context_) return;
    apply_draw_state(nullptr, draw_blend_);
    const float col[4] = {draw_color_[0] / 255.0f, draw_color_[1] / 255.0f,
                          draw_color_[2] / 255.0f, draw_color_[3] / 255.0f};
    glUniform1i(loc_usetex_, 0);
    emit_quad(dst.x, dst.y, dst.x + dst.w, dst.y + dst.h, 0, 0, 0, 0, col);
}

void GlesRenderBackend::draw_texture(TextureRef t, const FRect* src,
                                     const FRect* dst)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || !dst) return;
    apply_draw_state(gt, gt->blend);
    const float tw = float(gt->w), th = float(gt->h);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src && src->w > 0 && src->h > 0) {
        u0 = src->x / tw; // SDL 共享几何：src rect / 纹理尺寸（float 除法）
        v0 = src->y / th;
        u1 = (src->x + src->w) / tw;
        v1 = (src->y + src->h) / th;
    }
    const float col[4] = {gt->color_mod[0] / 255.0f, gt->color_mod[1] / 255.0f,
                          gt->color_mod[2] / 255.0f,
                          gt->alpha_mod / 255.0f};
    glActiveTexture(0x84C0); // GL_TEXTURE0
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    glUniform1i(loc_usetex_, 1);
    emit_quad(dst->x, dst->y, dst->x + dst->w, dst->y + dst->h, u0, v0, u1, v1,
              col);
}

void GlesRenderBackend::draw_texture_affine(TextureRef t, const FRect* src,
                                            const FPoint& o, const FPoint& r,
                                            const FPoint& d)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return;
    apply_draw_state(gt, gt->blend);
    const float tw = float(gt->w), th = float(gt->h);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src && src->w > 0 && src->h > 0) {
        u0 = src->x / tw;
        v0 = src->y / th;
        u1 = (src->x + src->w) / tw;
        v1 = (src->y + src->h) / th;
    }
    const float col[4] = {gt->color_mod[0] / 255.0f, gt->color_mod[1] / 255.0f,
                          gt->color_mod[2] / 255.0f,
                          gt->alpha_mod / 255.0f};
    // SDL_RenderTextureAffine：o=origin r=right d=down；第 4 角 = r+d-o
    const float qx[4] = {o.x, r.x, r.x + d.x - o.x, d.x};
    const float qy[4] = {o.y, r.y, r.y + d.y - o.y, d.y};
    const float qu[4] = {u0, u1, u1, u0};
    const float qv[4] = {v0, v0, v1, v1};
    glActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    glUniform1i(loc_usetex_, 1);
    GLfloat_ verts[6 * 8];
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        GLfloat_* v = verts + i * 8;
        v[0] = qx[k]; v[1] = qy[k];
        v[2] = col[0]; v[3] = col[1]; v[4] = col[2]; v[5] = col[3];
        v[6] = qu[k]; v[7] = qv[k];
    }
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glBufferData(GL_ARRAY_BUFFER_, sizeof(verts), verts, GL_DYNAMIC_DRAW_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES_, 0, 6);
    glBindVertexArray(0);
}

void GlesRenderBackend::draw_geometry(TextureRef t, const Vertex* verts,
                                      int nverts, const int* indices,
                                      int nindices)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || nverts <= 0 || !verts) return;
    apply_draw_state(gt, gt->blend);
    const int count = (indices && nindices > 0) ? nindices : nverts;
    if (count <= 0) return;
    std::vector<GLfloat_> buf(size_t(count) * 8);
    for (int i = 0; i < count; ++i) {
        const int j = indices ? indices[i] : i;
        if (j < 0 || j >= nverts) continue;
        const Vertex& sv = verts[j];
        GLfloat_* v = buf.data() + size_t(i) * 8;
        v[0] = sv.pos.x;
        v[1] = sv.pos.y;
        // 顶点色（emote：白×部件 alpha）；SDL GL 几何路径逐顶点同语义
        v[2] = sv.color.r; v[3] = sv.color.g;
        v[4] = sv.color.b; v[5] = sv.color.a;
        v[6] = sv.uv.x;    v[7] = sv.uv.y;
    }
    glActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    glUniform1i(loc_usetex_, 1);
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glBufferData(GL_ARRAY_BUFFER_, GLsizeiptr_(buf.size() * sizeof(GLfloat_)),
                 buf.data(), GL_DYNAMIC_DRAW_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES_, 0, (GLsizei_)count);
    glBindVertexArray(0);
}

bool GlesRenderBackend::draw_rule_transition(TextureRef capture,
                                             TextureRef rule,
                                             float progress, float band) {
    // type-2 rule 溶解：整幅旧帧以逐像素 keep 为 alpha 叠到当前 target。
    // 只在 FBO（stage 目标）上做——与主转场 overlay 同目标；窗口目标返回
    // false（引擎回退交叉淡化）。坐标数学（scale/off、blend、scissor）经
    // apply_draw_state 复用主 program 路径，随后切到 rule program 仅换
    // uniforms/纹理绑定——GL 状态与 program 无关，几何映射逐像素一致。
    if (!gl_context_ || !cur_target_ || !program_) return false;
    GlesTexture* cap = gles_tex(capture);
    GlesTexture* rl = gles_tex(rule);
    if (!cap || !cap->tex || !rl || !rl->tex) return false;
    if (!ensure_rule_program()) return false;
    apply_draw_state(nullptr, BlendMode::Blend); // blend/scissor/scale/off
    glUseProgram(rule_program_);
    glUniform2f(rule_loc_scale_, last_scale_[0], last_scale_[1]);
    glUniform2f(rule_loc_off_, last_off_[0], last_off_[1]);
    glUniform1f(rule_loc_progress_, progress);
    glUniform1f(rule_loc_band_, band);
    glActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D_, cap->tex);
    glUniform1i(rule_loc_utex_, 0);
    glActiveTexture(GL_TEXTURE1_);
    glBindTexture(GL_TEXTURE_2D_, rl->tex);
    glUniform1i(rule_loc_urule_, 1);
    glActiveTexture(GL_TEXTURE0_);
    const float col[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // 顶点色未参与 rule 求值
    const float w = float(cur_target_->w), h = float(cur_target_->h);
    emit_quad(0.0f, 0.0f, w, h, 0.0f, 0.0f, 1.0f, 1.0f, col);
    glUseProgram(program_); // 后续普通绘制各自 apply_draw_state；这里归位
    return true;
}

void GlesRenderBackend::present()
{
    if (!gl_context_) return;
    // GL 错误 canary（OA_RENDER_DIAG 的后端错误面）：每帧 drain 一次
    // glGetError；有错则记录到 err_（呈现帧内发生的 GL 错误会在这里现身）。
    for (;;) {
        const GLenum_ e = glGetError();
        if (e == GL_NO_ERROR_) break;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "gles: GL error 0x%x", (unsigned)e);
        err_ = buf; // 保留最后一条（下一个 present 前不再覆盖）
        break;
    }
    SDL_GL_SwapWindow(window_);
    // present 后引擎把 target 切回 stage FBO（render_end 尾部）；无需复位。
}

// ---------------------------------------------------------------------------
// 纹理对象
// ---------------------------------------------------------------------------

TextureRef GlesRenderBackend::create_texture(int w, int h,
                                             TextureAccess access)
{
    if (!gl_context_ || w <= 0 || h <= 0) return nullptr;
    GlesTexture* gt = new GlesTexture();
    gt->w = w;
    gt->h = h;
    gt->access = access;
    glGenTextures(1, &gt->tex);
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    glTexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, GL_RGBA_,
                 GL_UNSIGNED_BYTE_, nullptr);
    // 无 mipmap（sdl 线默认 scale mode = LINEAR，SDL GL 驱动同配置）
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    if (access == TextureAccess::Streaming) {
        gt->staging.resize(size_t(w) * h * 4);
    } else if (access == TextureAccess::Target) {
        glGenFramebuffers(1, &gt->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER_, gt->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                               GL_TEXTURE_2D_, gt->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER_) !=
            GL_FRAMEBUFFER_COMPLETE_) {
            fail("gles: framebuffer incomplete (%dx%d %s)", w, h,
                 access == TextureAccess::Target ? "target" : "");
            glBindFramebuffer(GL_FRAMEBUFFER_, 0);
            glDeleteFramebuffers(1, &gt->fbo);
            glDeleteTextures(1, &gt->tex);
            delete gt;
            return nullptr;
        }
        glBindFramebuffer(GL_FRAMEBUFFER_, 0);
    }
    glBindTexture(GL_TEXTURE_2D_, 0);
    return gt;
}

void GlesRenderBackend::destroy_texture(TextureRef t)
{
    if (!t) return;
    GlesTexture* gt = static_cast<GlesTexture*>(t);
    if (gt == cur_target_) cur_target_ = nullptr;
    if (gl_context_) { // 后端 shutdown 后的销毁（字形纹理随 FontSystem 晚于
        // release_all 析构）只能释放包装——GL 对象已随上下文销毁。
        // （SDL 端同路径靠 SDL3 对象校验 no-op；GLES 必须显式守卫。）
        if (gt->fbo) glDeleteFramebuffers(1, &gt->fbo);
        if (gt->tex) glDeleteTextures(1, &gt->tex);
    }
    delete gt;
}

void GlesRenderBackend::update_texture(TextureRef t, const uint8_t* rgba,
                                       int pitch)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || !rgba) return;
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 1);
    if (pitch != gt->w * 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, gt->w, gt->h, GL_RGBA_,
                    GL_UNSIGNED_BYTE_, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 4);
    glBindTexture(GL_TEXTURE_2D_, 0);
}

bool GlesRenderBackend::lock_texture(TextureRef t, uint8_t** pixels,
                                     int* pitch)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return false;
    if (gt->staging.empty()) gt->staging.assign(size_t(gt->w) * gt->h * 4, 0);
    *pixels = gt->staging.data();
    *pitch = gt->w * 4;
    return true;
}

void GlesRenderBackend::unlock_texture(TextureRef t)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || gt->staging.empty()) return;
    update_texture(t, gt->staging.data(), gt->w * 4);
}

bool GlesRenderBackend::texture_size(TextureRef t, float* w, float* h)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return false;
    if (w) *w = float(gt->w);
    if (h) *h = float(gt->h);
    return true;
}

void GlesRenderBackend::set_texture_blend(TextureRef t, BlendMode m)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) gt->blend = m;
}

void GlesRenderBackend::set_texture_alpha_mod(TextureRef t, uint8_t a)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) gt->alpha_mod = a;
}

void GlesRenderBackend::set_texture_color_mod(TextureRef t, uint8_t cr,
                                              uint8_t cg, uint8_t cb)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) {
        gt->color_mod[0] = cr;
        gt->color_mod[1] = cg;
        gt->color_mod[2] = cb;
    }
}

// ---------------------------------------------------------------------------
// 读回 + 坐标
// ---------------------------------------------------------------------------

bool GlesRenderBackend::read_target(int* w, int* h,
                                    std::vector<uint8_t>* rgba)
{
    if (!gl_context_) return false;
    if (!cur_target_) {
        // 窗口：读 letterbox dst 内容区（SDL 窗口 readback 语义：
        // y 翻行读 + 行序翻转 → top-down）
        if (!out_w_ || !out_h_ || dst_w_ <= 0 || dst_h_ <= 0) return false;
        const int rw = (int)std::ceil(dst_w_);
        const int rh = (int)std::ceil(dst_h_);
        const int rx = (int)std::floor(dst_x_);
        const int ry = (int)std::floor(dst_y_);
        rgba->resize(size_t(rw) * rh * 4);
        std::vector<uint8_t> raw(size_t(rw) * rh * 4);
        glPixelStorei(GL_PACK_ALIGNMENT_, 1);
        glReadPixels(rx, out_h_ - ry - rh, rw, rh, GL_RGBA_,
                     GL_UNSIGNED_BYTE_, raw.data());
        // 行翻转：raw 底→顶 → top-down
        for (int y = 0; y < rh; ++y) {
            std::memcpy(rgba->data() + size_t(y) * rw * 4,
                        raw.data() + size_t(rh - 1 - y) * rw * 4,
                        size_t(rw) * 4);
        }
        if (w) *w = rw;
        if (h) *h = rh;
        return true;
    }
    // FBO：row0 = 内容顶（无需翻转，SDL target readback 语义）
    GlesTexture* gt = cur_target_;
    rgba->resize(size_t(gt->w) * gt->h * 4);
    glPixelStorei(0x0D05, 1);
    glReadPixels(0, 0, gt->w, gt->h, GL_RGBA_, GL_UNSIGNED_BYTE_,
                 rgba->data());
    if (w) *w = gt->w;
    if (h) *h = gt->h;
    return true;
}

bool GlesRenderBackend::window_to_render(float wx, float wy, float* rx,
                                         float* ry)
{
    // SDL_RenderCoordinatesFromWindow 数学镜像（main_view viewport 0, scale 1）
    float x = wx * dpi_x_;
    float y = wy * dpi_y_;
    const float lw = float(logical_w_), lh = float(logical_h_);
    x = (x - dst_x_) * lw / dst_w_;
    y = (y - dst_y_) * lh / dst_h_;
    if (rx) *rx = x;
    if (ry) *ry = y;
    return true;
}

bool GlesRenderBackend::render_to_window(float rx, float ry, float* wx,
                                         float* wy)
{
    // SDL_RenderCoordinatesToWindow 数学镜像
    float x = rx, y = ry;
    const float lw = float(logical_w_), lh = float(logical_h_);
    x = dst_x_ + x * dst_w_ / lw;
    y = dst_y_ + y * dst_h_ / lh;
    if (wx) *wx = x / dpi_x_;
    if (wy) *wy = y / dpi_y_;
    return true;
}

} // namespace oa::render
