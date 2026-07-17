
#include "sf22aswt_reader_base.h"

namespace SF22ASWT
{
    int Samples_Max_Internal_RAM_Cap = 400000;

    extern "C" uint8_t external_psram_size;
    int samples_usedRam = 0;
#ifdef SF22ASWT_DEBUG
    String ReaderBase::getLastErrorStr() { return lastErrorStr; }
#endif
    SF22ASWT::Errors ReaderBase::getLastError() { return lastError; }
    uint32_t ReaderBase::getLastErrorPosition() { return lastErrorPosition; }
    size_t ReaderBase::getLastReadCount() { return lastReadCount; }
    bool ReaderBase::getLastReadWasOK() { return lastReadWasOK; }
    int ReaderBase::getTotalSampleDataSizeBytes() { return totalSampleDataSizeBytes; }
    uint32_t ReaderBase::getFileSize() { return fileSize; }

    CODE_LOCATION void ReaderBase::clearErrors()
    {
#ifdef SF22ASWT_DEBUG
        lastErrorStr = "";
#endif
        lastError = SF22ASWT::Errors::NONE;
        lastErrorPosition = 0;
        lastReadCount = 0;
    }

    CODE_LOCATION void ReaderBase::printSF2ErrorInfo(Print &printStream)
    {
        SF22ASWT::printError(printStream, lastError); printStream.print("\n");
#ifdef SF22ASWT_DEBUG
        printStream.println(lastErrorStr);
#endif
        printStream.print(" @ position: ");
        printStream.print(lastErrorPosition);
        printStream.print(", lastReadCount: ");
        printStream.print(lastReadCount); printStream.print("\n");
    }

    CODE_LOCATION bool ReaderBase::ReadStringUsingLeadingSize(File &file, String& string)
    {
        uint32_t size = 0;
        if ((lastReadCount = file.read(&size, 4)) != 4) FILE_ERROR(INFO_STRING_SIZE_READ) //FILE_ERROR("read error - while getting infoblock string size")
        char bytes[size];
        if ((lastReadCount = file.readBytes(bytes, size)) != size) FILE_ERROR(INFO_STRING_DATA_READ) //FILE_ERROR("read error - while reading infoblock string")
        
        // TODO. sanitize bytes
        string = String(bytes);

        return true;
    }

    CODE_LOCATION bool ReaderBase::verifyFourCC(const char* fourCC)
    {
        for (int i=0;i<4;i++)
            if (fourCC[i] < 32 || fourCC[i] > 126) return false;
        return true;
    }

