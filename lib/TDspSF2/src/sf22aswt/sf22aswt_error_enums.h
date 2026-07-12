#pragma once

#ifndef SF22ASWT_ERROR_ENUMS_H_
#define SF22ASWT_ERROR_ENUMS_H_

#include <Arduino.h>
#include "sf22aswt_settings.h"

namespace SF22ASWT::Error
{
    #define ERROR_ROOT_LOCATION_NIBBLE 3
    #define ERROR_SUB_LOCATION_NIBBLE  2
    #define ERROR_TYPE_LOCATION_NIBBLE 1
    #define ERROR_OPERATION_NIBBLE     0
    #define ERROR_ROOT_LOCATION_SHIFT (ERROR_ROOT_LOCATION_NIBBLE*4)
    #define ERROR_SUB_LOCATION_SHIFT  (ERROR_SUB_LOCATION_NIBBLE*4)
    #define ERROR_TYPE_LOCATION_SHIFT (ERROR_TYPE_LOCATION_NIBBLE*4)
    #define ERROR_OPERATION_SHIFT     (ERROR_OPERATION_NIBBLE*4)
    #define ERROR_ROOT_LOCATION_NIBBLE_MASK (0xF << ERROR_ROOT_LOCATION_SHIFT)
    #define ERROR_SUB_LOCATION_NIBBLE_MASK  (0xF << ERROR_SUB_LOCATION_SHIFT)
    #define ERROR_TYPE_LOCATION_NIBBLE_MASK (0xF << ERROR_TYPE_LOCATION_SHIFT)
    #define ERROR_OPERATION_NIBBLE_MASK     (0xF << ERROR_OPERATION_SHIFT)
    
    enum class Operation
	{
        OPEN = 1 << ERROR_OPERATION_SHIFT,
        /** required data bytes could not be read from file, 
         * probably because of file corruption */
		READ = 2 << ERROR_OPERATION_SHIFT,
        /** FourCC did contain non alphanumeric characters */
		INVALID = 3 << ERROR_OPERATION_SHIFT,
        /** FourCC/Size do not match the expected value */
		MISMATCH = 4 << ERROR_OPERATION_SHIFT,
        /** file seek error */
        SEEK = 5 << ERROR_OPERATION_SHIFT,
        /** file seek error while skipping data */
        SEEKSKIP = 6 << ERROR_OPERATION_SHIFT,
        /** fail to allocate memory in RAM for sample data*/
        MALLOC = 7 << ERROR_OPERATION_SHIFT,
        /** instrument index out of range */
        RANGE = 8 << ERROR_OPERATION_SHIFT,
        /** use when available ram is not sufficient to fit the sample data */
        INSUFF = 9 << ERROR_OPERATION_SHIFT,
	};
    enum class Type
	{
        /** only used for file open operation*/
        NONE = 0 << ERROR_TYPE_LOCATION_SHIFT,
		FOURCC = 1 << ERROR_TYPE_LOCATION_SHIFT,
		SIZE = 2 << ERROR_TYPE_LOCATION_SHIFT,
        DATA = 3 << ERROR_TYPE_LOCATION_SHIFT,
        BACK = 4 << ERROR_TYPE_LOCATION_SHIFT,
        INDEX = 0xD << ERROR_TYPE_LOCATION_SHIFT,
        UNKNOWN_BLOCK_SIZE = 0xE << ERROR_TYPE_LOCATION_SHIFT,
        UNKNOWN_BLOCK_DATA = 0xF << ERROR_TYPE_LOCATION_SHIFT,
	};
	enum class RootLocation
	{
		FILE = 1 << ERROR_ROOT_LOCATION_SHIFT,
		RIFF = 2 << ERROR_ROOT_LOCATION_SHIFT,
		LIST = 3 << ERROR_ROOT_LOCATION_SHIFT,
        LISTTYPE = 4 << ERROR_ROOT_LOCATION_SHIFT,
        INFO = 5 << ERROR_ROOT_LOCATION_SHIFT,
        SDTA = 6 << ERROR_ROOT_LOCATION_SHIFT,
        PDTA = 7 << ERROR_ROOT_LOCATION_SHIFT,
        RAM = 0xD << ERROR_ROOT_LOCATION_SHIFT,
        EXTRAM = 0xE << ERROR_ROOT_LOCATION_SHIFT,
        FUNCTION = 0xF << ERROR_ROOT_LOCATION_SHIFT,
	};
    enum class FUNCTION
    {
        LOAD_INST = 1 << ERROR_SUB_LOCATION_SHIFT,

    };
    enum class INFO
    {
        IFIL = 1 << ERROR_SUB_LOCATION_SHIFT,
        ISNG = 2 << ERROR_SUB_LOCATION_SHIFT,
        INAM = 3 << ERROR_SUB_LOCATION_SHIFT,
        IROM = 4 << ERROR_SUB_LOCATION_SHIFT,
        IVER = 5 << ERROR_SUB_LOCATION_SHIFT,
        ICRD = 6 << ERROR_SUB_LOCATION_SHIFT,
        IENG = 7 << ERROR_SUB_LOCATION_SHIFT,
        IPRD = 8 << ERROR_SUB_LOCATION_SHIFT,
        ICOP = 9 << ERROR_SUB_LOCATION_SHIFT,
        ICMT = 0xA << ERROR_SUB_LOCATION_SHIFT,
        ISFT = 0xB << ERROR_SUB_LOCATION_SHIFT,
    };
    enum class SDTA
    {
        SMPL = 1 << ERROR_SUB_LOCATION_SHIFT,
        SM24 = 2 << ERROR_SUB_LOCATION_SHIFT,
    };
    enum class PDTA
    {
        PHDR = 1 << ERROR_SUB_LOCATION_SHIFT,
        PBAG = 2 << ERROR_SUB_LOCATION_SHIFT,
        PMOD = 3 << ERROR_SUB_LOCATION_SHIFT,
        PGEN = 4 << ERROR_SUB_LOCATION_SHIFT,
        INST = 5 << ERROR_SUB_LOCATION_SHIFT,
        IBAG = 6 << ERROR_SUB_LOCATION_SHIFT,
        IMOD = 7 << ERROR_SUB_LOCATION_SHIFT,
        IGEN = 8 << ERROR_SUB_LOCATION_SHIFT,
        SHDR = 9 << ERROR_SUB_LOCATION_SHIFT,
    };
	
