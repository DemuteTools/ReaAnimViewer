// SPDX-License-Identifier: MIT
//
// Modern-GL function loader. Windows opengl32.dll exports only GL 1.1; every
// entry point >= 1.5 (shaders, VBOs, VAOs) is resolved at runtime via
// wglGetProcAddress against a *current* WGL context. This file plus
// renderer.cpp are the only translation units that call modern GL (the raw-GL
// equivalent of the architecture's "renderer.cpp is the only file that calls
// sg_*" rule — see architecture Spec Change Log 2026-06-23).

#pragma once

#include <string>

#ifdef _WIN32

#include <windows.h>
#include <gl/GL.h>

namespace rav {

// Resolves every modern-GL function pointer in the table below against the
// currently-current GL context. Call once, immediately after wglMakeCurrent.
// Returns false and sets out_error to the first missing function name on
// failure (an old/software driver may not export everything we need).
//
// Epic 11: the table is process-wide and also used by the video FX renderer on
// REAPER's video thread (its own context). Loading is mutex-guarded and the
// pointers are only written when they are not loaded yet or differ from the
// current context's (the viewer always wins); a change bumps GlFunctionsGeneration
// so the video thread re-checks before drawing. On failure nothing is written.
bool LoadGlFunctions(std::string& out_error);

// Video-thread side (Story 11-2): with ANOTHER context current, makes sure the
// process-wide table is usable from it. Loads the table when nothing is loaded yet;
// otherwise compares this context's entry points with the loaded ones and returns
// false (out_error set, nothing written) when they differ: drawing through the
// table from this context would then call another driver's code. Thread-safe.
// `out_generation` receives the generation the check was made against.
bool CheckGlFunctionsForCurrentContext(std::string& out_error, unsigned* out_generation);

// Bumped every time the table's pointers change. Thread-safe.
unsigned GlFunctionsGeneration();

}  // namespace rav

// ---- Types and enums missing from the GL 1.1 <gl/GL.h> ----------------------
// (GL >= 1.5 / 2.0 / 3.0 symbols the modern entry points need.)

typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_INFO_LOG_LENGTH        0x8B84
#define GL_ARRAY_BUFFER           0x8892
#define GL_ELEMENT_ARRAY_BUFFER   0x8893
#define GL_STATIC_DRAW            0x88E4
#define GL_DYNAMIC_DRAW           0x88E8
#define GL_TEXTURE0              0x84C0   // GL 1.3 — first texture unit for glActiveTexture
#define GL_TEXTURE1              0x84C1   // GL 1.3 — second texture unit (normal map, Story 6.5.1)
#define GL_SRGB8_ALPHA8          0x8C43   // GL 2.1/3.0 — sRGB internal format: GPU sRGB→linear decode on sample
#endif

// GL_RGBA8 (0x8058) is a GL 1.1 sized internal format; Windows <gl/GL.h> already
// defines it. Guard so we don't trip MSVC C4005 redefinition under /W3, while still
// providing it for any toolchain whose header omits it (linear normal-map upload).
#ifndef GL_RGBA8
#define GL_RGBA8                 0x8058
#endif

// GL 3.0 framebuffer-object enums for the Story 6.5.4 shadow-map depth FBO (the
// renderer now binds a transient offscreen depth FBO for the shadow pass — see the
// renderer.h header comment + the AR20 Spec Change Log). The default Windows <gl/GL.h>
// is 1.1 and omits these; guard each so a newer toolchain header doesn't trip C4005.
// GL_DEPTH_COMPONENT / GL_NONE / GL_FLOAT are GL 1.1 (already in <gl/GL.h>); only the
// FBO + clamp enums need providing here.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER           0x8D40
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT      0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24     0x81A6  // sized depth internal format for the shadow map
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE         0x812F  // GL 1.2; clamp shadow-map sampling at the border
#endif

// Story 6.5.6 — GL 3.0 enums for the offscreen MULTISAMPLE COLOUR FBO + blit-resolve (the
// games-standard MSAA path: render the scene into a multisample renderbuffer FBO whose sample
// count is reallocatable at runtime, then glBlitFramebuffer-resolve it to the window — see the
// renderer.h header comment + the AR20 Spec Change Log). GL_COLOR_BUFFER_BIT / GL_NEAREST are
// GL 1.1 (already in <gl/GL.h>); only these need providing. Guard each against C4005.
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER          0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0     0x8CE0
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER      0x8CA8  // glBlitFramebuffer READ target
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER      0x8CA9  // glBlitFramebuffer DRAW target
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES           0x8D57  // queried once to clamp the MSAA level selector
#endif

