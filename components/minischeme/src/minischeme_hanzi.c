/**
 * @file minischeme_hanzi.c
 * @brief Simplified / traditional Chinese on screen (see minischeme_hanzi.h).
 */

#include "minischeme_hanzi.h"

#include <stddef.h>
#include <string.h>

#include "minischeme_hanzi_table.h"

static int s_mode = MINI_HANZI_KEEP;

void mini_hanzi_set_mode(int mode) {
    s_mode = (mode == MINI_HANZI_SIMPLIFIED || mode == MINI_HANZI_TRADITIONAL) ? mode : MINI_HANZI_KEEP;
}

int mini_hanzi_mode(void) { return s_mode; }

int mini_hanzi_parse(const char *value) {
    if (!value) return -1;
    while (*value == ' ') value++;
    if (*value == '\0' || strcmp(value, "keep") == 0 || strcmp(value, "0") == 0) return MINI_HANZI_KEEP;
    if (strcmp(value, "simplified") == 0 || strcmp(value, "s") == 0 || strcmp(value, "简体") == 0) {
        return MINI_HANZI_SIMPLIFIED;
    }
    if (strcmp(value, "traditional") == 0 || strcmp(value, "t") == 0 || strcmp(value, "繁体") == 0 ||
        strcmp(value, "繁體") == 0) {
        return MINI_HANZI_TRADITIONAL;
    }
    return -1;
}

/* The index of cp in the sorted keys, or -1 */
static int find(const uint16_t *keys, int n, uint32_t cp) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid] == cp) return mid;
        if (keys[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

uint32_t mini_hanzi_convert(uint32_t cp, int mode) {
    if (cp < 0x4E00 || cp > 0x9FFF) return cp;   /* the tables hold the common ideographs only */
    if (mode == MINI_HANZI_TRADITIONAL) {
        int i = find(HANZI_S2T_FROM, HANZI_S2T_COUNT, cp);
        return i >= 0 ? HANZI_S2T_TO[i] : cp;
    }
    if (mode == MINI_HANZI_SIMPLIFIED) {
        int i = find(HANZI_T2S_FROM, HANZI_T2S_COUNT, cp);
        return i >= 0 ? HANZI_T2S_TO[i] : cp;
    }
    return cp;
}

uint32_t mini_hanzi_map(uint32_t cp) {
    return s_mode == MINI_HANZI_KEEP ? cp : mini_hanzi_convert(cp, s_mode);
}
