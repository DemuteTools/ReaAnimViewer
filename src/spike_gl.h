// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Minimal modern-GL surface for the spike. Windows opengl32.dll exports only
// GL 1.1; FBO / shader / VAO entry points are resolved via wglGetProcAddress.
// Also owns a hidden WGL context (sokol is intentionally NOT used — see
// SPIKE0_FINDINGS.md: the pinned sokol_gfx uses a new view-based API that the
// spike avoids to isolate the ReaImGui-bridge question; raw GL proves the same
// GL-context-coexistence + skinning + readback path).

#pragma once

#include <windows.h>
#include <gl/GL.h>

namespace spike {

// Creates a hidden top-level window owning a WGL context and makes it current,
// then resolves the modern-GL function pointers. Returns false on failure.
bool GlContextCreate(HINSTANCE hinst, HWND parent);
void GlContextDestroy();
bool GlMakeCurrent();

}  // namespace spike

// ---- Types and constants missing from the GL 1.1 <gl/GL.h> ------------------

typedef char     GLchar;
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
#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_CLAMP_TO_EDGE          0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                  0x8058
#endif

// ---- Modern-GL function pointers (defined in spike_gl.cpp) -------------------

#define SPIKE_GL_FUNCS(X) \
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
    X(void,   glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindFramebuffer, (GLenum, GLuint)) \
    X(void,   glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void,   glGenRenderbuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteRenderbuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindRenderbuffer, (GLenum, GLuint)) \
    X(void,   glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,   glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))

#define SPIKE_GL_DECL(ret, name, args) typedef ret (APIENTRY* PFN_##name) args; extern PFN_##name spike_##name;
SPIKE_GL_FUNCS(SPIKE_GL_DECL)
#undef SPIKE_GL_DECL

// Route the modern names (absent from GL 1.1) to the resolved pointers.
#define glCreateShader            spike_glCreateShader
#define glShaderSource            spike_glShaderSource
#define glCompileShader           spike_glCompileShader
#define glGetShaderiv             spike_glGetShaderiv
#define glGetShaderInfoLog        spike_glGetShaderInfoLog
#define glCreateProgram           spike_glCreateProgram
#define glAttachShader            spike_glAttachShader
#define glLinkProgram             spike_glLinkProgram
#define glGetProgramiv            spike_glGetProgramiv
#define glGetProgramInfoLog       spike_glGetProgramInfoLog
#define glDeleteShader            spike_glDeleteShader
#define glDeleteProgram           spike_glDeleteProgram
#define glUseProgram              spike_glUseProgram
#define glGetUniformLocation      spike_glGetUniformLocation
#define glUniformMatrix4fv        spike_glUniformMatrix4fv
#define glUniform1i               spike_glUniform1i
#define glGenBuffers              spike_glGenBuffers
#define glDeleteBuffers           spike_glDeleteBuffers
#define glBindBuffer              spike_glBindBuffer
#define glBufferData              spike_glBufferData
#define glGenVertexArrays         spike_glGenVertexArrays
#define glDeleteVertexArrays      spike_glDeleteVertexArrays
#define glBindVertexArray         spike_glBindVertexArray
#define glEnableVertexAttribArray spike_glEnableVertexAttribArray
#define glVertexAttribPointer     spike_glVertexAttribPointer
#define glVertexAttribIPointer    spike_glVertexAttribIPointer
#define glGenFramebuffers         spike_glGenFramebuffers
#define glDeleteFramebuffers      spike_glDeleteFramebuffers
#define glBindFramebuffer         spike_glBindFramebuffer
#define glFramebufferTexture2D    spike_glFramebufferTexture2D
#define glGenRenderbuffers        spike_glGenRenderbuffers
#define glDeleteRenderbuffers     spike_glDeleteRenderbuffers
#define glBindRenderbuffer        spike_glBindRenderbuffer
#define glRenderbufferStorage     spike_glRenderbufferStorage
#define glFramebufferRenderbuffer spike_glFramebufferRenderbuffer
#define glCheckFramebufferStatus  spike_glCheckFramebufferStatus
