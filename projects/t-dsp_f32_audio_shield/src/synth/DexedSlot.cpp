// DexedSlot.cpp — implementation of applyGain().
//
// Lives in a .cpp (not the header) because the gain stage type
// AudioEffectGain_F32 lives in OpenAudio_ArduinoLibrary, which is
// heavyweight to pull in transitively. The header forward-declares
// the type; full definition is needed only here.

#include <OpenAudio_ArduinoLibrary.h>

#include "DexedSlot.h"

namespace tdsp_synth {

void DexedSlot::applyGain() {
    const float linear =
        (_active && *_onPtr)
            ? tdsp::x32::faderToLinear(*_volX32Ptr)
            : 0.0f;
    _gain->setGain(linear);
}

}  // namespace tdsp_synth
