#pragma once

// this mode takes 448 bytes of flash
//#define SF22ASWT_PRINT_ERROR_CODE_AS_TEXT

// Save RAM1 by putting code in Flash
// Seems to increase the Flash, for some reason...
#define CODE_LOCATION FLASHMEM
//#define CODE_LOCATION
