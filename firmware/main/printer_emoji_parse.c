#include "printer_emoji_parse.h"
#include <string.h>

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c-'A';
    if (c >= 'a' && c <= 'z') return c-'a'+26;
    if (c >= '0' && c <= '9') return c-'0'+52;
    if (c == '+') return 62;
    return c == '/' ? 63 : -1;
}

static bool bitmap_decode(const char *source, uint8_t out[PRINTER_EMOJI_BYTES]) {
    if (!source || strlen(source) != 172 || source[171] != '=') return false;
    unsigned written = 0;
    for (unsigned i=0; i<172; i+=4) {
        int a=base64_digit(source[i]), b=base64_digit(source[i+1]), c=base64_digit(source[i+2]);
        if (a<0 || b<0 || c<0) return false;
        out[written++]=(uint8_t)((a<<2) | (b>>4));
        out[written++]=(uint8_t)((b<<4) | (c>>2));
        if (i == 168) return !(c & 3) && written == PRINTER_EMOJI_BYTES;
        int d=base64_digit(source[i+3]);
        if (d<0) return false;
        out[written++]=(uint8_t)((c<<6) | d);
    }
    return false;
}

static bool glyph_fields(const cJSON *glyph) {
    if (!cJSON_IsObject(glyph)) return false;
    unsigned seen=0;
    const cJSON *item;
    cJSON_ArrayForEach(item,glyph) {
        unsigned bit=!strcmp(item->string,"sequence") ? 1 : !strcmp(item->string,"bitmap") ? 2 : 0;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == 3;
}

static const char *glyph_sequence(const cJSON *glyph) {
    const char *sequence=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(glyph,"sequence"));
    if (!sequence) return NULL;
    return printer_emoji_sequence_valid(sequence,strlen(sequence)) ? sequence : NULL;
}

static bool unique_sequence(const cJSON *array, const cJSON *current, const char *sequence) {
    for (const cJSON *item=array->child;item && item!=current;item=item->next) {
        const char *previous=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"sequence"));
        if (previous && !strcmp(sequence,previous)) return false;
    }
    return true;
}

bool printer_emoji_parse_valid(const cJSON *array, const char *text, unsigned *count) {
    if (!cJSON_IsArray(array) || !text) return false;
    *count=0;
    uint8_t bitmap[PRINTER_EMOJI_BYTES];
    const cJSON *glyph;
    cJSON_ArrayForEach(glyph,array) {
        if (++*count>PRINTER_EMOJI_MAX || !glyph_fields(glyph)) return false;
        const char *sequence=glyph_sequence(glyph);
        if (!sequence || !strstr(text,sequence) || !unique_sequence(array,glyph,sequence)) return false;
        const char *encoded=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(glyph,"bitmap"));
        if (!bitmap_decode(encoded,bitmap)) return false;
    }
    return true;
}

bool printer_emoji_parse_copy(const cJSON *array, printer_emoji_glyph_t *out, unsigned count) {
    if (cJSON_GetArraySize(array)!=(int)count) return false;
    for (unsigned i=0;i<count;i++) {
        const cJSON *glyph=cJSON_GetArrayItem(array,(int)i);
        const char *sequence=glyph_sequence(glyph);
        if (!sequence) return false;
        memset(&out[i],0,sizeof out[i]);out[i].length=(uint8_t)strlen(sequence);
        memcpy(out[i].sequence,sequence,out[i].length);
        const char *encoded=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(glyph,"bitmap"));
        if (!bitmap_decode(encoded,out[i].bitmap)) return false;
    }
    return true;
}
