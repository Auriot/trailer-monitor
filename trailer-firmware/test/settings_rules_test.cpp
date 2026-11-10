// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from trailer-firmware/:
 *   g++ -std=c++11 -Wall -Isrc test/settings_rules_test.cpp -o <out> && <out>
 */
#include <stdio.h>
#include <string.h>

#include "settings_rules.h"

using namespace settingsrules;

static int failures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            failures++;                                    \
        }                                                  \
    } while (0)

// A made-up key, not a real one.
static const char* KEY = "00112233445566778899aabbccddeeff";

static void keys() {
    KeyCommand c = parseKeyCommand(KEY);
    CHECK(c.kind == KEY_SOLAR && !c.remove && c.key[0] == 0x00 && c.key[1] == 0x11 && c.key[15] == 0xff);

    char text[64];
    snprintf(text, sizeof(text), "solar,%s", KEY);
    c = parseKeyCommand(text);
    CHECK(c.kind == KEY_SOLAR && !c.remove && c.key[15] == 0xff);
    snprintf(text, sizeof(text), "shunt,%s", KEY);
    c = parseKeyCommand(text);
    CHECK(c.kind == KEY_SHUNT && !c.remove && c.key[8] == 0x88);
    snprintf(text, sizeof(text), "protect,%s", KEY);
    c = parseKeyCommand(text);
    CHECK(c.kind == KEY_PROTECT && !c.remove);
    snprintf(text, sizeof(text), "protect,%s", "00112233445566778899AABBCCDDEEFF");
    c = parseKeyCommand(text);
    CHECK(c.kind == KEY_PROTECT && c.key[10] == 0xaa);

    c = parseKeyCommand("solar,off");
    CHECK(c.kind == KEY_SOLAR && c.remove);
    c = parseKeyCommand("shunt,off");
    CHECK(c.kind == KEY_SHUNT && c.remove);
    c = parseKeyCommand("protect,off");
    CHECK(c.kind == KEY_PROTECT && c.remove);

    // Refused: bare off, wrong length, wrong letters, unknown kind, empty, nothing.
    CHECK(parseKeyCommand("off").kind == -1);
    CHECK(parseKeyCommand("OFF").kind == -1);
    CHECK(parseKeyCommand("shunt,OFF").kind == -1);
    CHECK(parseKeyCommand("0011").kind == -1);
    CHECK(parseKeyCommand("00112233445566778899aabbccddeeff0").kind == -1);
    CHECK(parseKeyCommand("00112233445566778899aabbccddeefg").kind == -1);
    snprintf(text, sizeof(text), "inverter,%s", KEY);
    CHECK(parseKeyCommand(text).kind == -1);
    snprintf(text, sizeof(text), "shunt, %s", KEY);
    CHECK(parseKeyCommand(text).kind == -1);
    CHECK(parseKeyCommand("shunt,").kind == -1);
    CHECK(parseKeyCommand("").kind == -1);
    CHECK(parseKeyCommand(nullptr).kind == -1);
}

static void names() {
    CHECK(validHeaterName("BYD-0000"));
    CHECK(validHeaterName("AA:BB:CC:DD:EE:FF"));
    CHECK(validHeaterName("a"));
    CHECK(validHeaterName("12345678901234567890123"));    // 23 characters
    CHECK(!validHeaterName("123456789012345678901234"));  // 24 characters
    CHECK(!validHeaterName(""));
    CHECK(!validHeaterName(nullptr));
    CHECK(!validHeaterName("BYD\"x"));
    CHECK(!validHeaterName("BYD\\x"));
    CHECK(!validHeaterName("BYD\nx"));
    CHECK(!validHeaterName("BYD\x7f"));
    CHECK(!validHeaterName("caf\xc3\xa9"));
    CHECK(isOff("off"));
    CHECK(!isOff("Off"));
    CHECK(!isOff(nullptr));

    CHECK(nameChange("BYD-1", "BYD-1", false, false) == NAME_SAME);
    CHECK(nameChange("BYD-1", "BYD-1", true, true) == NAME_SAME);
    CHECK(nameChange("BYD-1", "BYD-2", false, false) == NAME_APPLY);
    CHECK(nameChange("", "BYD-2", false, false) == NAME_APPLY);
    CHECK(nameChange("BYD-1", "", false, false) == NAME_APPLY);
    CHECK(nameChange("BYD-1", "BYD-2", true, false) == NAME_REFUSE);
    CHECK(nameChange("BYD-1", "BYD-2", false, true) == NAME_REFUSE);
    CHECK(nameChange("BYD-1", "", false, true) == NAME_REFUSE);
    CHECK(nameChange("BYD-1", "", true, false) == NAME_REFUSE);
}

static void services() {
    const uint8_t one[] = {0xE0, 0xFF};
    const uint8_t three[] = {0x0A, 0x18, 0xE0, 0xFF, 0x0F, 0x18};
    const uint8_t other[] = {0xFF, 0xE0};  // Wrong byte order.
    const uint8_t odd[] = {0x0A, 0x18, 0xE0};
    CHECK(hasService16(one, sizeof(one), 0xFFE0));
    CHECK(hasService16(three, sizeof(three), 0xFFE0));
    CHECK(!hasService16(three, sizeof(three), 0xFFE1));
    CHECK(!hasService16(other, sizeof(other), 0xFFE0));
    CHECK(!hasService16(odd, sizeof(odd), 0xFFE0));
    CHECK(!hasService16(one, 0, 0xFFE0));
    CHECK(!hasService16(one, 1, 0xFFE0));
}

static void nearList() {
    NearList list;
    CHECK(list.count == 0);
    list.add("BYD-1");
    list.add("BYD-1");
    list.add("");
    list.add("bad\"name");
    CHECK(list.count == 1 && strcmp(list.names[0], "BYD-1") == 0);
    list.add("AA:BB:CC:DD:EE:FF");
    list.add("BYD-3");
    list.add("BYD-4");
    CHECK(list.count == 3);
    CHECK(strcmp(list.names[1], "AA:BB:CC:DD:EE:FF") == 0 && strcmp(list.names[2], "BYD-3") == 0);
    for (size_t i = 0; i < list.count; i++) {
        CHECK(strlen(list.names[i]) < NAME_LEN);
    }
}

int main() {
    keys();
    names();
    services();
    nearList();
    if (failures == 0) {
        printf("settings_rules_test: all passed\n");
        return 0;
    }
    printf("settings_rules_test: %d failed\n", failures);
    return 1;
}
