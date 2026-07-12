#pragma once

#include <Arduino.h>
#include <SD.h>

#include "sf22aswt_reader_base.h"
#include "sf22aswt_structures.h"
#include "sf22aswt_enums.h"
#include "sf22aswt_helpers.h"
#include "sf22aswt_converter.h"

namespace SF22ASWT
{
    class ReaderLazy : public SF22ASWT::ReaderBase
    {
      public:
        sfbk_rec_lazy sfbk;

        bool CloneInto(ReaderLazy &other);
        /** reads and verifies the sf2 file,
         *  note. this is lazy read 
         *  and only the file data position for
         *  all used blocks are stored into ram
         */
        bool ReadFile(const char * filePath);
        bool ProcessInstrumentList(void (*callback)(SF22ASWT::inst_rec& inst, void* params), void* params);
        bool PrintInstrumentListAsJson(Print &printStream);
        bool PrintPresetListAsJson(Print &printStream);
        /**
         * this function do only load the sample preset headers for the instrument (soundfont igen data)
         * to load the actual sample data the function <instance name>::ReadSampleDataFromFile should be used
        */
        bool Load_instrument_data(uint index, SF22ASWT::instrument_data_temp &inst);
        /**
         * this function is like Load_instrument_data but also loads the sample data 
         * the output is AudioSynthWavetable::instrument_data
         * note that errPrintStream is default to Serial which can be changed into any Print Stream
        */
        bool Load_instrument(int instrumentIndex, AudioSynthWavetable::instrument_data*& aswt_id, Print &errPrintStream = Serial);
        /**
         * this is mostly intended as a demo or to quickly use this library
         * note that errPrintStream is default to Serial which can be changed into any Print Stream
        */
        bool Load_instrument_from_file(const char * filePath, int instrumentIndex, AudioSynthWavetable::instrument_data **aswt_id, Print &errPrintStream = Serial);
        bool PrintInfoBlock(Print &printStream);

  private:
        bool read_pdta_block(File &file, pdta_rec_lazy &pdta);
        bool fillBagsOfGens(File &file, bag_of_gens* bags, int ibag_startIndex, int ibag_count);
        
    };


};