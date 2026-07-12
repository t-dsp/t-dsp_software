
#include "sf22aswt_error_enums.h"

namespace SF22ASWT::Error
{
#ifdef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT
    const uint16_t Operation_LockupTable[] PROGMEM = {
        // do the shifts here to optimize the lockup function code
        (uint16_t)Operation::OPEN,
        (uint16_t)Operation::READ,
        (uint16_t)Operation::INVALID,
        (uint16_t)Operation::MISMATCH,
        (uint16_t)Operation::SEEK,
        (uint16_t)Operation::SEEKSKIP,
        (uint16_t)Operation::MALLOC,
        (uint16_t)Operation::RANGE,
        (uint16_t)Operation::INSUFF,
    };
    const int Operation_LockupTable_Size = sizeof(Operation_LockupTable) / sizeof(Operation_LockupTable[0]);
    const char* const Operation_Strings[] PROGMEM = {
        "OPEN",
        "READ",
        "INVALID",
        "MISMATCH",
        "SEEK",
        "SEEKSKIP",
        "MALLOC",
        "RANGE",
        "INSUFF",
    };

    const uint16_t RootLocation_LockupTable[] PROGMEM = {
        // do the shifts here to optimize the lockup function code
        (uint16_t)RootLocation::FUNCTION,
        (uint16_t)RootLocation::RAM,
        (uint16_t)RootLocation::EXTRAM,
        (uint16_t)RootLocation::FILE,
        (uint16_t)RootLocation::RIFF,
        (uint16_t)RootLocation::LIST,
        (uint16_t)RootLocation::LISTTYPE,
        (uint16_t)RootLocation::INFO,
        (uint16_t)RootLocation::SDTA,
        (uint16_t)RootLocation::PDTA,
    };
    const int RootLocation_LockupTable_Size = sizeof(RootLocation_LockupTable) / sizeof(RootLocation_LockupTable[0]);
    const char* const RootLocation_Strings[] PROGMEM = {
        "FUNCTION",
        "RAM",
        "EXTRAM",
        "FILE",
        "RIFF",
        "LIST",
        "LISTTYPE",
        "INFO",
        "SDTA",
        "PDTA",
    };

    const uint16_t Type_LockupTable[] PROGMEM = {
        // do the shifts here to optimize the lockup function code
        (uint16_t)Type::NONE,
        (uint16_t)Type::FOURCC,
        (uint16_t)Type::SIZE,
        (uint16_t)Type::DATA,
        (uint16_t)Type::BACK,
        (uint16_t)Type::INDEX,
        (uint16_t)Type::UNKNOWN_BLOCK_SIZE,
        (uint16_t)Type::UNKNOWN_BLOCK_DATA,
    };
    const int Type_LockupTable_Size = sizeof(Type_LockupTable) / sizeof(Type_LockupTable[0]);
    const char* const Type_Strings[] PROGMEM = {
        "NOT", // used only for file open operation
        "FOURCC",
        "SIZE",
        "DATA",
        "BACK",
        "INDEX",
        "UNKNOWN_BLOCK_SIZE",
        "UNKNOWN_BLOCK_DATA",
    };
    const uint16_t FUNCTION_LockupTable[] PROGMEM = {
        (uint16_t)FUNCTION::LOAD_INST,
    };
    const int FUNCTION_LockupTable_Size = sizeof(FUNCTION_LockupTable)/sizeof(FUNCTION_LockupTable[0]);
    const char* const FUNCTION_Strings[] PROGMEM = {
        "LOAD_INST",
    };

