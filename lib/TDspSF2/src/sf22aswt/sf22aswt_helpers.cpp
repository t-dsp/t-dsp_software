
#include "sf22aswt_helpers.h"

namespace Helpers
{
    void printRawBytes(Print &printStream, const char* bytes, size_t length)
    {
        for (size_t i=0;i<length;i++)
        {
            if (bytes[i] < 32 || bytes[i] > 126)
            {
                printStream.print("[");
                printStream.print(bytes[i],HEX);
                printStream.print("]");
            }
            else
                printStream.write(bytes[i]);
        }
    }
    void printRawBytesSanitized(Print &printStream, const char* bytes, size_t length)
    {
        for (size_t i=0;i<length;i++)
        {
            if (bytes[i] < 32 || bytes[i] > 126)
                printStream.write(' ');
            else
                printStream.write(bytes[i]);
        }
    }
    void printRawBytesSanitizedUntil(Print &printStream, const char* bytes, size_t length, char untilchar)
    {
        for (size_t i=0;i<length;i++)
        {
            if (bytes[i] == untilchar) return;
            if (bytes[i] < 32 || bytes[i] > 126)
                printStream.write(' ');
            else
                printStream.write(bytes[i]);
        }
    }
    void printRawBytesUntil(Print &printStream, const char* bytes, size_t length, char untilchar)
    {
        for (size_t i=0;i<length;i++)
        {
            if (bytes[i] == untilchar) return;
            if (bytes[i] < 32 || bytes[i] > 126)
            {
                printStream.print("[");
                printStream.print(bytes[i],HEX);
                printStream.print("]");
            }
            else
                printStream.write(bytes[i]);
        }
    }

    
}