    /** * Used for root errors * */
    #define ERROR(ROOT_LOCATION, TYPE, OPERATION) (\
                                    (uint16_t)SF22ASWT::Error::Operation::OPERATION + \
                                    (uint16_t)SF22ASWT::Error::Type::TYPE + \
                                    (uint16_t)SF22ASWT::Error::RootLocation::ROOT_LOCATION)
    /** * Used for sub location errors * */
    #define ERROR_SUB(ROOT_LOCATION, SUB_LOCATION, TYPE, OPERATION) (\
                                    (uint16_t)SF22ASWT::Error::Operation::OPERATION + \
                                    (uint16_t)SF22ASWT::Error::Type::TYPE + \
                                    (uint16_t)SF22ASWT::Error::ROOT_LOCATION::SUB_LOCATION + \
                                    (uint16_t)SF22ASWT::Error::RootLocation::ROOT_LOCATION)
    



}

namespace SF22ASWT
{
    enum class Errors : uint16_t
    {
        NONE                    = 0x0000, // no error
        /** usen when checking for available ram before allocating memory for sampledata */
        RAM_SIZE_INSUFF          = ERROR(RAM, SIZE, INSUFF),
        EXTRAM_SIZE_INSUFF       = ERROR(EXTRAM, SIZE, INSUFF),
        /** MALLOC fail when allocating memory for sampledata */
        RAM_DATA_MALLOC         = ERROR(RAM, DATA, MALLOC),
        EXTRAM_DATA_MALLOC      = ERROR(EXTRAM, DATA, MALLOC),
        FUNCTION_LOAD_INST_INDEX_RANGE = ERROR_SUB(FUNCTION, LOAD_INST, INDEX, RANGE),

        FILE_NOT_OPEN           = ERROR(FILE, NONE, OPEN), // file could not be opened
        FILE_FOURCC_READ        = ERROR(FILE, FOURCC, READ),     // read error - RIFF fileTag
        FILE_FOURCC_INVALID     = ERROR(FILE, FOURCC, INVALID),  // RIFF tag invalid
        FILE_FOURCC_MISMATCH    = ERROR(FILE, FOURCC, MISMATCH), // not a RIFF fileformat
        
        RIFF_SIZE_READ          = ERROR(RIFF, SIZE, READ), // read error - riff size
        RIFF_SIZE_MISMATCH      = ERROR(RIFF, SIZE, MISMATCH), // riff size do not match expected filesize
        RIFF_FOURCC_READ        = ERROR(RIFF, FOURCC, READ), // read error - RIFF fileformat
        RIFF_FOURCC_INVALID     = ERROR(RIFF, FOURCC, INVALID), // invalid - RIFF fileformat tag
        RIFF_FOURCC_MISMATCH    = ERROR(RIFF, FOURCC, MISMATCH), // not a sfbk fileformat
        