// ---- Modern-GL function table (X-macro; defined in gl_loader.cpp) ------------
// Epic 2/3 append rows here (textures, glVertexAttribIPointer, …) without churn.

#define RAV_GL_FUNCS(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void,   glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void,   glCompileShader, (GLuint)) \
    X(void,   glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void,   glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void,   glAttachShader, (GLuint, GLuint)) \
    X(void,   glLinkProgram, (GLuint)) \
    X(void,   glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void,   glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,   glDeleteShader, (GLuint)) \
    X(void,   glDeleteProgram, (GLuint)) \
    X(void,   glUseProgram, (GLuint)) \
    X(GLint,  glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void,   glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
    X(void,   glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
    X(void,   glUniform3fv, (GLint, GLsizei, const GLfloat*)) \
    X(void,   glUniform1f, (GLint, GLfloat)) \
    X(void,   glUniform1i, (GLint, GLint)) \
    X(void,   glGenBuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteBuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindBuffer, (GLenum, GLuint)) \
    X(void,   glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void,   glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void,   glDeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void,   glBindVertexArray, (GLuint)) \
    X(void,   glEnableVertexAttribArray, (GLuint)) \
    X(void,   glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void,   glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*)) \
    X(void,   glActiveTexture, (GLenum)) \
    X(void,   glGenerateMipmap, (GLenum)) \
    X(void,   glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindFramebuffer, (GLenum, GLuint)) \
    X(void,   glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void,   glGenRenderbuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteRenderbuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindRenderbuffer, (GLenum, GLuint)) \
    X(void,   glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void,   glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void,   glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))

#define RAV_GL_DECL(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; extern PFN_##name rav_##name;
RAV_GL_FUNCS(RAV_GL_DECL)
#undef RAV_GL_DECL

// Route the modern names (absent from GL 1.1) to the resolved pointers, so call
// sites read as plain GL. renderer.cpp includes this header to get them.
#define glCreateShader            rav_glCreateShader
#define glShaderSource            rav_glShaderSource
#define glCompileShader           rav_glCompileShader
#define glGetShaderiv             rav_glGetShaderiv
#define glGetShaderInfoLog        rav_glGetShaderInfoLog
#define glCreateProgram           rav_glCreateProgram
#define glAttachShader            rav_glAttachShader
#define glLinkProgram             rav_glLinkProgram
#define glGetProgramiv            rav_glGetProgramiv
#define glGetProgramInfoLog       rav_glGetProgramInfoLog
#define glDeleteShader            rav_glDeleteShader
#define glDeleteProgram           rav_glDeleteProgram
#define glUseProgram              rav_glUseProgram
#define glGetUniformLocation      rav_glGetUniformLocation
#define glUniformMatrix4fv        rav_glUniformMatrix4fv
#define glUniformMatrix3fv        rav_glUniformMatrix3fv
#define glUniform3fv              rav_glUniform3fv
#define glUniform1f               rav_glUniform1f
#define glUniform1i               rav_glUniform1i
#define glGenBuffers              rav_glGenBuffers
#define glDeleteBuffers           rav_glDeleteBuffers
#define glBindBuffer              rav_glBindBuffer
#define glBufferData              rav_glBufferData
#define glGenVertexArrays         rav_glGenVertexArrays
#define glDeleteVertexArrays      rav_glDeleteVertexArrays
#define glBindVertexArray         rav_glBindVertexArray
#define glEnableVertexAttribArray rav_glEnableVertexAttribArray
#define glVertexAttribPointer     rav_glVertexAttribPointer
#define glVertexAttribIPointer    rav_glVertexAttribIPointer
#define glActiveTexture           rav_glActiveTexture
#define glGenerateMipmap          rav_glGenerateMipmap
#define glGenFramebuffers         rav_glGenFramebuffers
#define glDeleteFramebuffers      rav_glDeleteFramebuffers
#define glBindFramebuffer         rav_glBindFramebuffer
#define glFramebufferTexture2D    rav_glFramebufferTexture2D
#define glCheckFramebufferStatus  rav_glCheckFramebufferStatus
#define glGenRenderbuffers        rav_glGenRenderbuffers
#define glDeleteRenderbuffers     rav_glDeleteRenderbuffers
#define glBindRenderbuffer        rav_glBindRenderbuffer
#define glRenderbufferStorageMultisample rav_glRenderbufferStorageMultisample
#define glFramebufferRenderbuffer rav_glFramebufferRenderbuffer
#define glBlitFramebuffer         rav_glBlitFramebuffer

#endif  // _WIN32
