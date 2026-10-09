// SPDX-License-Identifier: MIT
//
// Linux stand-in for <gl/GL.h> (GL 1.1). The dump tool never uploads to a GPU: these
// exist only so asset_loader.cpp compiles; every call is a no-op that reports failure.
#pragma once

#include <cstddef>

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef float          GLfloat;
typedef unsigned int   GLbitfield;
typedef unsigned char  GLubyte;
typedef void           GLvoid;

#define GL_NO_ERROR             0
#define GL_INVALID_OPERATION    0x0502
#define GL_TEXTURE_2D           0x0DE1
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_REPEAT               0x2901
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_LINEAR               0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_RGBA8                0x8058

inline void   glGenTextures(GLsizei n, GLuint* t) { for (GLsizei i = 0; i < n; ++i) t[i] = 0; }
inline void   glDeleteTextures(GLsizei, const GLuint*) {}
inline void   glBindTexture(GLenum, GLuint) {}
inline void   glPixelStorei(GLenum, GLint) {}
inline void   glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {}
inline void   glTexParameteri(GLenum, GLenum, GLint) {}
inline GLenum glGetError() { return GL_NO_ERROR; }
