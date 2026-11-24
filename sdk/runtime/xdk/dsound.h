// mechassault-recomp patch: what a title can ask of the DirectSound mixer (xdk/dsound.cpp)
#pragma once

namespace dsound {
// Between LetFinish(true) and LetFinish(false) a stream flushed while it plays keeps the rest of what it is saying, up
// to its next pause (160 ms under -26 dBFS, at most what is queued): that audio is copied out of the title's packets,
// which complete as flushed, and plays ahead of whatever the title queues next, so the next line waits for it rather
// than overlapping it. MechAssault's radio lines (src/game/voice.cpp).
void LetFinish(bool on);
}  // namespace dsound