        LIST_FOURCC_READ        = ERROR(LIST, FOURCC, READ), // read error -  listTag
        LIST_FOURCC_INVALID     = ERROR(LIST, FOURCC, INVALID), // invalid - listTag
        LIST_FOURCC_MISMATCH    = ERROR(LIST, FOURCC, MISMATCH), // listtag is not LIST
        LIST_SIZE_READ          = ERROR(LIST, SIZE, READ), // read error - list size
        LIST_UNKNOWN_BLOCK_DATA_SKIP = ERROR(LIST, UNKNOWN_BLOCK_DATA, SEEKSKIP),

        LISTTYPE_FOURCC_READ    = ERROR(LISTTYPE, FOURCC, READ), // read error - list type
        LISTTYPE_FOURCC_INVALID = ERROR(LISTTYPE, FOURCC, MISMATCH), // invalid - list type
        
        INFO_FOURCC_READ        = ERROR(INFO, FOURCC, READ), // read error - info subblock type
        INFO_FOURCC_INVALID     = ERROR(INFO, FOURCC, INVALID), // invalid - info subblock type
        INFO_DATA_SKIP          = ERROR(INFO, DATA, SEEKSKIP),
        INFO_DATA_SEEK          = ERROR(INFO, DATA, SEEK),
        INFO_BACK_SEEK          = ERROR(INFO, BACK, SEEK),
        INFO_UNKNOWN_BLOCK_SIZE_READ = ERROR(INFO, UNKNOWN_BLOCK_SIZE, READ), // read error - unknown info subblock size
        INFO_UNKNOWN_BLOCK_DATA_SKIP = ERROR(INFO, UNKNOWN_BLOCK_DATA, SEEKSKIP), // seek error - skipping unknown info subblock
        INFO_STRING_SIZE_READ   = ERROR(INFO, SIZE, READ), // the sub block is set later
        INFO_STRING_DATA_READ   = ERROR(INFO, DATA, READ), // the sub block is set later
        INFO_IFIL_SIZE_READ     = ERROR_SUB(INFO, IFIL, SIZE, READ),
        INFO_IFIL_DATA_READ     = ERROR_SUB(INFO, IFIL, DATA, READ),
        INFO_IVER_SIZE_READ     = ERROR_SUB(INFO, IVER, SIZE, READ),
        INFO_IVER_DATA_READ     = ERROR_SUB(INFO, IVER, DATA, READ),
        // the following are only here for printing the error list, and normally is 
        // INFO_STRING_SIZE_READ and INFO_STRING_DATA_READ used in code
        INFO_ISNG_SIZE_READ     = ERROR_SUB(INFO, ISNG, SIZE, READ),
        INFO_ISNG_DATA_READ     = ERROR_SUB(INFO, ISNG, DATA, READ),
        INFO_INAM_SIZE_READ     = ERROR_SUB(INFO, INAM, SIZE, READ),
        INFO_INAM_DATA_READ     = ERROR_SUB(INFO, INAM, DATA, READ),
        INFO_IROM_SIZE_READ     = ERROR_SUB(INFO, IROM, SIZE, READ),
        INFO_IROM_DATA_READ     = ERROR_SUB(INFO, IROM, DATA, READ),
        INFO_ICRD_SIZE_READ     = ERROR_SUB(INFO, ICRD, SIZE, READ),
        INFO_ICRD_DATA_READ     = ERROR_SUB(INFO, ICRD, DATA, READ),
        INFO_IENG_SIZE_READ     = ERROR_SUB(INFO, IENG, SIZE, READ),
        INFO_IENG_DATA_READ     = ERROR_SUB(INFO, IENG, DATA, READ),
        INFO_IPRD_SIZE_READ     = ERROR_SUB(INFO, IPRD, SIZE, READ),
        INFO_IPRD_DATA_READ     = ERROR_SUB(INFO, IPRD, DATA, READ),
        INFO_ICOP_SIZE_READ     = ERROR_SUB(INFO, ICOP, SIZE, READ),
        INFO_ICOP_DATA_READ     = ERROR_SUB(INFO, ICOP, DATA, READ),
        INFO_ICMT_SIZE_READ     = ERROR_SUB(INFO, ICMT, SIZE, READ),
        INFO_ICMT_DATA_READ     = ERROR_SUB(INFO, ICMT, DATA, READ),
        INFO_ISFT_SIZE_READ     = ERROR_SUB(INFO, ISFT, SIZE, READ),
        INFO_ISFT_DATA_READ     = ERROR_SUB(INFO, ISFT, DATA, READ),

