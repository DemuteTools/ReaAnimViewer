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
bool LoadGlFunctions(std::string& out_error);

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
    X(void,   glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))

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

#endif  // _WIN32
