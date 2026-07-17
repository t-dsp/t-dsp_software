
#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "sf22aswt_enums.h"
#include "sf22aswt_structures.h"
#include "sf22aswt_helpers.h"
#include "sf22aswt_reader_base.h"

namespace SF22ASWT::converter
{
    AudioSynthWavetable::instrument_data to_AudioSynthWavetable_instrument_data(SF22ASWT::instrument_data_temp &data);
    SF22ASWT::sample_header toFinal(SF22ASWT::sample_header_temp &sd);
}