    CODE_LOCATION bool ReaderBase::readInfoBlock(File &file, INFO &info)
    {
        char fourCC[4];
        while (file.available() > 0)
        {
            if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(INFO_FOURCC_READ) //FILE_ERROR("read error - while getting infoblock type")
            DebugPrintFOURCC(fourCC);
            if (verifyFourCC(fourCC) == false) FILE_ERROR(INFO_FOURCC_INVALID) //FILE_ERROR("error - infoblock type invalid")

            uint32_t dummy = 0;
            if (strncmp(fourCC, "ifil", 4) == 0) {
                if ((lastReadCount = file.read(&dummy, 4)) != 4) FILE_ERROR(INFO_IFIL_SIZE_READ) //FILE_ERROR("read error - ifil size dummy read")
                if ((lastReadCount = file.read(&info.ifil, 4)) != 4) FILE_ERROR(INFO_IFIL_DATA_READ) //FILE_ERROR("read error - while ifil read")
            }
            else if (strncmp(fourCC, "isng", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.isng) == false){ FILE_ERROR_APPEND_SUB(INFO, ISNG) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "INAM", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.INAM) == false){ FILE_ERROR_APPEND_SUB(INFO, INAM) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "irom", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.irom) == false){ FILE_ERROR_APPEND_SUB(INFO, IROM) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "iver", 4) == 0) {
                if ((lastReadCount = file.read(&dummy, 4)) != 4) FILE_ERROR(INFO_IVER_SIZE_READ) //FILE_ERROR("read error - iver size dummy read")
                if ((lastReadCount = file.read(&info.iver, 4)) != 4) FILE_ERROR(INFO_IVER_DATA_READ) //FILE_ERROR("read error - while iver read")
            }
            else if (strncmp(fourCC, "ICRD", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.ICRD) == false){ FILE_ERROR_APPEND_SUB(INFO, ICRD) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "IENG", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.IENG) == false){ FILE_ERROR_APPEND_SUB(INFO, IENG) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "IPRD", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.IPRD) == false){ FILE_ERROR_APPEND_SUB(INFO, IPRD) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "ICOP", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.ICOP) == false){ FILE_ERROR_APPEND_SUB(INFO, ICOP) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "ICMT", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.ICMT) == false){ FILE_ERROR_APPEND_SUB(INFO, ICMT) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "ISFT", 4) == 0) { if (ReadStringUsingLeadingSize(file, info.ISFT) == false){ FILE_ERROR_APPEND_SUB(INFO, ISFT) return false; }}// ReadStringUsingLeadingSize takes care of the error report
            else if (strncmp(fourCC, "LIST", 4) == 0)
            {
                // skip back
                if (file.seek(-4, SeekCur) == false) FILE_SEEK_ERROR(INFO_BACK_SEEK, -4)
                return true;
            }
            else
            {
                // normally unknown blocks should be ignored
                if ((lastReadCount = file.read(&dummy, 4)) != 4) FILE_ERROR(INFO_UNKNOWN_BLOCK_SIZE_READ) //FILE_ERROR("read error - while getting unknown INFO block size")
                if (file.seek(file.position() + dummy) == false) FILE_SEEK_ERROR(INFO_UNKNOWN_BLOCK_DATA_SKIP, file.position() + dummy) //FILE_ERROR("seek error - while skipping unknown INFO block")
            }
        }
        return true;
    }

    CODE_LOCATION bool ReaderBase::read_sdta_block(File &file, sdta_rec_lazy &sdta)
    {
        char fourCC[4];
        while (file.available())
        {
            if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(SDTA_FOURCC_READ) //("read error - while getting sdtablock type")
            DebugPrintFOURCC(fourCC);
            if (verifyFourCC(fourCC) == false) FILE_ERROR(SDTA_FOURCC_INVALID) //("error - sdtablock type invalid")
            
            if (strncmp(fourCC, "smpl", 4) == 0)
            {
                if ((lastReadCount = file.read(&sdta.smpl.size, 4)) != 4) FILE_ERROR(SDTA_SMPL_SIZE_READ) //("read error - while reading smpl size")
                sdta.smpl.position = file.position();
                // skip sample data
                if (file.seek(sdta.smpl.size, SeekCur) == false) FILE_SEEK_ERROR(SDTA_SMPL_DATA_SKIP, sdta.smpl.size) //("seek error - while skipping smpl data")
            }
            else if (strncmp(fourCC, "sm24", 4) == 0)
            {
                if ((lastReadCount = file.read(&sdta.sm24.size, 4)) != 4) FILE_ERROR(SDTA_SM24_SIZE_READ) //("read error - while reading sm24 size")
                sdta.sm24.position = file.position();
                // skip sample data
                if (file.seek(sdta.sm24.size, SeekCur) == false) FILE_SEEK_ERROR(SDTA_SM24_DATA_SKIP, sdta.sm24.size) //("seek error - while skipping sm24 data")
            }
            else if (strncmp(fourCC, "LIST", 4) == 0)
            {
                // skip back
                if (file.seek(file.position()-4) == false) FILE_SEEK_ERROR(SDTA_BACK_SEEK, -4)
                return true;
            }
            else
            {
                // normally unknown blocks should be ignored
                uint32_t size = 0;
                if ((lastReadCount = file.read(&size, 4)) != 4) FILE_ERROR(SDTA_UNKNOWN_BLOCK_SIZE_READ) //("read error - while getting unknown sdta block size")
                if (file.seek(size, SeekCur) == false) FILE_SEEK_ERROR(SDTA_UNKNOWN_BLOCK_DATA_SKIP, size) //("seek error - while skipping unknown sdta block")
            }
        }
        return true;
    }

    CODE_LOCATION void ReaderBase::FreePrevSampleData()
    {
        DebugPrintln("try to free prev loaded sampledata");
        for (int i = 0;i<sample_count;i++)
        {
            if (samples[i].data != nullptr) {
                DebugPrintln("freeing " + String(i) + " @ " + String((uint64_t)samples[i].data));
                if (samples_useExtMem == false)
                    free(samples[i].data);
                else
                    extmem_free(samples[i].data);

                samples_usedRam -= samples[i].dataSize;
            }
        }
        delete[] samples;
        DebugPrintln("[OK]");
        samples = nullptr;
    }

    CODE_LOCATION bool ReaderBase::ReadSampleDataFromFile(instrument_data_temp &inst, bool forceUseInternalRam)
    {
        clearErrors();
        if (lastReadWasOK == false) { lastError = SF22ASWT::Errors::FILE_NOT_OPEN; return false; }
        
        if (samples != nullptr) {
            FreePrevSampleData();
        } 
        // first calculate totalSampleDataSizeBytes as an early check to minimize unnecessary loading
        totalSampleDataSizeBytes = 0;
        for (int si=0;si<inst.sample_count;si++)
        {
            int length_32 = (int)std::ceil((double)inst.samples[si].LENGTH / 2.0f);
            int pad_length = (length_32 % 128 == 0) ? 0 : (128 - length_32 % 128);
            int ary_length = length_32 + pad_length;
            totalSampleDataSizeBytes+=ary_length*4;
        }
        samples_useExtMem = (external_psram_size != 0) && (forceUseInternalRam == false);
        
        // early check for available ram
        if (samples_useExtMem == false) {
            if (totalSampleDataSizeBytes > (SF22ASWT::Samples_Max_Internal_RAM_Cap - samples_usedRam)) {
                lastError = SF22ASWT::Errors::RAM_SIZE_INSUFF;
                return false;
            }

        }
        else {
            if (totalSampleDataSizeBytes > ((external_psram_size * 1024 * 1024) - samples_usedRam)) {
                lastError = SF22ASWT::Errors::EXTRAM_SIZE_INSUFF;
                return false;
            }
        }

        samples = new sample_data[inst.sample_count];
        sample_count = inst.sample_count;
        int allocatedSize = 0;
#ifdef SF22ASWT_DEBUG
        if (samples_useExtMem)
            USerial.println("using external ram (PSRAM)");
#endif

        File file = SD.open(filePath.c_str());
        if (!file) { lastError = SF22ASWT::Errors::FILE_NOT_OPEN; return false; } // extra failsafe

        for (int si=0;si<inst.sample_count;si++)
        {
            DebugPrintln_Text_Var("reading sample: ", si);
            int length_32 = (int)std::ceil((double)inst.samples[si].LENGTH / 2.0f);
            size_t length_8 = length_32*4;
            int pad_length = (length_32 % 128 == 0) ? 0 : (128 - length_32 % 128);
            int ary_length = length_32 + pad_length;
            int ary_length_8 = ary_length*4;

            if (samples_useExtMem == false) { // use internal ram
                samples[si].data = (uint32_t*)malloc(ary_length_8);
            }
            else {
                samples[si].data = (uint32_t*)extmem_malloc(ary_length_8);
            }

            if (samples[si].data == nullptr) {
                lastError = SF22ASWT::Errors::RAM_DATA_MALLOC;
#ifdef SF22ASWT_DEBUG
                lastErrorStr = "@ sample " + String(si) + " could not allocate additional " + String(ary_length_8) + " bytes, allocated " + String(allocatedSize*4) + " of " + String(totalSampleDataSizeBytes) + " bytes";
#endif
                file.close();
                FreePrevSampleData();
                return false;
            }
            samples[si].dataSize = ary_length_8;
            samples_usedRam += ary_length_8;

            if (file.seek(inst.samples[si].sample_start) == false) {
                //lastError = "@ sample " +  String(si) + " could not seek to data location in file";
                lastError = SF22ASWT::Errors::SDTA_SMPL_DATA_SEEK;
                lastErrorPosition = file.position();
                lastReadCount = inst.samples[si].sample_start;
                file.close();
                FreePrevSampleData();
                return false;
            }
            if ((lastReadCount = file.readBytes((char*)samples[si].data, length_8)) != length_8) {
                //lastError = "@ sample " +  String(si) + " could not read sample data from file, wanted:" + length_8 + " but could only read " + lastReadCount;
                lastError = SF22ASWT::Errors::SDTA_SMPL_DATA_READ;
                lastErrorPosition = inst.samples[si].sample_start;
                file.close();
                FreePrevSampleData();
                return false;
            }
            for (int i = length_32; i < ary_length;i++)
            {
                samples[si].data[i] = 0x00000000;
            }
            inst.samples[si].sample = (int16_t*)samples[si].data;
            allocatedSize+=ary_length;
        }
#ifdef SF22ASWT_DEBUG
        USerial.print("Used ram for samples:"); USerial.println(samples_usedRam);
#endif
        file.close();
        return true;
    }

