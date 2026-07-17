#pragma once

#include <Arduino.h>
#include "sf22aswt_settings.h"


namespace Helpers
{
    void printRawBytes(Print &printStream, const char* bytes, size_t length);
    void printRawBytesSanitized(Print &printStream, const char* bytes, size_t length);
    void printRawBytesSanitizedUntil(Print &printStream, const char* bytes, size_t length, char untilchar);
    void printRawBytesUntil(Print &printStream, const char* bytes, size_t length, char untilchar);

    // can be used to get strings from:
    // PrintInstrumentListAsJson, PrintPresetListAsJson & PrintInfoBlock 
    // or other things using Print
    class MemoryStream : public Print {
    private:
        char *buffer;
        size_t bufferSize;
        size_t bufferIndex;

    public:
        MemoryStream(size_t size) {
            buffer = new char[size];
            bufferSize = size;
            bufferIndex = 0;
        }

        ~MemoryStream() {
            delete[] buffer;
        }

        virtual size_t write(uint8_t c) {
            if (bufferIndex < bufferSize - 1) {
                buffer[bufferIndex++] = c;
                buffer[bufferIndex] = '\0'; // Null-terminate the string
                return 1; // Successful write
            } else {
                return 0; // Buffer overflow
            }
        }

        virtual size_t write(const uint8_t *buffer, size_t size) {
            size_t bytesWritten = 0;
            for (size_t i = 0; i < size; ++i) {
                if (write(buffer[i]) == 1) {
                    bytesWritten++;
                } else {
                    break; // Stop writing on buffer overflow
                }
            }
            return bytesWritten;
        }

        const char *getData() {
            return buffer;
        }

        size_t getSize() {
            return bufferIndex;
        }

        void clear() {
            bufferIndex = 0;
            buffer[0] = '\0';
        }

            
    };
}