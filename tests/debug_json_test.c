#include "gb_debug_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    char out[256];
    CHECK(gb_debug_json_get("{\"type\":\"key\",\"key\":\"Right\"}", "key", out, sizeof(out)) && !strcmp(out, "Right"));
    CHECK(gb_debug_json_get("{\"path\":\"F:\\\\Games\\\\my \\\"save\\\".state\"}", "path", out, sizeof(out)) && !strcmp(out, "F:\\Games\\my \"save\".state"));
    CHECK(gb_debug_json_get("{\"path\":\"\\u00e9\\ud83d\\ude80\"}", "path", out, sizeof(out)) && !strcmp(out, "\xc3\xa9\xf0\x9f\x9a\x80"));
    CHECK(gb_debug_json_get("{\"nested\":{\"cmd\":\"quit\"},\"cmd\":\"ping\"}", "cmd", out, sizeof(out)) && !strcmp(out, "ping"));
    CHECK(gb_debug_json_get("{\"list\":[1,true,null,{\"x\":0}],\"id\":123}", "id", out, sizeof(out)) && !strcmp(out, "123"));
    CHECK(!gb_debug_json_get("{\"cmd\":\"ping\",\"cmd\":\"quit\"}", "cmd", out, sizeof(out)));
    CHECK(!gb_debug_json_valid("{\"path\":\"one\",\"path\":\"two\"}"));
    CHECK(!gb_debug_json_get("{\"cmd\":\"abcdef\"}", "cmd", out, 4));
    const char* bad[] = {"{\"x\":01}","{\"x\":1.}","{\"x\":1e}","{\"x\":truez}","{\"x\":\"a\\q\"}","{\"x\":\"\\u0000\"}","{\"x\":\"\\ud800\"}","{\"x\":1,}","{\"x\":[1,]}","{\"x\":\"unterminated}","{} trailing",NULL};
    for (unsigned i = 0; bad[i]; ++i) CHECK(!gb_debug_json_valid(bad[i]));
    puts("debug JSON escaping, scalar lookup, nested values and malformed-input checks passed");
    return 0;
}
