// SPDX-License-Identifier: MIT
//
// OpenGL on REAPER's video thread (Epic 11). The viewer's context lives on the UI
// thread and must never be touched from here, so each thread that renders video
// frames gets its OWN hidden 1x1 window + WGL context, created on first use and kept
// for the life of the thread (spike 11-0, findings 3 and 8: one video thread per
// session, creation ~9 ms once). A frame renders into an offscreen FBO (colour RGBA8
// + depth 24) of the frame's size and is read back into REAPER's frame buffer.
//
// The FBO entry points are loaded into a per-thread table here; this file never uses
// the viewer's process-global GL loader (gl_loader.h), whose pointers belong to the
// UI thread's context. Never throws (AR18).

#pragma once

namespace rav {

// Scope of one video frame on the calling thread:
//
//   VideoGlFrame gl;
//   if (gl.Begin(w, h)) { ...draw into the bound FBO, viewport set... gl.ReadInto(px, rb); }
//
// Begin creates this thread's context on first use, makes it current (saving the
// context that was current before), and binds a w x h target FBO. The destructor
// unbinds the FBO and restores the previous context, whatever happened.
class VideoGlFrame {
public:
    VideoGlFrame() = default;
    ~VideoGlFrame();
    VideoGlFrame(const VideoGlFrame&) = delete;
    VideoGlFrame& operator=(const VideoGlFrame&) = delete;

    // False when this thread has no usable GL (logged once per thread); the caller
    // then produces the frame without GL.
    bool Begin(int width, int height);

    // The bound target (non-zero after a successful Begin).
    unsigned Fbo() const;

    // glFinish + read the target back as B,G,R,A bytes, top row first, into `pixels`
    // (`row_bytes` apart, >= width * 4). False on a GL error.
    bool ReadInto(unsigned char* pixels, int row_bytes);

private:
    bool  active_  = false;
    void* prev_dc_ = nullptr;
    void* prev_rc_ = nullptr;
    int   width_   = 0;
    int   height_  = 0;
};

}  // namespace rav
