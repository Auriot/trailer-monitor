// SPDX-License-Identifier: Apache-2.0
/*
 * Decoder for Victron Instant Readout Bluetooth broadcasts: solar charger, battery monitor
 * (SmartShunt) and Smart BatteryProtect.
 * Plain C++ with no Particle code, so the same file also runs in a test on a computer.
 *
 * Layout of the manufacturer data, by byte:
 *   0-1  company ID 0x02E1 (E1 02)
 *   2-3  prefix, starts with record type 0x10
 *   4-5  model ID, low byte first (SmartSolar 150/60 is A062 or A06B per victron-ble)
 *   6    readout type (0x01 is solar charger)
 *   7-8  counter start, low byte first
 *   9    first byte of the key, used as a key check
 *   10+  data, AES-128-CTR
 * Data fields, low bit first: state 8, error 8, volts 16 signed (0.01 V), amps 16
 * signed (0.1 A), yield 16 (10 Wh), solar 16 (1 W), load 9 (0.1 A).
 * Battery monitor (type 0x02), 15 bytes: time left 16 (minutes), volts 16 signed (0.01 V),
 * alarm 16, aux 16, aux mode 2, current 22 signed (1 mA, minus = out of the battery),
 * used 20 (0.1 Ah, Victron shows it as negative), state of charge 10 (0.1 %).
 * Smart BatteryProtect (type 0x09), 15 bytes: state 8, output 8, error 8, alarm 16, warning 16,
 * input volts 16 signed (0.01 V), output volts 16 (0.01 V), off reason 32.
 * Field layouts follow the open victron-ble parser; Victron does not support parsers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace victron {

constexpr uint8_t COMPANY_LOW = 0xE1;
constexpr uint8_t COMPANY_HIGH = 0x02;
constexpr uint8_t RECORD_INSTANT_READOUT = 0x10;
constexpr uint8_t READOUT_SOLAR_CHARGER = 0x01;
constexpr uint8_t READOUT_BATTERY_MONITOR = 0x02;
constexpr uint8_t READOUT_BATTERY_PROTECT = 0x09;
constexpr size_t HEADER_LEN = 10;
constexpr size_t SOLAR_DATA_LEN = 12;  // 89 bits
constexpr size_t SHUNT_DATA_LEN = 15;  // 118 bits
constexpr size_t PROTECT_DATA_LEN = 15;  // 120 bits

namespace detail {

inline uint8_t sbox(uint8_t x) {
    static const uint8_t table[256] = {
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
    return table[x];
}

inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

}  // namespace detail

// AES-128, encrypt one block. CTR mode needs only the encrypt direction.
inline void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    using detail::sbox;
    using detail::xtime;
    uint8_t roundKey[16];
    uint8_t s[16];
    uint8_t rcon = 0x01;
    memcpy(roundKey, key, 16);
    for (int i = 0; i < 16; i++) {
        s[i] = in[i] ^ roundKey[i];
    }
    for (int round = 1; round <= 10; round++) {
        // Next round key.
        uint8_t t0 = sbox(roundKey[13]) ^ rcon;
        uint8_t t1 = sbox(roundKey[14]);
        uint8_t t2 = sbox(roundKey[15]);
        uint8_t t3 = sbox(roundKey[12]);
        rcon = xtime(rcon);
        roundKey[0] ^= t0;
        roundKey[1] ^= t1;
        roundKey[2] ^= t2;
        roundKey[3] ^= t3;
        for (int i = 4; i < 16; i++) {
            roundKey[i] ^= roundKey[i - 4];
        }
        // SubBytes and ShiftRows.
        uint8_t b[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                b[c * 4 + r] = sbox(s[((c + r) % 4) * 4 + r]);
            }
        }
        // MixColumns, except in the last round.
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = b + c * 4;
                uint8_t all = col[0] ^ col[1] ^ col[2] ^ col[3];
                uint8_t first = col[0];
                col[0] ^= all ^ xtime(col[0] ^ col[1]);
                col[1] ^= all ^ xtime(col[1] ^ col[2]);
                col[2] ^= all ^ xtime(col[2] ^ col[3]);
                col[3] ^= all ^ xtime(col[3] ^ first);
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] = b[i] ^ roundKey[i];
        }
    }
    memcpy(out, s, 16);
}

// AES-128-CTR. The counter block is the 16-bit start value, low byte first, then zeros,
// and counts up low byte first. Encrypt and decrypt are the same operation.
inline void aes128Ctr(const uint8_t key[16], uint16_t start, const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t counter[16] = {};
    counter[0] = (uint8_t)(start & 0xFF);
    counter[1] = (uint8_t)(start >> 8);
    uint8_t stream[16];
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            aes128EncryptBlock(key, counter, stream);
            for (int c = 0; c < 16; c++) {
                if (++counter[c] != 0) {
                    break;
                }
            }
        }
        out[i] = in[i] ^ stream[i % 16];
    }
}

struct Header {
    uint16_t model = 0;
    uint8_t readoutType = 0;
    uint16_t counterStart = 0;
    uint8_t keyCheck = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
};

// True if this manufacturer data is an Instant Readout record.
inline bool parseHeader(const uint8_t* mfg, size_t len, Header* out) {
    if (len < HEADER_LEN || mfg[0] != COMPANY_LOW || mfg[1] != COMPANY_HIGH || mfg[2] != RECORD_INSTANT_READOUT) {
        return false;
    }
    out->model = (uint16_t)(mfg[4] | (mfg[5] << 8));
    out->readoutType = mfg[6];
    out->counterStart = (uint16_t)(mfg[7] | (mfg[8] << 8));
    out->keyCheck = mfg[9];
    out->data = mfg + HEADER_LEN;
    out->dataLen = len - HEADER_LEN;
    return true;
}

struct SolarReading {
    uint8_t state = 0xFF;       // 0xFF means not available
    uint8_t error = 0xFF;       // 0xFF means not available
    int16_t centivolts = 0x7FFF;   // 0x7FFF means not available
    int16_t deciamps = 0x7FFF;     // 0x7FFF means not available
    uint16_t yield10Wh = 0xFFFF;   // 0xFFFF means not available
    uint16_t watts = 0xFFFF;       // 0xFFFF means not available
    uint16_t loadDeciamps = 0x1FF; // 0x1FF means not available
};

enum class DecodeResult { OK, KEY_MISMATCH, NOT_SOLAR, TOO_SHORT };

inline DecodeResult decodeSolar(const uint8_t key[16], const Header& h, SolarReading* out) {
    if (h.readoutType != READOUT_SOLAR_CHARGER) {
        return DecodeResult::NOT_SOLAR;
    }
    if (h.keyCheck != key[0]) {
        return DecodeResult::KEY_MISMATCH;
    }
    if (h.dataLen < SOLAR_DATA_LEN) {
        return DecodeResult::TOO_SHORT;
    }
    uint8_t p[SOLAR_DATA_LEN];
    aes128Ctr(key, h.counterStart, h.data, SOLAR_DATA_LEN, p);
    out->state = p[0];
    out->error = p[1];
    out->centivolts = (int16_t)(uint16_t)(p[2] | (p[3] << 8));
    out->deciamps = (int16_t)(uint16_t)(p[4] | (p[5] << 8));
    out->yield10Wh = (uint16_t)(p[6] | (p[7] << 8));
    out->watts = (uint16_t)(p[8] | (p[9] << 8));
    out->loadDeciamps = (uint16_t)(p[10] | ((p[11] & 0x01) << 8));
    return DecodeResult::OK;
}

// Reads fields packed low bit first, as Victron sends them.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    uint32_t take(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; i++, pos_++) {
            size_t byte = pos_ / 8;
            if (byte < len_ && (data_[byte] >> (pos_ % 8)) & 1) {
                v |= (uint32_t)1 << i;
            }
        }
        return v;
    }

    int32_t takeSigned(int bits) {
        uint32_t v = take(bits);
        if (bits < 32 && (v >> (bits - 1)) & 1) {
            v |= ~(((uint32_t)1 << bits) - 1);
        }
        return (int32_t)v;
    }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

// Decrypts n bytes of a record into out after checking type, key and length.
inline DecodeResult decryptRecord(const uint8_t key[16], const Header& h, uint8_t type, size_t n, uint8_t* out) {
    if (h.readoutType != type) {
        return DecodeResult::NOT_SOLAR;  // Wrong type for this decoder.
    }
    if (h.keyCheck != key[0]) {
        return DecodeResult::KEY_MISMATCH;
    }
    if (h.dataLen < n) {
        return DecodeResult::TOO_SHORT;
    }
    aes128Ctr(key, h.counterStart, h.data, n, out);
    return DecodeResult::OK;
}

// SmartShunt and other battery monitors. Sentinels mean not available.
struct ShuntReading {
    uint16_t minutesLeft = 0xFFFF;
    int16_t centivolts = 0x7FFF;
    uint16_t alarm = 0;
    uint16_t aux = 0;
    uint8_t auxMode = 3;               // 0 starter volts, 1 midpoint volts, 2 temperature, 3 none
    int32_t milliamps = 0x3FFFFF;      // Minus = out of the battery.
    uint32_t usedDeciAh = 0xFFFFF;     // Ah used since full, 0.1 Ah, as a positive number.
    uint16_t socDeci = 0x3FF;          // State of charge, 0.1 %.
};

constexpr int32_t SHUNT_NO_CURRENT = 0x3FFFFF;

inline DecodeResult decodeShunt(const uint8_t key[16], const Header& h, ShuntReading* out) {
    uint8_t p[SHUNT_DATA_LEN];
    DecodeResult r = decryptRecord(key, h, READOUT_BATTERY_MONITOR, SHUNT_DATA_LEN, p);
    if (r != DecodeResult::OK) {
        return r;
    }
    BitReader b(p, sizeof(p));
    out->minutesLeft = (uint16_t)b.take(16);
    out->centivolts = (int16_t)b.takeSigned(16);
    out->alarm = (uint16_t)b.take(16);
    out->aux = (uint16_t)b.take(16);
    out->auxMode = (uint8_t)b.take(2);
    // 0x3FFFFF (not available) reads as -1 when signed, so test the raw bits first.
    uint32_t rawAmps = b.take(22);
    out->milliamps = rawAmps == 0x3FFFFF ? SHUNT_NO_CURRENT
                                          : (int32_t)((rawAmps & 0x200000) ? (rawAmps | 0xFFC00000) : rawAmps);
    out->usedDeciAh = b.take(20);
    out->socDeci = (uint16_t)b.take(10);
    return DecodeResult::OK;
}

// Smart BatteryProtect. Sentinels mean not available.
struct ProtectReading {
    uint8_t state = 0xFF;       // 249 active
    uint8_t output = 0xFF;      // 0 shutdown, 1 on, 4 off
    uint8_t error = 0xFF;
    uint16_t alarm = 0;
    uint16_t warning = 0;
    int16_t inCentivolts = 0x7FFF;
    uint16_t outCentivolts = 0xFFFF;
    uint32_t offReason = 0;
};

inline DecodeResult decodeProtect(const uint8_t key[16], const Header& h, ProtectReading* out) {
    uint8_t p[PROTECT_DATA_LEN];
    DecodeResult r = decryptRecord(key, h, READOUT_BATTERY_PROTECT, PROTECT_DATA_LEN, p);
    if (r != DecodeResult::OK) {
        return r;
    }
    BitReader b(p, sizeof(p));
    out->state = (uint8_t)b.take(8);
    out->output = (uint8_t)b.take(8);
    out->error = (uint8_t)b.take(8);
    out->alarm = (uint16_t)b.take(16);
    out->warning = (uint16_t)b.take(16);
    out->inCentivolts = (int16_t)b.takeSigned(16);
    out->outCentivolts = (uint16_t)b.take(16);
    out->offReason = b.take(32);
    return DecodeResult::OK;
}

// Reads 32 hex digits into 16 bytes. Returns false on any other input.
inline bool parseKey(const char* text, uint8_t key[16]) {
    if (text == nullptr || strlen(text) != 32) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        char c = text[i];
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return false;
        }
        if (i % 2 == 0) {
            key[i / 2] = (uint8_t)(v << 4);
        } else {
            key[i / 2] |= (uint8_t)v;
        }
    }
    return true;
}

}  // namespace victron