    const uint16_t INFO_LockupTable[] PROGMEM = {
        (uint16_t)INFO::IFIL,
        (uint16_t)INFO::ISNG,
        (uint16_t)INFO::INAM,
        (uint16_t)INFO::IROM,
        (uint16_t)INFO::IVER,
        (uint16_t)INFO::ICRD,
        (uint16_t)INFO::IENG,
        (uint16_t)INFO::IPRD,
        (uint16_t)INFO::ICOP,
        (uint16_t)INFO::ICMT,
        (uint16_t)INFO::ISFT,
    };
    const int INFO_LockupTable_Size = sizeof(INFO_LockupTable) / sizeof(INFO_LockupTable[0]);
    const char* const INFO_Strings[] PROGMEM = {
        "IFIL",
        "ISNG",
        "INAM",
        "IROM",
        "IVER",
        "ICRD",
        "IENG",
        "IPRD",
        "ICOP",
        "ICMT",
        "ISFT",
    };

    const uint16_t SDTA_LockupTable[] PROGMEM = {
        (uint16_t)SDTA::SMPL,
        (uint16_t)SDTA::SM24,
    };
    const int SDTA_LockupTable_Size = sizeof(SDTA_LockupTable) / sizeof(SDTA_LockupTable[0]);
    const char* const SDTA_Strings[] PROGMEM = {
        "SMPL",
        "SM24",
    };

    const uint16_t PDTA_LockupTable[] PROGMEM = {
        (uint16_t)PDTA::PHDR,
        (uint16_t)PDTA::PBAG,
        (uint16_t)PDTA::PMOD,
        (uint16_t)PDTA::PGEN,
        (uint16_t)PDTA::INST,
        (uint16_t)PDTA::IBAG,
        (uint16_t)PDTA::IMOD,
        (uint16_t)PDTA::IGEN,
        (uint16_t)PDTA::SHDR,
    };
    const int PDTA_LockupTable_Size = sizeof(PDTA_LockupTable) / sizeof(PDTA_LockupTable[0]);
    const char* const PDTA_Strings[] PROGMEM = {
        "PHDR",
        "PBAG",
        "PMOD",
        "PGEN",
        "INST",
        "IBAG",
        "IMOD",
        "IGEN",
        "SHDR",
    };
    
#endif

} // 

namespace SF22ASWT
{
    using namespace SF22ASWT::Error;
#ifdef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT
    bool printError(Print &printStream, const uint16_t *lockupTable, const char * const *strings, int size, uint32_t code)
    {
        for (int i=0;i<size;i++) {
            if (lockupTable[i] == code)
            {
                printStream.print(strings[i]);
                return true;
            }
        }
        return false;
    }
#endif
    void printError(Print &printStream, Errors pe)
    {
        uint16_t code = (uint16_t)pe;
#ifdef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT
        if (code == 0) {
            printStream.print("NONE");
            return;
        }
#endif
        printStream.print(code, 16);
#ifndef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT
        printStream.println();
#endif
#ifdef SF22ASWT_PRINT_ERROR_CODE_AS_TEXT
        printStream.print(" ");
        uint32_t root = code & ERROR_ROOT_LOCATION_NIBBLE_MASK;
        uint32_t sub = code & ERROR_SUB_LOCATION_NIBBLE_MASK;
        uint32_t type = code & ERROR_TYPE_LOCATION_NIBBLE_MASK;
        uint32_t operation = code & ERROR_OPERATION_NIBBLE_MASK;

        if (printError(printStream,RootLocation_LockupTable, RootLocation_Strings, RootLocation_LockupTable_Size, root))
            printStream.print("_");
        
        // TODO the following should use the different lockup tables for each sub type list
        if (root == ((uint16_t)RootLocation::INFO)) {
            if (printError(printStream,INFO_LockupTable, INFO_Strings, INFO_LockupTable_Size, sub))
                printStream.print("_");
        } else if (root == ((uint16_t)RootLocation::SDTA)) {
            if (printError(printStream,SDTA_LockupTable, SDTA_Strings, SDTA_LockupTable_Size, sub))
                printStream.print("_");
        } else if (root == ((uint16_t)RootLocation::PDTA)) {
            if (printError(printStream,PDTA_LockupTable, PDTA_Strings, PDTA_LockupTable_Size, sub))
                printStream.print("_");
        } else if (root == ((uint16_t)RootLocation::FUNCTION)) {
            if (printError(printStream,FUNCTION_LockupTable, FUNCTION_Strings, FUNCTION_LockupTable_Size, sub))
                printStream.print("_");
        }
        if (printError(printStream,Type_LockupTable, Type_Strings, Type_LockupTable_Size, type))
            printStream.print("_");
        
        printError(printStream,Operation_LockupTable, Operation_Strings, Operation_LockupTable_Size, operation);
#endif
    }
}


