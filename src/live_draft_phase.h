// SPDX-License-Identifier: MIT
//
// 13-53 (Antho 2026-10-14): which a Sound's panel draws while a drag edits it, the live draft or the
// store. Released, a dragged edge / fade / cut / sync point / dB / threshold jumped back to where it
// was grabbed for a frame, then landed where it was let go: the draft ended at the top of the release
// frame (no button held any more) before the waveform sent its "done", and a commit ended it at once
// while its write is only picked (it runs once the panel is drawn), so whatever the frame drew after
// it read the old store.
//
// The rule kept here: a draft stays drawn on the frame its button is released (the gesture commits it
// then), and a committed draft stays drawn until its write has run (Written with the commit's token);
// it ends at once when cancelled. Pure, no ImGui, no REAPER (tests/live_draft_phase_test).

#pragma once

#include <string>

namespace rav {

class LiveDraftPhase {
public:
    // The draft of `uid` is what its panel draws.
    bool Shows(const std::string& uid) const { return live_ && uid_ == uid; }
    bool Live() const { return live_; }
    bool Committing() const { return live_ && committing_; }
    const std::string& Uid() const { return uid_; }

    // Starts or continues the draft of `uid`; true when it starts (the caller snapshots the store).
    // An edit after a commit in the same frame restarts the draft (true: snapshot again, from the committed
    // draft), which no longer waits for that write.
    bool Touch(const std::string& uid)
    {
        if (live_ && uid_ == uid) {
            if (!committing_) return false;
            // Committed this frame: the store now holds (or will hold) the draft, so the next edit
            // starts over from it -- the caller re-snapshots.
            committing_ = false;
            ++token_;
            return true;
        }
        live_ = true;
        uid_ = uid;
        committing_ = false;
        ++token_;
        return true;
    }

    // The draft of `uid` is being written (its write picked): kept drawn until Written(token).
    // 0 when there is nothing to commit (no draft of `uid`, or its commit is already picked).
    unsigned Commit(const std::string& uid)
    {
        if (!live_ || uid_ != uid || committing_) return 0;
        committing_ = true;
        if (++token_ == 0) ++token_;
        return token_;
    }

    // The commit `token`'s write has run (whether or not it changed the store): the store holds it.
    void Written(unsigned token)
    {
        if (live_ && committing_ && token != 0 && token == token_) {
            live_ = false;
            committing_ = false;
        }
    }

    void Cancel()
    {
        live_ = false;
        committing_ = false;
        ++token_;
    }

    // At the top of a frame: the draft ends unless something still holds it -- the button down, or
    // released this very frame (its gesture's "done" comes as the waveform draws), a number field
    // dragged or a fader typed into. A commit waiting for its write is not ended here.
    bool EndsAtFrameStart(bool button_down, bool button_released, bool number_active, bool fader_typing) const
    {
        return live_ && !committing_ && !button_down && !button_released && !number_active && !fader_typing;
    }

private:
    bool        live_ = false;
    bool        committing_ = false;
    unsigned    token_ = 0;
    std::string uid_;
};

}  // namespace rav
