// SPDX-License-Identifier: Apache-2.0
/*
 * Settings the phone page sends to the box: Victron keys, the heater name, and the list of
 * nearby heaters the page shows so a builder can tap theirs.
 * Plain C++ with no Particle code, so the same file also runs in a test on a computer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "victron_decode.h"

namespace settingsrules {

constexpr size_t NAME_LEN = 24;  // A heater name, plus its end mark.
constexpr char OFF_WORD[] = "off";

enum KeyKind { KEY_SOLAR = 0, KEY_SHUNT = 1, KEY_PROTECT = 2, KEY_KINDS = 3 };

struct KeyCommand {
    int kind = -1;  // -1: the text is not right.
    bool remove = false;
    uint8_t key[16] = {};
};

// "<32 hex>" or "solar,<32 hex>" is the solar charger's key; "shunt,<32 hex>" and
// "protect,<32 hex>" the others. "solar,off", "shunt,off" and "protect,off" remove a key.
// A bare "off" is refused, so a slip cannot remove the solar key.
inline KeyCommand parseKeyCommand(const char* text) {
    KeyCommand command;
    if (text == nullptr) {
        return command;
    }
    int kind = KEY_SOLAR;
    bool named = true;
    if (strncmp(text, "solar,", 6) == 0) {
        text += 6;
    } else if (strncmp(text, "shunt,", 6) == 0) {
        kind = KEY_SHUNT;
        text += 6;
    } else if (strncmp(text, "protect,", 8) == 0) {
        kind = KEY_PROTECT;
        text += 8;
    } else {
        named = false;
    }
    if (strcmp(text, OFF_WORD) == 0) {
        if (named) {
            command.kind = kind;
            command.remove = true;
        }
        return command;
    }
    if (victron::parseKey(text, command.key)) {
        command.kind = kind;
    }
    return command;
}

// A heater name the box can keep and put in its report as it is: 1 to 23 characters, plain
// printable letters, no quote mark and no backslash.
inline bool validHeaterName(const char* text) {
    size_t length = text != nullptr ? strlen(text) : 0;
    if (length == 0 || length >= NAME_LEN) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

inline bool isOff(const char* text) {
    return text != nullptr && strcmp(text, OFF_WORD) == 0;
}

enum NameChange { NAME_SAME, NAME_APPLY, NAME_REFUSE };

// A new heater name, or none (""), is refused while the heater runs or the box has a run of its
// own. Then the box never forgets the heater it may have to stop.
inline NameChange nameChange(const char* current, const char* asked, bool runActive, bool heaterOn) {
    if (strcmp(current, asked) == 0) {
        return NAME_SAME;
    }
    if (runActive || heaterOn) {
        return NAME_REFUSE;
    }
    return NAME_APPLY;
}

// True if a list of 16-bit Bluetooth service numbers (raw broadcast bytes, low byte first)
// holds the number asked for.
inline bool hasService16(const uint8_t* list, size_t length, uint16_t service) {
    for (size_t i = 0; i + 1 < length; i += 2) {
        if ((uint16_t)(list[i] | (list[i + 1] << 8)) == service) {
            return true;
        }
    }
    return false;
}

// Nearby heaters heard in one scan: each name (or address, when it sends no name) once. A full
// list keeps the first ones heard. A name the report cannot carry as it is, is left out.
constexpr size_t NEAR_MAX = 3;

struct NearList {
    char names[NEAR_MAX][NAME_LEN] = {};
    size_t count = 0;

    void add(const char* name) {
        if (!validHeaterName(name)) {
            return;
        }
        for (size_t i = 0; i < count; i++) {
            if (strcmp(names[i], name) == 0) {
                return;
            }
        }
        if (count >= NEAR_MAX) {
            return;
        }
        strncpy(names[count], name, NAME_LEN - 1);
        names[count][NAME_LEN - 1] = 0;
        count++;
    }
};

}  // namespace settingsrules
