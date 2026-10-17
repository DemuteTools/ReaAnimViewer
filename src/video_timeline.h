// SPDX-License-Identifier: MIT
//
// What the video FX must draw, as the video thread may read it (Story 11-2).
//
// REAPER's item and track APIs belong to the main thread: reading them from the video
// thread while the user deletes an item could crash. So a main-thread timer builds an
// immutable SNAPSHOT of every track that carries the RAV video FX: its RAV animation
// items (position, length, take start offset, file), the project's video size and the
// track's background option. The video thread only reads the latest snapshot, by the
// MediaTrack* REAPER gives the FX (used as a key, never dereferenced there).
//
// The snapshot is rebuilt when a project's state-change count moves (any undoable
// edit), when an option changes, and at least once a second (edits that create no undo
// point). It also tells the shared asset cache which files are in use.

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>
#include <vector>

#include "play_item_plan.h"

class MediaTrack;
class ReaProject;

namespace rav {

struct VideoItemSpan {
    double      start = 0.0;  // D_POSITION
    double      end   = 0.0;  // D_POSITION + D_LENGTH (exclusive)
    std::string path;         // the animation file
};

struct VideoTrackTimeline {
    MediaTrack* track = nullptr;
    int  video_width  = 0;          // project video size; 0 = not set (use the default)
    int  video_height = 0;
    bool transparent  = false;      // background option (P_EXT, saved with the project)
    std::vector<VideoItemSpan> items;  // in the track's item order (muted ones too)
    // Story 11-7: items[i] as a playback candidate (offset, rate, loop, mute, lane), same
    // order, for PickPlayingItem / PlayClipTime (play_item_plan.h).
    std::vector<PlayCandidate> plays;
};

struct VideoTimelineSnapshot {
    std::vector<VideoTrackTimeline> tracks;
    const VideoTrackTimeline* Find(const MediaTrack* track) const;
};

// The item a frame shows: on `track`, the item REAPER plays at t — the viewer's rule
// (Story 11-7, PickPlayingItem: not muted, on a playing lane, the upper lane, then the
// later start) — and its clip time (PlayClipTime: offset, play rate, loop).
struct VideoItemHit {
    const VideoTrackTimeline* track = nullptr;
    const VideoItemSpan*      item  = nullptr;
    double                    anim_time = 0.0;
};
bool FindVideoItemAt(const VideoTimelineSnapshot& snap, const MediaTrack* track, double t,
                     VideoItemHit& out);

// Main thread: the project's video size (Project Settings > Video), 0 x 0 when not set
// (proj nullptr = the current project). Story 11-4: Video view frames at this size.
void ProjectVideoSizeOf(ReaProject* proj, int* w, int* h);

// Any thread: the latest snapshot (never null).
std::shared_ptr<const VideoTimelineSnapshot> CurrentVideoTimeline();

// Main thread: REAPER "timer" callback. Rebuilds the snapshot when needed. No-throw.
void VideoTimelineTick();

// Unload: drop the snapshot.
void ClearVideoTimeline();

// ---- Background option (UX decision 6: viewer background, or transparent) ----------
// Per track, saved with the project (P_EXT:RAV_VIDEO_BG). Main thread.
bool VideoBackgroundTransparent(MediaTrack* track);
void SetVideoBackgroundTransparent(MediaTrack* track, bool transparent);

// Action "RAV: Video FX - Transparent background (selected tracks)": turns the option on
// for every selected track when one of them is off, else turns it off for all; one undo
// point; a message box when no track is selected.
void ToggleVideoBackgroundOnSelectedTracks();
// Its toggle state: 1 when tracks are selected and all are transparent, else 0.
int VideoBackgroundToggleState();

}  // namespace rav

#endif  // _WIN32
