// SPDX-License-Identifier: MIT
//
// The video FX test pattern (Story 11-1), a sync check that can be decoded from a
// rendered video (tools/video-gate/decode_stripes.bat):
//   - orange background (255,128,0): azure blue means red and blue are swapped;
//   - a dark vertical bar sliding left to right once every 4 s of PROJECT time;
//   - at the top, on a grey band 1/8 of the height, 16 equal cells holding the frame
//     index in binary, most significant bit on the left, white = 1, black = 0.
// Everything is a pure function of (project_time, frame rate, size): the same frame
// requested twice, in any order, gives the same pixels (spike 11-0, finding 7).

#pragma once

namespace rav {

// round(project_time x frame_rate); 0 when either is <= 0. The pattern shows its low
// 16 bits.
long long VideoFrameIndex(double project_time, double frame_rate);

// Draws the pattern with OpenGL into the currently bound framebuffer (w x h, viewport
// set). Uses only GL 1.1 scissored clears; leaves scissor test disabled.
void DrawTestPatternGl(int width, int height, double project_time, long long frame_index);

// Same pattern written directly into a B,G,R,A top-down buffer (no GL).
void DrawTestPatternCpu(unsigned char* pixels, int width, int height, int row_bytes, double project_time,
                        long long frame_index);

}  // namespace rav
