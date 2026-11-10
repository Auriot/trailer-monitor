// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from trailer-firmware/:
 *   g++ -std=c++11 -Wall -Isrc test/victron_decode_test.cpp -o <out> && <out>
 * The broadcasts below were made with a made-up key (00112233445566778899aabbccddeeff), never a
 * real device key.
 */
#include <stdio.h>
#include <string.h>

#include "victron_decode.h"

using namespace victron;

static int failures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            failures++;                                    \
        }                                                  \
    } while (0)

static size_t hexBytes(const char* hex, uint8_t* out) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static uint8_t key[16];

static void testShunt() {
    uint8_t m[32];
    size_t n = hexBytes("E102100239C002D31500B9041D9DE945D6BE43E678F2AB6A46", m);
    Header h;
    CHECK(parseHeader(m, n, &h));
    CHECK(h.readoutType == READOUT_BATTERY_MONITOR);
    CHECK(h.model == 0xC039);
    ShuntReading s;
    CHECK(decodeShunt(key, h, &s) == DecodeResult::OK);
    CHECK(s.minutesLeft == 2863);
    CHECK(s.centivolts == 1269);
    CHECK(s.alarm == 4);
    CHECK(s.aux == 29815);
    CHECK(s.auxMode == 2);
    CHECK(s.milliamps == -2317);
    CHECK(s.usedDeciAh == 13);
    CHECK(s.socDeci == 994);
    // Charging, and the not-available values.
    n = hexBytes("E102100239C002020100BB80EC043DE341F81641B5F90DBD7E", m);
    CHECK(parseHeader(m, n, &h));
    ShuntReading c;
    CHECK(decodeShunt(key, h, &c) == DecodeResult::OK);
    CHECK(c.milliamps == 5123);
    CHECK(c.centivolts == 1351);
    CHECK(c.minutesLeft == 0xFFFF);
    CHECK(c.usedDeciAh == 0xFFFFF);
    CHECK(c.socDeci == 0x3FF);
    CHECK(c.auxMode == 3);
    // Wrong key, wrong type, too short.
    uint8_t other[16];
    memcpy(other, key, 16);
    other[0] ^= 1;
    CHECK(decodeShunt(other, h, &c) == DecodeResult::KEY_MISMATCH);
    ProtectReading p;
    CHECK(decodeProtect(key, h, &p) != DecodeResult::OK);
    SolarReading r;
    CHECK(decodeSolar(key, h, &r) == DecodeResult::NOT_SOLAR);
    h.dataLen = SHUNT_DATA_LEN - 1;
    CHECK(decodeShunt(key, h, &c) == DecodeResult::TOO_SHORT);
}

static void testProtect() {
    uint8_t m[32];
    size_t n = hexBytes("E1021002B0A309B300008BAD5A83871A3F6B1CC7040F7B344C", m);
    Header h;
    CHECK(parseHeader(m, n, &h));
    CHECK(h.readoutType == READOUT_BATTERY_PROTECT);
    ProtectReading p;
    CHECK(decodeProtect(key, h, &p) == DecodeResult::OK);
    CHECK(p.state == 249);
    CHECK(p.output == 4);
    CHECK(p.error == 0);
    CHECK(p.alarm == 1);
    CHECK(p.warning == 0);
    CHECK(p.inCentivolts == 1180);
    CHECK(p.outCentivolts == 0);
    CHECK(p.offReason == 0x10);
    ShuntReading s;
    CHECK(decodeShunt(key, h, &s) != DecodeResult::OK);
}

static void testBits() {
    uint8_t d[4] = {0xF0, 0x0F, 0xFF, 0x80};
    BitReader b(d, sizeof(d));
    CHECK(b.take(4) == 0x0);
    CHECK(b.take(8) == 0xFF);
    CHECK(b.takeSigned(4) == 0);
    CHECK(b.takeSigned(8) == -1);
    CHECK(b.take(8) == 0x80);
    CHECK(b.take(8) == 0);  // Past the end reads as zero.
}

int main() {
    parseKey("00112233445566778899aabbccddeeff", key);
    testBits();
    testShunt();
    testProtect();
    printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
