
#include "sf22aswt_reader.h"

namespace SF22ASWT
{
    CODE_LOCATION bool Reader::ReadFile(String filePath)
    {
        File file = SD.open(filePath.c_str());
        if (!file) { lastError = SF22ASWT::Errors::FILE_NOT_OPEN; return false; }
        fileSize = file.size();

        char fourCC[4];

        if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(FILE_FOURCC_READ)//("read error - while reading fileTag")
        if (verifyFourCC(fourCC) == false) FILE_ERROR(FILE_FOURCC_INVALID)//("error - invalid fileTag")
        if (strncmp(fourCC, "RIFF", 4) != 0) FILE_ERROR(FILE_FOURCC_MISMATCH)//("error - not a RIFF fileformat")
        
        if ((lastReadCount = file.read(&sfbk.size, 4)) != 4) FILE_ERROR(RIFF_SIZE_READ)//("read error - while reading RIFF size")
        if (fileSize != (sfbk.size + 8)) FILE_ERROR(RIFF_SIZE_MISMATCH)//("error - fileSize mismatch")

        if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(RIFF_FOURCC_READ)//("read error - while reading fileformat")
        if (verifyFourCC(fourCC) == false) FILE_ERROR(RIFF_FOURCC_INVALID)//("error - invalid fileformat")
        
        if (strncmp(fourCC, "sfbk", 4) != 0) FILE_ERROR(RIFF_FOURCC_MISMATCH)//("error - not a sfbk fileformat")

        uint32_t chunkSize = 0;
        while (file.available() > 0)
        {
            // every block starts with a LIST tag
            if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(LIST_FOURCC_READ)//("read error - while reading listTag")
            if (verifyFourCC(fourCC) == false) FILE_ERROR(LIST_FOURCC_INVALID)//("error - listTag invalid")
            if (strncmp(fourCC, "LIST", 4) != 0) FILE_ERROR(LIST_FOURCC_MISMATCH)//("error - listTag is not LIST")

            
            if ((lastReadCount = file.read(&chunkSize, 4)) != 4) FILE_ERROR(LIST_SIZE_READ)//("read error - while getting listSize")

            if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(LISTTYPE_FOURCC_READ)//("read error - while reading listType")
            DebugPrintFOURCC(fourCC);
            DebugPrintFOURCC_size(chunkSize);
            if (verifyFourCC(fourCC) == false) FILE_ERROR(LISTTYPE_FOURCC_INVALID)//("error - invalid listType")
            
            
            if (strncmp(fourCC, "INFO", 4) == 0)
            {
                sfbk.info.size = chunkSize;
                if (readInfoBlock(file, sfbk.info) == false) return false;
                //file.close(); return true; // early return debug test
            }
            else if (strncmp(fourCC, "sdta", 4) == 0)
            {
                sfbk.sdta.size = chunkSize;
                if (read_sdta_block(file, sfbk.sdta) == false) return false;
                //file.close(); return true; // early return debug test
            }
            else if (strncmp(fourCC, "pdta", 4) == 0)
            {
                sfbk.pdta.size = chunkSize;
                if (read_pdta_block(file) == false) return false;
                //file.close(); return true; // early return debug test
            }
            else
            {
                // normally unknown blocks should be ignored
                if (file.seek(chunkSize - 4, SeekCur) == false) FILE_SEEK_ERROR(LIST_UNKNOWN_BLOCK_DATA_SKIP, chunkSize - 4)//("seek error - while skipping unknown sfbk root block")
            }
        }
        this->filePath = filePath;
        file.close();
        return true;
    }
    
    
    CODE_LOCATION bool Reader::read_pdta_block(File &file)
    {
        char fourCC[4];
        uint32_t size = 0;
        while (file.available())
        {
            if ((lastReadCount = file.readBytes(fourCC, 4)) != 4) FILE_ERROR(PDTA_FOURCC_READ)//("read error - while getting pdta block type")
            DebugPrintFOURCC(fourCC);
            if (verifyFourCC(fourCC) == false) FILE_ERROR(PDTA_FOURCC_INVALID)//("error - pdta type invalid")

            // store result for easier pinpoint of error handing
            bool sizeReadFail = ((lastReadCount = file.read(&size, 4)) != 4);

            if (strncmp(fourCC, "phdr", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_PHDR_SIZE_READ) //("read error - while getting pdta phdr block size")
                if (size % phdr_rec::Size != 0) FILE_ERROR(PDTA_PHDR_SIZE_MISMATCH)//("error - pdta phdr block size mismatch")

                sfbk.pdta.phdr_count = size/phdr_rec::Size;
                sfbk.pdta.phdr = new phdr_rec[sfbk.pdta.phdr_count];
                for (uint32_t i = 0; i < sfbk.pdta.phdr_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.phdr[i], phdr_rec::Size)) != phdr_rec::Size) FILE_ERROR(PDTA_SHDR_DATA_READ)//("read error - while reading pdta phdr record")
                }
            }
            else if (strncmp(fourCC, "pbag", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_PBAG_SIZE_READ) //("read error - while getting pdta pbag block size")
                if (size % bag_rec::Size != 0) FILE_ERROR(PDTA_PBAG_SIZE_MISMATCH)//("error - pdta pbag block size mismatch")

                sfbk.pdta.pbag_count = size/bag_rec::Size;
                sfbk.pdta.pbag = new bag_rec[sfbk.pdta.pbag_count];
                for (uint32_t i = 0; i < sfbk.pdta.pbag_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.pbag[i], bag_rec::Size)) != bag_rec::Size) FILE_ERROR(PDTA_PBAG_DATA_READ)//("read error - while reading pdta pbag record")
                }
            }
            else if (strncmp(fourCC, "pmod", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_PMOD_SIZE_READ) //("read error - while getting pdta pmod block size")
                if (size % mod_rec::Size != 0) FILE_ERROR(PDTA_PMOD_SIZE_MISMATCH)//("error - pdta pmod block size mismatch")

                sfbk.pdta.pmod_count = size/mod_rec::Size;
                sfbk.pdta.pmod = new mod_rec[sfbk.pdta.pmod_count];
                for (uint32_t i = 0; i < sfbk.pdta.pmod_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.pmod[i], mod_rec::Size)) != mod_rec::Size) FILE_ERROR(PDTA_PMOD_DATA_READ)//("read error - while reading pdta pmod record")
                }
            }
            else if (strncmp(fourCC, "pgen", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_PGEN_SIZE_READ) //("read error - while getting pdta pgen block size")
                if (size % gen_rec::Size != 0) FILE_ERROR(PDTA_PGEN_SIZE_MISMATCH)//("error - pdta pgen block size mismatch")

                sfbk.pdta.pgen_count = size/gen_rec::Size;
                sfbk.pdta.pgen = new gen_rec[sfbk.pdta.pgen_count];
                for (uint32_t i = 0; i < sfbk.pdta.pgen_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.pgen[i], gen_rec::Size)) != gen_rec::Size) FILE_ERROR(PDTA_PGEN_DATA_READ)//("read error - while reading pdta pgen record")
                }
            }
            else if (strncmp(fourCC, "inst", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_INST_SIZE_READ) //("read error - while getting pdta inst block size")
                if (size % inst_rec::Size != 0) FILE_ERROR(PDTA_INST_SIZE_MISMATCH)//("error - pdta inst block size mismatch")

                sfbk.pdta.inst_count = size/inst_rec::Size;
                sfbk.pdta.inst = new inst_rec[sfbk.pdta.inst_count];
                for (uint32_t i = 0; i < sfbk.pdta.inst_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.inst[i], inst_rec::Size)) != inst_rec::Size) FILE_ERROR(PDTA_INST_DATA_READ)//("read error - while reading pdta inst record")
                }
            }
            else if (strncmp(fourCC, "ibag", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_IBAG_SIZE_READ) //("read error - while getting pdta ibag block size")
                if (size % bag_rec::Size != 0) FILE_ERROR(PDTA_IBAG_SIZE_MISMATCH)//("error - pdta ibag block size mismatch")

                sfbk.pdta.ibag_count = size/bag_rec::Size;
                sfbk.pdta.ibag = new bag_rec[sfbk.pdta.ibag_count];
                for (uint32_t i = 0; i < sfbk.pdta.ibag_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.ibag[i], bag_rec::Size)) != bag_rec::Size) FILE_ERROR(PDTA_IBAG_DATA_READ)//("read error - while reading pdta ibag record")
                }
            }
            else if (strncmp(fourCC, "imod", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_IMOD_SIZE_READ) //("read error - while getting pdta imod block size")
                if (size % mod_rec::Size != 0) FILE_ERROR(PDTA_IMOD_SIZE_MISMATCH)//("error - pdta imod block size mismatch")

                sfbk.pdta.imod_count = size/mod_rec::Size;
                sfbk.pdta.imod = new mod_rec[sfbk.pdta.imod_count];
                for (uint32_t i = 0; i < sfbk.pdta.imod_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.imod[i], mod_rec::Size)) != mod_rec::Size) FILE_ERROR(PDTA_IMOD_DATA_READ)//("read error - while reading pdta imod record")
                }
            }
            else if (strncmp(fourCC, "igen", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_IGEN_SIZE_READ) //("read error - while getting pdta igen block size")
                if (size % gen_rec::Size != 0) FILE_ERROR(PDTA_IGEN_SIZE_MISMATCH)//("error - pdta igen block size mismatch")

                sfbk.pdta.igen_count = size/gen_rec::Size;
                sfbk.pdta.igen = new gen_rec[sfbk.pdta.igen_count];
                for (uint32_t i = 0; i < sfbk.pdta.igen_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.igen[i], gen_rec::Size)) != gen_rec::Size) FILE_ERROR(PDTA_IGEN_DATA_READ)//("read error - while reading pdta igen record")
                }
            }
            else if (strncmp(fourCC, "shdr", 4) == 0)
            {
                if (sizeReadFail) FILE_ERROR(PDTA_SHDR_SIZE_READ) //("read error - while getting pdta shdr block size")
                if (size % shdr_rec::Size != 0) FILE_ERROR(PDTA_SHDR_SIZE_MISMATCH)//("error - pdta shdr block size mismatch")

                sfbk.pdta.shdr_count = size/shdr_rec::Size;
                sfbk.pdta.shdr = new shdr_rec[sfbk.pdta.shdr_count];
                for (uint32_t i = 0; i < sfbk.pdta.shdr_count; i++) {
                    if ((lastReadCount = file.read(&sfbk.pdta.shdr[i], shdr_rec::Size)) != shdr_rec::Size) FILE_ERROR(PDTA_SHDR_DATA_READ)//("read error - while reading pdta shdr record")
                }
            }
            else if (strncmp(fourCC, "LIST", 4) == 0) // failsafe if file don't follow standard
            {
                // skip back
                if (file.seek(file.position()-4) == false) FILE_SEEK_ERROR(PDTA_BACK_SEEK, file.position()-4)
                return true;
            }
            else
            {
                // normally unknown blocks should be ignored
                if (sizeReadFail) FILE_ERROR(PDTA_UNKNOWN_BLOCK_SIZE_READ) //("read error - while getting unknown block size")
                if (file.seek(size, SeekCur) == false) FILE_SEEK_ERROR(PDTA_UNKNOWN_BLOCK_DATA_SKIP, size)//("seek error - while skipping unknown pdta block")
            }
        }
        return true;
    }
}