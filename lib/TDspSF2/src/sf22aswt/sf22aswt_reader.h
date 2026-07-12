/**
 * this is a soundfont 2 reader that reads 
 * the following blocks into ram:
 * info
 * pdta 
 * sdta (only store pointers to where the sample data is located in the file)
 * 
 * note. this class is not completed, and was created before the lazy reader 
 * mostly as a development guide
 * maybe it will be completed in the future to include the needed functions
 * see down 
 * 
 */

#pragma once

#include <Arduino.h>
#include <SD.h>
#include "sf22aswt_enums.h"
#include "sf22aswt_structures.h"
#include "sf22aswt_helpers.h"
#include "sf22aswt_reader_base.h"
#include "sf22aswt_converter.h"

namespace SF22ASWT
{
    class Reader : public SF22ASWT::ReaderBase
    {
      public:
        sfbk_rec sfbk;

        bool ReadFile(String filePath);

      /* TODO implement the following functions
        void printInstrumentListAsJson(Stream &stream);
        void printPresetListAsJson(Stream &stream);
        bool load_instrument_data(uint index, SF22ASWT::instrument_data_temp &inst);
        bool load_instrument_from_file(const char * filePath, int instrumentIndex, AudioSynthWavetable::instrument_data **aswt_id);
        void PrintInfoBlock(Print &stream);
      */

      private:
        bool read_pdta_block(File &br);

      /* TODO implement the following function
        bool fillBagsOfGens(bag_of_gens* bags, int ibag_startIndex, int ibag_count);
      */
    };
}