        SDTA_FOURCC_READ        = ERROR(SDTA, FOURCC, READ), // read error - sdta subblock type
        SDTA_FOURCC_INVALID     = ERROR(SDTA, FOURCC, INVALID), // invalid - sdta subblock type
        SDTA_BACK_SEEK          = ERROR(SDTA, BACK, SEEK),
        SDTA_UNKNOWN_BLOCK_SIZE_READ = ERROR(SDTA, UNKNOWN_BLOCK_SIZE, READ), // read error - unknown sdta subblock size
        SDTA_UNKNOWN_BLOCK_DATA_SKIP = ERROR(SDTA, UNKNOWN_BLOCK_DATA, SEEKSKIP), // seek error - skipping unknown sdta subblock
        SDTA_SMPL_SIZE_READ     = ERROR_SUB(SDTA, SMPL, SIZE, READ), // read error - smpl size
        SDTA_SMPL_DATA_SEEK     = ERROR_SUB(SDTA, SMPL, DATA, SEEK),
        SDTA_SMPL_DATA_READ     = ERROR_SUB(SDTA, SMPL, DATA, READ),
        SDTA_SMPL_DATA_SKIP     = ERROR_SUB(SDTA, SMPL, DATA, SEEKSKIP), // seek error - skipping smpl data
        SDTA_SM24_SIZE_READ     = ERROR_SUB(SDTA, SM24, SIZE, READ), // read error - smpl size
        SDTA_SM24_DATA_SEEK     = ERROR_SUB(SDTA, SM24, DATA, SEEK),
        SDTA_SM24_DATA_READ     = ERROR_SUB(SDTA, SM24, DATA, READ),
        SDTA_SM24_DATA_SKIP     = ERROR_SUB(SDTA, SM24, DATA, SEEKSKIP), // seek error - skipping smpl data
        
        PDTA_FOURCC_READ        = ERROR(PDTA, FOURCC, READ), // read error - pdta subblock type
        PDTA_FOURCC_INVALID     = ERROR(PDTA, FOURCC, INVALID), // invalid - pdta subblock type
        PDTA_BACK_SEEK          = ERROR(PDTA, BACK, SEEK),
        PDTA_UNKNOWN_BLOCK_SIZE_READ = ERROR(PDTA, UNKNOWN_BLOCK_SIZE, READ), // read error - pdta subblock size
        PDTA_UNKNOWN_BLOCK_DATA_SKIP = ERROR(PDTA, UNKNOWN_BLOCK_DATA, SEEKSKIP), // seek error - skipping unknown pdta subblock
        
        PDTA_PHDR_SIZE_READ     = ERROR_SUB(PDTA, PHDR, SIZE, READ),
        PDTA_PHDR_SIZE_MISMATCH = ERROR_SUB(PDTA, PHDR, SIZE, MISMATCH),
        PDTA_PHDR_DATA_SEEK     = ERROR_SUB(PDTA, PHDR, DATA, SEEK),
        PDTA_PHDR_DATA_READ     = ERROR_SUB(PDTA, PHDR, DATA, READ),
        PDTA_PHDR_DATA_SKIP     = ERROR_SUB(PDTA, PHDR, DATA, SEEKSKIP),
        
        PDTA_PBAG_SIZE_READ     = ERROR_SUB(PDTA, PBAG, SIZE, READ),
        PDTA_PBAG_SIZE_MISMATCH = ERROR_SUB(PDTA, PBAG, SIZE, MISMATCH),
        PDTA_PBAG_DATA_SEEK     = ERROR_SUB(PDTA, PBAG, DATA, SEEK),
        PDTA_PBAG_DATA_READ     = ERROR_SUB(PDTA, PBAG, DATA, READ),
        PDTA_PBAG_DATA_SKIP     = ERROR_SUB(PDTA, PBAG, DATA, SEEKSKIP),
        
        PDTA_PMOD_SIZE_READ     = ERROR_SUB(PDTA, PMOD, SIZE, READ),
        PDTA_PMOD_SIZE_MISMATCH = ERROR_SUB(PDTA, PMOD, SIZE, MISMATCH),
        PDTA_PMOD_DATA_SEEK     = ERROR_SUB(PDTA, PMOD, DATA, SEEK),
        PDTA_PMOD_DATA_READ     = ERROR_SUB(PDTA, PMOD, DATA, READ),
        PDTA_PMOD_DATA_SKIP     = ERROR_SUB(PDTA, PMOD, DATA, SEEKSKIP),
        
