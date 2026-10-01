// OpllBank.cpp — see OpllBank.h.
#include "OpllBank.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#if defined(__IMXRT1062__)
extern "C" void* extmem_malloc(size_t);
extern uint8_t external_psram_size;
#endif

namespace tdsp {

static int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "13 01 18 0F 9E 60 00 9F<TAB>Piano 1" -> regs + name. Returns false for comments/blank/bad lines.
bool OpllBankLib::parseLine(const char* line, uint8_t regs[8], char* name, size_t nameCap) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/')) return false;
    for (int i = 0; i < 8; i++) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '$') p++;
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        int hi = hexNib(p[0]), lo = hexNib(p[1]);
        if (hi < 0 || lo < 0) return false;
        regs[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
        if (*p == ',') p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '|' || *p == ';') p++;
    size_t n = 0;
    while (*p && *p != '\r' && *p != '\n' && n + 1 < nameCap) name[n++] = *p++;
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) n--;   // trim trailing blanks
    name[n] = 0;
    if (!n) snprintf(name, nameCap, "patch");
    return true;
}

bool OpllBankLib::loadFile(FS& fs, const char* path, const char* defaultName) {
    if (bankCount_ >= kMaxBanks) return false;
    File f = fs.open(path);
    if (!f || f.isDirectory()) return false;
    const int bank = bankCount_;
    strncpy(bankNames_[bank], defaultName, sizeof(bankNames_[bank]) - 1);
    bankNames_[bank][sizeof(bankNames_[bank]) - 1] = 0;
    strncpy(bankFiles_[bank], defaultName, sizeof(bankFiles_[bank]) - 1);
    bankFiles_[bank][sizeof(bankFiles_[bank]) - 1] = 0;
    bankStart_[bank] = count_;
    bankSize_[bank]  = 0;

    char line[160];
    size_t n = 0;
    auto flush = [&]() {
        line[n] = 0;
        n = 0;
        // "#name: Display Name" overrides the bank's display name.
        const char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "#name:", 6)) {
            p += 6; while (*p == ' ' || *p == '\t') p++;
            size_t k = 0;
            while (*p && *p != '\r' && k + 1 < sizeof(bankNames_[bank])) bankNames_[bank][k++] = *p++;
            while (k && bankNames_[bank][k - 1] == ' ') k--;
            bankNames_[bank][k] = 0;
            return;
        }
        if (count_ >= cap_) return;
        OpllPatch& pt = patches_[count_];
        if (parseLine(line, pt.regs, pt.name, sizeof(pt.name))) { pt.bank = (uint8_t)bank; count_++; bankSize_[bank]++; }
    };
    while (f.available()) {
        int c = f.read();
        if (c < 0) break;
        if (c == '\n') flush();
        else if (n < sizeof(line) - 1) line[n++] = (char)c;
        // over-long lines: silently truncate (names are cut anyway)
    }
    if (n) flush();
    f.close();
    if (bankSize_[bank] == 0) return false;   // empty / unparsable file: don't register a bank
    bankCount_++;
    return true;
}

bool OpllBankLib::begin(FS& fs, const char* dir, int maxPatches, int heapCap) {
    count_ = 0; bankCount_ = 0;
    File d = fs.open(dir);
    if (!d || !d.isDirectory()) return false;

    if (!patches_) {
        int want = maxPatches;
#if defined(__IMXRT1062__)
        if (external_psram_size > 0) {
            patches_ = (OpllPatch*)extmem_malloc(sizeof(OpllPatch) * want);
            psram_ = patches_ != nullptr;
        }
#endif
        if (!patches_) {
            want = want < heapCap ? want : heapCap;
            patches_ = (OpllPatch*)malloc(sizeof(OpllPatch) * want);
            psram_ = false;
        }
        if (!patches_) { d.close(); return false; }
        cap_ = want;
    }

    // Files in directory order; bank display name = file name without the extension.
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char* nm = e.name();
            const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
            size_t L = strlen(base);
            if (L > 4 && !strcasecmp(base + L - 4, ".txt")) {
                char disp[32]; size_t k = L - 4; if (k > sizeof(disp) - 1) k = sizeof(disp) - 1;
                memcpy(disp, base, k); disp[k] = 0;
                char path[96]; snprintf(path, sizeof(path), "%s/%s", dir, base);
                e.close();
                loadFile(fs, path, disp);
                continue;
            }
        }
        e.close();
    }
    d.close();
    return count_ > 0;
}

int OpllBankLib::findBank(const char* name) const {
    for (int b = 0; b < bankCount_; b++)
        if (!strcasecmp(bankNames_[b], name) || !strcasecmp(bankFiles_[b], name)) return b;
    return -1;
}

} // namespace tdsp
