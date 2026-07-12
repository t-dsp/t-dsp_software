#pragma once

#include <Arduino.h>
#include <SD.h>
#include "sf22aswt_enums.h"
#include "sf22aswt_error_enums.h"
#include "sf22aswt_structures.h"
#include "sf22aswt_helpers.h"

#ifndef USerial
#define USerial SerialUSB
#endif

//#define SF22ASWT_DEBUG

#ifdef SF22ASWT_DEBUG
#ifndef USerial
  #define USerial SerialUSB1
#endif
  #define DebugPrint(args) USerial.print(args);
  #define DebugPrintln(args) USerial.println(args);
  #define DebugPrint_Text_Var(text, var) USerial.print(text); USerial.print(var);
  #define DebugPrintln_Text_Var(text, var) USerial.print(text); USerial.println(var);
  #define DebugPrintFOURCC(fourCC) USerial.print(">>>"); Helpers::printRawBytes(USerial, fourCC, 4); USerial.println("<<<");
  #define DebugPrintFOURCC_size(size) USerial.print("size: "); USerial.print(size);  USerial.print("\n");
#else
  #define DebugPrint(args)
  #define DebugPrintln(args)
  #define DebugPrint_Text_Var(text, var)
  #define DebugPrintln_Text_Var(text, var)
  #define DebugPrintFOURCC(fourCC)
  #define DebugPrintFOURCC_size(size)
#endif

#define FILE_ERROR(ERROR_TYPE) {lastError=SF22ASWT::Errors::ERROR_TYPE; lastErrorPosition = file.position() - lastReadCount; file.close(); return false;}
#define FILE_SEEK_ERROR(ERROR_TYPE, SEEK_POS) {lastError=SF22ASWT::Errors::ERROR_TYPE; lastErrorPosition = file.position(); lastReadCount = SEEK_POS; file.close(); return false; }
#define FILE_ERROR_APPEND_SUB(ROOT_TYPE, SUB_TYPE) lastError = (SF22ASWT::Errors)((uint16_t)lastError & (uint16_t)SF22ASWT::Error::ROOT_TYPE::SUB_TYPE);
        

namespace SF22ASWT
{
    extern int Samples_Max_Internal_RAM_Cap;
    /**
     * keeping track of all used ram, 
     * have it global as multiple files can be loaded, 
     * and for future use when multiple instruments can be loaded at once
    */
    extern int samples_usedRam; // 
    /**
     * this class is only intended for inherited use
     * and contains the 'common' stuff that is used on both lazy reader and 'normal' reader
    */
    class ReaderBase {
      public:
        SF22ASWT::Errors getLastError();
#ifdef SF22ASWT_DEBUG
        String getLastErrorStr();
#endif
        uint32_t getLastErrorPosition();
        size_t getLastReadCount();
        bool getLastReadWasOK();
        int getTotalSampleDataSizeBytes();
        uint32_t getFileSize();
        //#define FILE_ERROR(msg) {lastError=msg; lastErrorPosition = file.position() - lastReadCount; file.close(); return false;}

        void printSF2ErrorInfo(Print &print);
        bool ReadSampleDataFromFile(instrument_data_temp &inst, bool forceUseInternalRam = false);

      protected:
        ReaderBase() {}
        ~ReaderBase() {}

        void clearErrors();
#ifdef SF22ASWT_DEBUG
        String lastErrorStr; // used to provide additional info if needed
#endif
        SF22ASWT::Errors lastError = SF22ASWT::Errors::NONE;
        uint32_t lastErrorPosition;
        size_t lastReadCount = 0; // used to track errors

        uint32_t fileSize;
        String filePath;
        
        bool lastReadWasOK = false;

        // TODO make all samples load into a single array for easier allocation / deallocation
        // also maybe have it as a own contained memory pool
        sample_data *samples;
        bool samples_useExtMem = false;
        int sample_count = 0;
        int totalSampleDataSizeBytes = 0;

        bool ReadStringUsingLeadingSize(File &file, String& string);
        bool verifyFourCC(const char* fourCC);

        bool readInfoBlock(File &file, INFO &info);
        /// <summary>
        /// reads data offset pointers and sizes of sample data, not the actual data
        /// as the data is read from file on demand
        /// </summary>
        /// <param name="br"></param>
        /// <returns></returns>
        bool read_sdta_block(File &file, sdta_rec_lazy &sdta);

        void FreePrevSampleData();

// #pragma region gen_get_functions
        bool get_parameter_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, SF2GeneratorAmount *amount);
        float get_decibel_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN, float MAX);
        float get_timecents_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN);
        float get_hertz(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN, float MAX);
        int get_pitch_cents(bag_of_gens* bags, int sampleIndex, SFGenerator genType, int DEFAULT, int MIN, int MAX);
        int get_cooked_loop_start(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr);
        int get_cooked_loop_end(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr);
        int get_sample_note(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr);
        int get_fine_tuning(bag_of_gens* bags, int sampleIndex);
        bool get_sample_header(File &file, sfbk_rec_lazy &sfbk, bag_of_gens* bags, int sampleIndex, shdr_rec *shdr);
        bool get_sample_repeat(bag_of_gens* bags, int sampleIndex, bool defaultValue);
        int get_length(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr);
        int get_key_range_end(bag_of_gens* bags, int sampleIndex);
        int get_length_bits(int len);
// #pragma endregion
        void DebugPrintBagContents(bag_of_gens &gen);
    };
};