// SPDX-License-Identifier: MIT
//
// Move-only RAII wrappers for the raw-GL objects the renderer owns (D2/AR8).
// Each handle frees itself on destruction; copying is forbidden so a GL object
// has exactly one owner. The architecture tree places this at "Phase 1" wrapping
// sg_*_t; the raw-GL decision (Spec Change Log 2026-06-23) pulls it to Phase 0.5
// and re-bases the wrappers on GLuint.
//
// NOTE: the destructors call glDelete*, which require a *current* GL context.
// The renderer must release/shutdown its handles while its context is current.

#pragma once

#include "gl_loader.h"

#ifdef _WIN32

namespace rav {

// One move-only RAII owner for a GLuint handle, parameterized on the GL delete
// call. The three GL object kinds differ ONLY in that delete, so they share this
// body — a change to the ownership contract is made once, here.
template <class Deleter>
class GpuHandle {
public:
    GpuHandle() = default;
    explicit GpuHandle(GLuint h) noexcept : h_(h) {}
    GpuHandle(GpuHandle&& o) noexcept : h_(o.h_) { o.h_ = 0; }
    GpuHandle& operator=(GpuHandle&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = 0; }
        return *this;
    }
    GpuHandle(const GpuHandle&) = delete;
    GpuHandle& operator=(const GpuHandle&) = delete;
    ~GpuHandle() { reset(); }

    GLuint get() const noexcept { return h_; }
    GLuint* addr() noexcept { return &h_; }  // for glGen*(1, h.addr())

private:
    void reset() noexcept { if (h_) { Deleter{}(h_); h_ = 0; } }
    GLuint h_ = 0;
};

struct BufferDeleter      { void operator()(GLuint h) const noexcept { glDeleteBuffers(1, &h); } };
struct VertexArrayDeleter { void operator()(GLuint h) const noexcept { glDeleteVertexArrays(1, &h); } };
struct ProgramDeleter     { void operator()(GLuint h) const noexcept { glDeleteProgram(h); } };

using GpuBuffer      = GpuHandle<BufferDeleter>;       // VBO / IBO
using GpuVertexArray = GpuHandle<VertexArrayDeleter>;  // VAO
using GpuProgram     = GpuHandle<ProgramDeleter>;      // linked shader program

}  // namespace rav

#endif  // _WIN32