namespace SF22ASWT::Error
{
    const SF22ASWT::Errors ErrorList[] PROGMEM = {
        Errors::NONE,
        Errors::RAM_DATA_MALLOC,
        Errors::FUNCTION_LOAD_INST_INDEX_RANGE,
        //Errors::NONE,
        Errors::FILE_NOT_OPEN,
        Errors::FILE_FOURCC_READ,
        Errors::FILE_FOURCC_INVALID,
        Errors::FILE_FOURCC_MISMATCH,
        //Errors::NONE,
        Errors::RIFF_SIZE_READ,
        Errors::RIFF_SIZE_MISMATCH,
        Errors::RIFF_FOURCC_READ,
        Errors::RIFF_FOURCC_INVALID,
        Errors::RIFF_FOURCC_MISMATCH,
        //Errors::NONE,
        Errors::LIST_FOURCC_READ,
        Errors::LIST_FOURCC_INVALID,
        Errors::LIST_FOURCC_MISMATCH,
        Errors::LIST_SIZE_READ,
        Errors::LIST_UNKNOWN_BLOCK_DATA_SKIP,
        //Errors::NONE,
        Errors::LISTTYPE_FOURCC_READ,
        Errors::LISTTYPE_FOURCC_INVALID,
        //Errors::NONE,
        Errors::INFO_FOURCC_READ,
        Errors::INFO_FOURCC_INVALID,
        Errors::INFO_DATA_SKIP,
        Errors::INFO_DATA_SEEK,
        Errors::INFO_BACK_SEEK,
        Errors::INFO_STRING_SIZE_READ,
        Errors::INFO_STRING_DATA_READ,
        //Errors::NONE,
        Errors::INFO_IFIL_SIZE_READ,
        Errors::INFO_IFIL_DATA_READ,
        Errors::INFO_ISNG_SIZE_READ,
        Errors::INFO_ISNG_DATA_READ,
        Errors::INFO_INAM_SIZE_READ,
        Errors::INFO_INAM_DATA_READ,
        Errors::INFO_IROM_SIZE_READ,
        Errors::INFO_IROM_DATA_READ,
        Errors::INFO_IVER_SIZE_READ,
        Errors::INFO_IVER_DATA_READ,
        Errors::INFO_ICRD_SIZE_READ,
        Errors::INFO_ICRD_DATA_READ,
        Errors::INFO_IENG_SIZE_READ,
        Errors::INFO_IENG_DATA_READ,
        Errors::INFO_IPRD_SIZE_READ,
        Errors::INFO_IPRD_DATA_READ,
        Errors::INFO_ICOP_SIZE_READ,
        Errors::INFO_ICOP_DATA_READ,
        Errors::INFO_ICMT_SIZE_READ,
        Errors::INFO_ICMT_DATA_READ,
        Errors::INFO_ISFT_SIZE_READ,
        Errors::INFO_ISFT_DATA_READ,
        Errors::INFO_UNKNOWN_BLOCK_SIZE_READ,
        Errors::INFO_UNKNOWN_BLOCK_DATA_SKIP,
        //Errors::NONE,
        Errors::SDTA_FOURCC_READ,
        Errors::SDTA_FOURCC_INVALID,
        Errors::SDTA_BACK_SEEK,
        Errors::SDTA_UNKNOWN_BLOCK_SIZE_READ,
        Errors::SDTA_UNKNOWN_BLOCK_DATA_SKIP,
        Errors::SDTA_SMPL_SIZE_READ,
        Errors::SDTA_SMPL_DATA_SEEK,
        Errors::SDTA_SMPL_DATA_READ,
        Errors::SDTA_SMPL_DATA_SKIP,
        Errors::SDTA_SM24_SIZE_READ,
        Errors::SDTA_SM24_DATA_SEEK,
        Errors::SDTA_SM24_DATA_READ,
        Errors::SDTA_SM24_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_FOURCC_READ,
        Errors::PDTA_FOURCC_INVALID,
        Errors::PDTA_BACK_SEEK,
        Errors::PDTA_UNKNOWN_BLOCK_SIZE_READ,
        Errors::PDTA_UNKNOWN_BLOCK_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_PHDR_SIZE_READ,
        Errors::PDTA_PHDR_SIZE_MISMATCH,
        Errors::PDTA_PHDR_DATA_SEEK,
        Errors::PDTA_PHDR_DATA_READ,
        Errors::PDTA_PHDR_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_PBAG_SIZE_READ,
        Errors::PDTA_PBAG_SIZE_MISMATCH,
        Errors::PDTA_PBAG_DATA_SEEK,
        Errors::PDTA_PBAG_DATA_READ,
        Errors::PDTA_PBAG_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_PMOD_SIZE_READ,
        Errors::PDTA_PMOD_SIZE_MISMATCH,
        Errors::PDTA_PMOD_DATA_SEEK,
        Errors::PDTA_PMOD_DATA_READ,
        Errors::PDTA_PMOD_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_PGEN_SIZE_READ,
        Errors::PDTA_PGEN_SIZE_MISMATCH,
        Errors::PDTA_PGEN_DATA_SEEK,
        Errors::PDTA_PGEN_DATA_READ,
        Errors::PDTA_PGEN_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_INST_SIZE_READ,
        Errors::PDTA_INST_SIZE_MISMATCH,
        Errors::PDTA_INST_DATA_SEEK,
        Errors::PDTA_INST_DATA_READ,
        Errors::PDTA_INST_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_IBAG_SIZE_READ,
        Errors::PDTA_IBAG_SIZE_MISMATCH,
        Errors::PDTA_IBAG_DATA_SEEK,
        Errors::PDTA_IBAG_DATA_READ,
        Errors::PDTA_IBAG_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_IMOD_SIZE_READ,
        Errors::PDTA_IMOD_SIZE_MISMATCH,
        Errors::PDTA_IMOD_DATA_SEEK,
        Errors::PDTA_IMOD_DATA_READ,
        Errors::PDTA_IMOD_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_IGEN_SIZE_READ,
        Errors::PDTA_IGEN_SIZE_MISMATCH,
        Errors::PDTA_IGEN_DATA_SEEK,
        Errors::PDTA_IGEN_DATA_READ,
        Errors::PDTA_IGEN_DATA_SKIP,
        //Errors::NONE,
        Errors::PDTA_SHDR_SIZE_READ,
        Errors::PDTA_SHDR_SIZE_MISMATCH,
        Errors::PDTA_SHDR_DATA_SEEK,
        Errors::PDTA_SHDR_DATA_READ,
        Errors::PDTA_SHDR_DATA_SKIP,

    };
    int ErrorList_Size = sizeof(ErrorList) / sizeof(ErrorList[0]);

    CODE_LOCATION void PrintList(Print &printStream)
    {

        printStream.print("Error count: "); printStream.print(ErrorList_Size-1); printStream.write('\n');
        for (int i=0;i<ErrorList_Size;i++)
        {
            printError(printStream, (SF22ASWT::Errors)ErrorList[i]);
            //printStream.println();
            printStream.write('\n');
        
        }
        //Errors pe = Errors::LISTTYPE_FOURCC_INVALID;
    }
}
