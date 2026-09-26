#include "scan.h"

/* UTF-8 stays UTF-8. Invalid byte sequences are escaped to U+00xx for display;
 * root_hex and path_hex remain the lossless source of truth. */
static unsigned utf8_length(const unsigned char *s) {
    unsigned n = s[0] >= 0xc2 && s[0] <= 0xdf   ? 2
                 : s[0] >= 0xe0 && s[0] <= 0xef ? 3
                 : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4
                                                : 0;
    if (!n) {
        return 0;
    }
    for (unsigned j = 1; j < n; ++j) {
        if (!s[j] || (s[j] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) {
        return 0;
    }
    return n;
}

void vs_json_string(FILE *f, const char *str) {
    const unsigned char *s = (const unsigned char *)str;
    fputc('"', f);
    while (*s) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
            ++s;
        } else if (c < 32) {
            fprintf(f, "\\u%04x", c);
            ++s;
        } else if (c < 128) {
            fputc(c, f);
            ++s;
        } else {
            unsigned n = utf8_length(s);
            if (n) {
                fwrite(s, 1, n, f);
                s += n;
            } else {
                fprintf(f, "\\u%04x", c);
                ++s;
            }
        }
    }
    fputc('"', f);
}

void vs_json_hex(FILE *f, const unsigned char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    fputc('"', f);
    for (size_t j = 0; j < n; ++j) {
        fputc(hex[s[j] >> 4], f);
        fputc(hex[s[j] & 15], f);
    }
    fputc('"', f);
}