// #pragma region gen_get
CODE_LOCATION bool ReaderBase::get_parameter_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, SF2GeneratorAmount *amount)
    {
        bool globalExists = (bags[0].count != 0)?(bags[0].lastItem().sfGenOper != SFGenerator::sampleID):true;
        int bagIndex = globalExists?(sampleIndex+1):sampleIndex;

        uint16_t sampleGenCount = bags[bagIndex].count;
        for (int i=0;i<sampleGenCount;i++)
        {
            if (bags[bagIndex].items[i].sfGenOper == genType) {
                *amount = bags[bagIndex].items[i].genAmount;
                return true;
            }
        }
        if (globalExists == false) return false;
        // try again with global bag
        uint16_t globalGenCount = bags[0].count;
        for (int i = 0; i < globalGenCount; i++)
        {
            if (bags[0].items[i].sfGenOper == genType) {
                *amount = bags[0].items[i].genAmount;
                return true;
            }
        }
        return false;
    }


    CODE_LOCATION float ReaderBase::get_decibel_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN, float MAX)
    {
        SF2GeneratorAmount genval;
        float val = get_parameter_value(bags, sampleIndex, genType, &genval)?genval.centibels(): DEFAULT;
        return (val > MAX) ? MAX : ((val < MIN) ? MIN : val);
    }


    CODE_LOCATION float ReaderBase::get_timecents_value(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN)
    {
        SF2GeneratorAmount genval;
        float val = get_parameter_value(bags, sampleIndex, genType, &genval)?genval.cents()*1000.0f: DEFAULT;
        return (val > MIN) ? val : MIN;
    }


    CODE_LOCATION float ReaderBase::get_hertz(bag_of_gens* bags, int sampleIndex, SFGenerator genType, float DEFAULT, float MIN, float MAX)
    {
        SF2GeneratorAmount genval;
        float val = get_parameter_value(bags, sampleIndex, genType, &genval)?genval.absolute_cents(): DEFAULT;
        return (val > MAX) ? MAX : ((val < MIN) ? MIN : val);
    }


    CODE_LOCATION int ReaderBase::get_pitch_cents(bag_of_gens* bags, int sampleIndex, SFGenerator genType, int DEFAULT, int MIN, int MAX)
    {
        SF2GeneratorAmount genval;
        int val = get_parameter_value(bags, sampleIndex, genType, &genval)?genval.Amount: DEFAULT;
        return (val > MAX) ? MAX : ((val < MIN) ? MIN : val);
    }


    CODE_LOCATION int ReaderBase::get_cooked_loop_start(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr)
    {
        int result = (int)(shdr.dwStartloop - shdr.dwStart);
        SF2GeneratorAmount genval;
        result += get_parameter_value(bags, sampleIndex, SFGenerator::startloopAddrsOffset, &genval)?genval.Amount:0;
        result += get_parameter_value(bags, sampleIndex, SFGenerator::startloopAddrsCoarseOffset, &genval)?genval.coarse_offset():0;
        return result;
    }


    CODE_LOCATION int ReaderBase::get_cooked_loop_end(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr)
    {
        int result = (int)(shdr.dwEndloop - shdr.dwStart);
        SF2GeneratorAmount genval;
        result += get_parameter_value(bags, sampleIndex, SFGenerator::endloopAddrsOffset, &genval)?genval.Amount:0;
        result += get_parameter_value(bags, sampleIndex, SFGenerator::endloopAddrsCoarseOffset, &genval)?genval.coarse_offset():0;
        return result;
    }
    
    
    CODE_LOCATION int ReaderBase::get_sample_note(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr)
    {
        SF2GeneratorAmount genval;
        return get_parameter_value(bags, sampleIndex, SFGenerator::overridingRootKey, &genval)?genval.UAmount:((shdr.byOriginalKey <= 127)?shdr.byOriginalKey:60);
    }
    
    
    CODE_LOCATION int ReaderBase::get_fine_tuning(bag_of_gens* bags, int sampleIndex)
    {
        SF2GeneratorAmount genval;
        return get_parameter_value(bags, sampleIndex, SFGenerator::fineTune, &genval)?genval.Amount:0;
    }
    
    
    CODE_LOCATION bool ReaderBase::get_sample_header(File &file, sfbk_rec_lazy &sfbk, bag_of_gens* bags, int sampleIndex, shdr_rec *shdr)
    {
        SF2GeneratorAmount genval;
        if (get_parameter_value(bags, sampleIndex, SFGenerator::sampleID, &genval) == false) return false;
        uint32_t seekPos = sfbk.pdta.shdr_position + genval.UAmount*shdr_rec::Size;
        if (file.seek(seekPos) == false) FILE_SEEK_ERROR(PDTA_SHDR_DATA_SEEK, seekPos)
        if (file.read(shdr, shdr_rec::Size) != shdr_rec::Size) FILE_ERROR(PDTA_SHDR_DATA_READ)
        return true;
    }
    
    
    CODE_LOCATION bool ReaderBase::get_sample_repeat(bag_of_gens* bags, int sampleIndex, bool defaultValue)
    {
        SF2GeneratorAmount genVal;
        if (get_parameter_value(bags, sampleIndex, SFGenerator::sampleModes, &genVal) == false){ DebugPrintln("could not get samplemode"); return defaultValue; }
        
        return (genVal.sample_mode() == SFSampleMode::kLoopContinuously);// || (val.sample_mode == SampleMode.kLoopEndsByKeyDepression);
    }
    
    
    CODE_LOCATION int ReaderBase::get_length(bag_of_gens* bags, int sampleIndex, shdr_rec &shdr)
    {
        int length = (int)(shdr.dwEnd - shdr.dwStart);
        int cooked_loop_end_val = get_cooked_loop_end(bags, sampleIndex, shdr);
        if (get_sample_repeat(bags, sampleIndex, false) && cooked_loop_end_val < length)
        {
            return cooked_loop_end_val + 1;
        }
        return length;
    }
    
    
    CODE_LOCATION int ReaderBase::get_key_range_end(bag_of_gens* bags, int sampleIndex)
    {
        SF2GeneratorAmount genval;
        return get_parameter_value(bags, sampleIndex, SFGenerator::keyRange, &genval)?genval.rangeHigh():127;
    }
    
    
    CODE_LOCATION int ReaderBase::get_length_bits(int len)
    {
        int length_bits = 0;
        while (len != 0)
        {
            length_bits += 1;
            len = len >> 1;
        }
        return length_bits;
    }

    
    CODE_LOCATION void ReaderBase::DebugPrintBagContents(bag_of_gens &gen)
    {
        DebugPrint("bag contents:\n");
 #ifdef SF22ASWT_DEBUG
        for (int i2=0;i2<gen.count;i2++)
        {
            DebugPrint_Text_Var("  sfGenOper:", (uint16_t)gen.items[i2].sfGenOper);
            DebugPrintln_Text_Var(", value:", gen.items[i2].genAmount.UAmount);
        }
 #endif
    }
// #pragma endregion
}