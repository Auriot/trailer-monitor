// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from trailer-firmware/:
 *   g++ -std=c++11 -Wall -Isrc test/probe_roles_test.cpp -o <out> && <out>
 */
#include <stdio.h>

#include "probe_roles.h"

using namespace proberoles;

static int failures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            failures++;                                    \
        }                                                  \
    } while (0)

int main() {
    const uint8_t ids[2][ID_LEN] = {{1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}};
    const uint8_t idsSwapped[2][ID_LEN] = {{2, 2, 2, 2, 2, 2, 2, 2}, {1, 1, 1, 1, 1, 1, 1, 1}};
    const uint8_t blank[ID_LEN] = {};
    const uint8_t first[ID_LEN] = {1, 1, 1, 1, 1, 1, 1, 1};
    const uint8_t second[ID_LEN] = {2, 2, 2, 2, 2, 2, 2, 2};
    const uint8_t gone[ID_LEN] = {9, 9, 9, 9, 9, 9, 9, 9};

    // No probes.
    Roles r = assign(ids, 0, first);
    CHECK(r.inside == NONE && r.outside == NONE);

    // Nothing saved: the first two found fill the roles, a lone probe is inside.
    r = assign(ids, 2, blank);
    CHECK(r.inside == 0 && r.outside == 1);
    r = assign(ids, 1, blank);
    CHECK(r.inside == 0 && r.outside == NONE);

    // The saved ID decides, whatever order the search returns.
    r = assign(ids, 2, first);
    CHECK(r.inside == 0 && r.outside == 1);
    r = assign(ids, 2, second);
    CHECK(r.inside == 1 && r.outside == 0);
    r = assign(idsSwapped, 2, first);
    CHECK(r.inside == 1 && r.outside == 0);

    // One probe quiet: the other keeps its side.
    r = assign(ids, 1, first);
    CHECK(r.inside == 0 && r.outside == NONE);
    r = assign(idsSwapped, 1, first);
    CHECK(r.inside == NONE && r.outside == 0);

    // A replaced probe: both unknown, so the first found is inside. The swap button fixes it.
    r = assign(ids, 2, gone);
    CHECK(r.inside == 0 && r.outside == 1);

    // Readings.
    CHECK(goodReading(21.5f));
    CHECK(goodReading(-40.0f));
    CHECK(goodReading(84.5f));
    CHECK(!goodReading(85.0f));
    CHECK(!goodReading(-127.0f));
    CHECK(!goodReading(200.0f));
    CHECK(toCenti(25.13f) == 2513);
    CHECK(toCenti(-0.31f) == -31);
    CHECK(toCenti(0.0f) == 0);

    if (failures == 0) {
        printf("all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
