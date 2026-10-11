#!/usr/bin/env python3
"""Allocation-free status serialization: escaping, complete output and every capacity boundary."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = r'''
#include "status_json.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void fields(status_json_t *json) {
    status_json_string(json, "device", "kubik-test");
    status_json_string(json, "escaped", "\"\\\n\r\t\001\037 Tess UTF-8: \320\242\320\265\321\201\321\201");
    status_json_integer(json, "uptime", 4294967295);
    status_json_integer(json, "offset", -12);
    status_json_integer(json, "minimum", INT64_MIN);
    status_json_integer(json, "maximum", INT64_MAX);
    status_json_bool(json, "online", true);
    status_json_bool(json, "recording", false);
}
int main(void) {
    char complete[1024]; status_json_t json;
    status_json_begin(&json, complete, sizeof complete); fields(&json);
    assert(status_json_end(&json)); puts(complete);
    size_t needed = strlen(complete) + 1;
    for (size_t capacity = 0; capacity <= needed + 1; capacity++) {
        unsigned char guarded[1026]; memset(guarded, 0xA5, sizeof guarded);
        status_json_begin(&json, (char *)guarded + 1, capacity); fields(&json);
        bool result = status_json_end(&json);
        assert(result == (capacity >= needed));
        assert(guarded[0] == 0xA5 && guarded[capacity + 1] == 0xA5);
        if (result) assert(!strcmp((char *)guarded + 1, complete));
        else if (capacity) assert(guarded[1] == 0);
    }
    status_json_begin(&json, complete, sizeof complete);
    assert(status_json_end(&json) && !strcmp(complete, "{}"));
}
'''
with tempfile.TemporaryDirectory(prefix="kubik-status-") as tmp:
    source = Path(tmp) / "test.c"; source.write_text(SOURCE)
    executable = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(source),
                    "firmware/main/status_json.c", "-o", str(executable)], cwd=ROOT, check=True)
    result = subprocess.check_output([str(executable)], text=True)
    assert json.loads(result) == {"device": "kubik-test", "escaped": '"\\\n\r\t\x01\x1f Tess UTF-8: Тесс',
                                 "uptime": 4294967295, "offset": -12, "minimum": -(2**63), "maximum": 2**63 - 1, "online": True, "recording": False}
print("status JSON: escaping, types, exact fit, all short buffers and signed 64-bit limits passed under ASan/UBSan")