        PDTA_PGEN_SIZE_READ     = ERROR_SUB(PDTA, PGEN, SIZE, READ),
        PDTA_PGEN_SIZE_MISMATCH = ERROR_SUB(PDTA, PGEN, SIZE, MISMATCH),
        PDTA_PGEN_DATA_SEEK     = ERROR_SUB(PDTA, PGEN, DATA, SEEK),
        PDTA_PGEN_DATA_READ     = ERROR_SUB(PDTA, PGEN, DATA, READ),
        PDTA_PGEN_DATA_SKIP     = ERROR_SUB(PDTA, PGEN, DATA, SEEKSKIP),

        PDTA_INST_SIZE_READ     = ERROR_SUB(PDTA, INST, SIZE, READ),
        PDTA_INST_SIZE_MISMATCH = ERROR_SUB(PDTA, INST, SIZE, MISMATCH),
        PDTA_INST_DATA_SEEK     = ERROR_SUB(PDTA, INST, DATA, SEEK),
        PDTA_INST_DATA_READ     = ERROR_SUB(PDTA, INST, DATA, READ),
        PDTA_INST_DATA_SKIP     = ERROR_SUB(PDTA, INST, DATA, SEEKSKIP),

        PDTA_IBAG_SIZE_READ     = ERROR_SUB(PDTA, IBAG, SIZE, READ),
        PDTA_IBAG_SIZE_MISMATCH = ERROR_SUB(PDTA, IBAG, SIZE, MISMATCH),
        PDTA_IBAG_DATA_SEEK     = ERROR_SUB(PDTA, IBAG, DATA, SEEK),
        PDTA_IBAG_DATA_READ     = ERROR_SUB(PDTA, IBAG, DATA, READ),
        PDTA_IBAG_DATA_SKIP     = ERROR_SUB(PDTA, IBAG, DATA, SEEKSKIP),

        PDTA_IMOD_SIZE_READ     = ERROR_SUB(PDTA, IMOD, SIZE, READ),
        PDTA_IMOD_SIZE_MISMATCH = ERROR_SUB(PDTA, IMOD, SIZE, MISMATCH),
        PDTA_IMOD_DATA_SEEK     = ERROR_SUB(PDTA, IMOD, DATA, SEEK),
        PDTA_IMOD_DATA_READ     = ERROR_SUB(PDTA, IMOD, DATA, READ),
        PDTA_IMOD_DATA_SKIP     = ERROR_SUB(PDTA, IMOD, DATA, SEEKSKIP),

        PDTA_IGEN_SIZE_READ     = ERROR_SUB(PDTA, IGEN, SIZE, READ),
        PDTA_IGEN_SIZE_MISMATCH = ERROR_SUB(PDTA, IGEN, SIZE, MISMATCH),
        PDTA_IGEN_DATA_SEEK     = ERROR_SUB(PDTA, IGEN, DATA, SEEK),
        PDTA_IGEN_DATA_READ     = ERROR_SUB(PDTA, IGEN, DATA, READ),
        PDTA_IGEN_DATA_SKIP     = ERROR_SUB(PDTA, IGEN, DATA, SEEKSKIP),

        PDTA_SHDR_SIZE_READ     = ERROR_SUB(PDTA, SHDR, SIZE, READ),
        PDTA_SHDR_SIZE_MISMATCH = ERROR_SUB(PDTA, SHDR, SIZE, MISMATCH),
        PDTA_SHDR_DATA_SEEK     = ERROR_SUB(PDTA, SHDR, DATA, SEEK),
        PDTA_SHDR_DATA_READ     = ERROR_SUB(PDTA, SHDR, DATA, READ),
        PDTA_SHDR_DATA_SKIP     = ERROR_SUB(PDTA, SHDR, DATA, SEEKSKIP),
    };

    #ifdef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT

    bool printError(Print &printStream, const uint16_t *lockupTable, const char * const *strings, int size, uint32_t code);
    
#endif
    void printError(Print &printStream, Errors pe);
}

namespace SF22ASWT::Error
{
    extern const Errors ErrorList[];
    extern int ErrorList_Size;

    void PrintList(Print &printStream);
} // namespace SF22ASWT::Error::Test

#endif