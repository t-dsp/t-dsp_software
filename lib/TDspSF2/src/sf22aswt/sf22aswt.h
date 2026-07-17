#pragma once

#define USE_LAZY_READER

#ifndef USE_LAZY_READER
// using SF22ASWT::reader is currently not fully implemented
// as both list_instruments and load_instrument is missing from 
// sf22asw_reader.h, and is only included for future use
// when more ram is available or for specific demands
#include <sf22aswt_reader.h>
#define SF22ASWTreader SF22ASWT::Reader
#else
#include <sf22aswt_reader_lazy.h>
#define SF22ASWTreader SF22ASWT::ReaderLazy
#endif
