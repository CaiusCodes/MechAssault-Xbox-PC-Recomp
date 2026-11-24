// Let voice lines finish (F1 > Game, on by default). MechAssault's radio lines are long tracks in
// Audio\SFX_<language>.d1Bank (275 of them, most the same length in every language: the dubs were laid on the English
// timeline), streamed in 1.49 s packets on one voice stream. The sound emitter update (Rev 1 0x19CF30, 2002 0x1838A0)
// ends a line after a whole number of packets, timed for the English recording, and deleting the emitter's entry
// (0x19CFFD..0x19D031) discontinues and flushes the stream with about 4.4 s still queued. A German line that runs longer
// loses its end there (GitHub issue #1): in Mission 11 the English line ends just before the stop and the German one
// 0.8 s after it; Mission 6's opening line is cut mid-sentence in English too. (scratch/rev1/voice_bank.py decodes the
// banks; XBE_SNDLOG=1 logs each stop, the levels after it and what was kept.)
// Two [[hook]]s (config/mechassault.toml) put that cleanup between dsound::LetFinish(true) and (false): the stream then
// keeps what it is still saying up to its next pause and plays it before the next line, which waits for it instead of
// overlapping it. A stop that falls in a pause (most English lines) changes nothing.
#include <cstdint>

#include "cpu/x86_context.h"
#include "xdk/dsound.h"

namespace mechassault {
bool g_voice_finish = true;  // settings.toml voice_finish
}

void MechAssaultVoiceEndBegin(X86Context&, uint8_t*) {
    if (mechassault::g_voice_finish) dsound::LetFinish(true);
}
void MechAssaultVoiceEndDone(X86Context&, uint8_t*) { dsound::LetFinish(false); }
