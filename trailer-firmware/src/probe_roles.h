// SPDX-License-Identifier: Apache-2.0
/*
 * Decides which temperature probe is inside and which is outside, and checks a reading.
 * No Particle code here, so it also runs on a computer: see test/probe_roles_test.cpp.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace proberoles {

constexpr size_t ID_LEN = 8;
constexpr int NONE = -1;

struct Roles {
    int inside;
    int outside;
};

inline bool isBlank(const uint8_t* id) {
    for (size_t i = 0; i < ID_LEN; i++) {
        if (id[i] != 0) {
            return false;
        }
    }
    return true;
}

// ids holds the probes found, up to two. saved is the ID kept as the inside probe, all zeros
// if none is kept yet. A lone probe that does not match the saved ID is outside: the inside
// probe has gone quiet, and a reading must not change sides because of that.
inline Roles assign(const uint8_t (*ids)[ID_LEN], size_t count, const uint8_t* saved) {
    Roles roles = {NONE, NONE};
    if (count == 0) {
        return roles;
    }
    if (memcmp(ids[0], saved, ID_LEN) == 0) {
        roles.inside = 0;
        roles.outside = count >= 2 ? 1 : NONE;
    } else if (count >= 2 && memcmp(ids[1], saved, ID_LEN) == 0) {
        roles.inside = 1;
        roles.outside = 0;
    } else if (count >= 2) {
        roles.inside = 0;
        roles.outside = 1;
    } else if (isBlank(saved)) {
        roles.inside = 0;
    } else {
        roles.outside = 0;
    }
    return roles;
}

// A DS18B20 returns exactly 85.00 C at power-up if a read finished too early, so that value
// is not a real reading. The chip range is -55 to 125 C.
inline bool goodReading(float c) {
    return c >= -55.0f && c <= 125.0f && !(c > 84.99f && c < 85.01f);
}

inline int toCenti(float c) {
    return (int)(c * 100.0f + (c >= 0 ? 0.5f : -0.5f));
}

}  // namespace proberoles
