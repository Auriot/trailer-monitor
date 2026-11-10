// SPDX-License-Identifier: Apache-2.0
/*
 * Movement guard: the box guards the trailer where it has stood still 30 minutes and makes an
 * alert when it moves 100 m or more, or when it runs on its own battery (12 V lost).
 * Plain C++ with no Particle code, so the same file also runs in a test on a computer
 * (test/guard_rules_test.cpp). trailer.cpp feeds it a GPS fix every 5 s, sends the alerts it makes,
 * asks for a position when follow says one is due, and saves its state when it changes.
 *
 * Times: "now" is seconds since the box started (never jumps). "unix" is the clock, 0 while it is
 * not set; it is used only for the daily cap and to carry a follow across a restart.
 * Coordinates stay on the box: they are in its flash file only, never in a report or an event.
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace guardrules {

constexpr double MOVED_M = 100;           // Same 100 m as the phone page.
constexpr double STILL_M = 50;            // Good fixes this close to the anchor count as still.
constexpr uint32_t STILL_S = 30 * 60;     // Still this long starts the guard.
constexpr uint32_t TOW_STILL_S = 120 * 60;  // After "I'm towing" with no move of 100 m or more.
constexpr uint32_t MAX_GAP_S = 10 * 60;   // A longer gap between good fixes restarts the still clock.
constexpr float GOOD_ACC_M = 25;          // A fix counts only with accuracy this good or better.
constexpr int MOVED_FIXES = 3;            // Moved fixes in a row to call it moved.
constexpr int CAP_PER_DAY = 5;            // Alerts a UTC day, all kinds together.
constexpr uint32_t FOLLOW_S = 2 * 60 * 60;  // Positions for this long after a moved alert.
constexpr uint32_t FOLLOW_EVERY_S = 2 * 60;
constexpr uint32_t POWER_LOST_S = 2 * 60;   // On its own battery this long: "12 V lost".
constexpr uint32_t POWER_BACK_S = 10 * 60;  // 12 V back this long arms "12 V lost" again.
constexpr int POWER_BATTERY = 5;            // System.powerSource(): internal battery.
constexpr int POWER_UNKNOWN = 0;
constexpr int SHIFT_MAX_M = 1000;
constexpr size_t QUEUE_LEN = 3;
constexpr uint32_t FORMAT = 0x47440001;  // Change this if Saved changes.
constexpr double METERS_PER_DEG_LAT = 111320.0;

enum Mode : uint8_t { WAITING = 0, GUARDING = 1, TOWING = 2 };
enum Kind : uint8_t { NONE = 0, MOVED = 1, POWER = 2 };

// Command results, shown in the report as gd.res.
constexpr int RES_OK = 1;
constexpr int RES_BAD = -1;
constexpr int RES_REFUSED = -2;

struct Fix {
    bool locked = false;
    bool stable = false;
    double lat = 0;
    double lon = 0;
    float acc = 0;  // Horizontal accuracy in meters.
};

struct Alert {
    uint32_t n;
    uint8_t kind;
    uint8_t pad[3];
};
static_assert(sizeof(Alert) == 8, "Alert is 8 bytes");

// The flash file /trailer/guard.bin. The alert number is in its own file, so a change here can
// never make a number repeat.
struct Saved {
    uint32_t format;
    uint8_t mode;
    uint8_t haveTowSpot;
    uint8_t towLeft;
    uint8_t powerArmed;
    double guardLat;
    double guardLon;
    double towLat;
    double towLon;
    uint32_t day;  // UTC day of dayCount, 0 if the clock was not set.
    uint32_t dayCount;
    uint32_t followUntilUnix;  // 0: no follow, or the clock was not set when it started.
    uint32_t queueLen;
    Alert queue[QUEUE_LEN];
};
static_assert(sizeof(Saved) == 80, "Change FORMAT if Saved changes");

enum CommandKind { CMD_BAD = 0, CMD_TOW, CMD_TEST_GUARD, CMD_TEST_SHIFT, CMD_TEST_CAP };

struct Command {
    CommandKind kind = CMD_BAD;
    int value = 0;
};

// "tow", "test,guard", "test,shift,<1 to 1000>", "test,cap,<1 to 5>".
inline Command parseCommand(const char* text) {
    Command c;
    if (text == nullptr) {
        return c;
    }
    if (strcmp(text, "tow") == 0) {
        c.kind = CMD_TOW;
    } else if (strcmp(text, "test,guard") == 0) {
        c.kind = CMD_TEST_GUARD;
    } else if (strncmp(text, "test,shift,", 11) == 0 || strncmp(text, "test,cap,", 9) == 0) {
        bool shift = text[5] == 's';
        const char* digits = text + (shift ? 11 : 9);
        char* end = nullptr;
        long v = strtol(digits, &end, 10);
        int high = shift ? SHIFT_MAX_M : CAP_PER_DAY;
        if (end != digits && *end == 0 && v >= 1 && v <= high) {
            c.kind = shift ? CMD_TEST_SHIFT : CMD_TEST_CAP;
            c.value = (int)v;
        }
    }
    return c;
}

inline double metersBetween(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0, rad = M_PI / 180.0;
    double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
    double h = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * rad) * cos(lat2 * rad) * sin(dLon / 2) * sin(dLon / 2);
    if (h > 1) {
        h = 1;
    }
    return 2 * R * asin(sqrt(h));
}

// Locked, with an accuracy the box can trust.
inline bool usable(const Fix& f) { return f.locked && f.acc > 0 && f.acc <= GOOD_ACC_M; }
// Good enough to say the trailer stands still, or to set the guard spot. The GPS library's
// "stable" flag is NOT used: it means the last 5 accuracy readings spread under 5 % of their mean,
// which at 0.8 m accuracy is 4 cm, so it flickers. On the real box (2026-11-10, accuracy 0.79 m)
// it gave no stable fix for 10 minutes and the guard could not start.
inline bool good(const Fix& f) { return usable(f); }

struct Guard {
    Saved s = {};
    uint32_t nextN = 1;
    int cap = CAP_PER_DAY;  // Lowered only by test,cap; a restart puts it back.
    int lastResult = 0;
    uint32_t dropped = 0;   // Alerts not made: over the cap or the queue full.
    uint32_t lastN = 0;     // Last alert made since start, 0 if none.
    uint32_t lastAt = 0;

    // Still clock.
    bool haveAnchor = false;
    double anchorLat = 0, anchorLon = 0;
    uint32_t clockStart = 0;
    // Last good fix.
    bool haveGood = false;
    double goodLat = 0, goodLon = 0;
    uint32_t goodAt = 0;
    // Last usable fix (for the distance in the report).
    bool haveUsable = false;
    uint32_t usableAt = 0;
    double usableMeters = 0;

    int movedCount = 0;
    bool lostTiming = false, backTiming = false;
    uint32_t lostSince = 0, backSince = 0;
    bool following = false;
    uint32_t followUntil = 0, nextFollowAt = 0;

    bool dirty = false;   // Saved needs writing.
    bool nDirty = false;  // nextN needs writing.

    Guard() { reset(); }

    void reset() {
        s = Saved();
        s.format = FORMAT;
        s.mode = WAITING;
        s.powerArmed = 1;
    }

    // savedOk: the file was read with the right size. nOk: the number file was read. seedN: the
    // first number to use when it was not (the clock, or a random number).
    void load(const Saved& saved, bool savedOk, uint32_t savedN, bool nOk, uint32_t seedN) {
        if (savedOk && saved.format == FORMAT && saved.mode <= TOWING && saved.queueLen <= QUEUE_LEN) {
            s = saved;
        } else {
            reset();
        }
        if (nOk && savedN != 0) {
            nextN = savedN;
        } else {
            nextN = seedN != 0 ? seedN : 1;
            nDirty = true;
        }
    }

    Mode mode() const { return (Mode)s.mode; }

    void setDay(uint32_t unix) {
        uint32_t day = unix / 86400;
        // Day 0 is "not known": it never resets the count, so a restart cannot reset the cap.
        if (unix != 0 && day > s.day) {
            s.day = day;
            s.dayCount = 0;
            dirty = true;
        }
    }

    // Makes an alert unless the cap or a full queue stops it. True if made.
    bool makeAlert(Kind kind, uint32_t now, uint32_t unix) {
        setDay(unix);
        if (s.queueLen >= QUEUE_LEN || s.dayCount >= (uint32_t)cap) {
            dropped++;
            return false;
        }
        Alert a = {};
        a.n = nextN++;
        if (nextN == 0) {
            nextN = 1;
        }
        a.kind = kind;
        s.queue[s.queueLen++] = a;
        s.dayCount++;
        lastN = a.n;
        lastAt = now;
        dirty = true;
        nDirty = true;
        return true;
    }

    bool peek(Alert* a) const {
        if (s.queueLen == 0) {
            return false;
        }
        *a = s.queue[0];
        return true;
    }

    // The first waiting alert reached the cloud.
    void sent() {
        if (s.queueLen == 0) {
            return;
        }
        for (uint32_t i = 1; i < s.queueLen; i++) {
            s.queue[i - 1] = s.queue[i];
        }
        s.queueLen--;
        s.queue[s.queueLen] = Alert();
        dirty = true;
    }

    void startGuard(double lat, double lon) {
        s.mode = GUARDING;
        s.guardLat = lat;
        s.guardLon = lon;
        s.haveTowSpot = 0;
        s.towLeft = 0;
        movedCount = 0;
        dirty = true;
    }

    void startFollow(uint32_t now, uint32_t unix) {
        following = true;
        followUntil = now + FOLLOW_S;
        nextFollowAt = now;
        s.followUntilUnix = unix != 0 ? unix + FOLLOW_S : 0;
        dirty = true;
    }

    void stillClock(const Fix& f, uint32_t now) {
        // Order matters: the gap is measured from the last good fix before this one.
        bool gap = haveGood && now - goodAt > MAX_GAP_S;
        if (!haveAnchor || gap || metersBetween(anchorLat, anchorLon, f.lat, f.lon) > STILL_M) {
            haveAnchor = true;
            anchorLat = f.lat;
            anchorLon = f.lon;
            clockStart = now;
        }
        uint32_t needed = (s.mode == TOWING && !s.towLeft) ? TOW_STILL_S : STILL_S;
        if (now - clockStart >= needed) {
            startGuard(anchorLat, anchorLon);
        }
    }

    void power(int source, uint32_t now, uint32_t unix) {
        if (source == POWER_BATTERY) {
            backTiming = false;
            if (!lostTiming) {
                lostTiming = true;
                lostSince = now;
            }
            if (s.powerArmed && s.mode != TOWING && now - lostSince >= POWER_LOST_S) {
                makeAlert(POWER, now, unix);
                s.powerArmed = 0;  // One per loss, even if the cap stopped it.
                dirty = true;
            }
        } else if (source != POWER_UNKNOWN) {
            lostTiming = false;
            if (!backTiming) {
                backTiming = true;
                backSince = now;
            }
            if (!s.powerArmed && now - backSince >= POWER_BACK_S) {
                s.powerArmed = 1;
                dirty = true;
            }
        }
    }

    // Every 5 s. fix is null when the GPS could not be read.
    void tick(const Fix* fix, int powerSource, uint32_t now, uint32_t unix) {
        setDay(unix);
        // A follow carried across a restart: resume it once the clock is set.
        if (!following && s.followUntilUnix != 0 && unix != 0) {
            if (s.followUntilUnix > unix) {
                following = true;
                followUntil = now + (s.followUntilUnix - unix);
                nextFollowAt = now;
            } else {
                s.followUntilUnix = 0;
                dirty = true;
            }
        }
        power(powerSource, now, unix);
        if (fix == nullptr || !usable(*fix)) {
            return;
        }
        const Fix& f = *fix;
        if (s.mode == GUARDING) {
            double d = metersBetween(s.guardLat, s.guardLon, f.lat, f.lon);
            haveUsable = true;
            usableAt = now;
            usableMeters = d;
            if (d >= MOVED_M + f.acc) {
                movedCount++;
            } else {
                movedCount = 0;
            }
            if (movedCount >= MOVED_FIXES) {
                makeAlert(MOVED, now, unix);
                s.mode = WAITING;
                haveAnchor = false;
                movedCount = 0;
                startFollow(now, unix);
            }
        } else if (s.mode == TOWING) {
            if (!s.haveTowSpot) {
                s.haveTowSpot = 1;
                s.towLat = f.lat;
                s.towLon = f.lon;
                dirty = true;
            } else if (!s.towLeft && metersBetween(s.towLat, s.towLon, f.lat, f.lon) >= MOVED_M + f.acc) {
                s.towLeft = 1;
                haveAnchor = false;  // The 30 minutes count from the next good fix.
                dirty = true;
            }
        }
        if (good(f)) {
            if (s.mode != GUARDING) {
                stillClock(f, now);
            }
            haveGood = true;
            goodLat = f.lat;
            goodLon = f.lon;
            goodAt = now;
        }
    }

    // A good fix in the last MAX_GAP_S.
    bool freshGood(uint32_t now) const { return haveGood && now - goodAt <= MAX_GAP_S; }

    int command(const Command& c, uint32_t now) {
        int res = RES_OK;
        switch (c.kind) {
            case CMD_TOW:
                // Tow spot: the guard spot, else the last good fix, else the next usable fix.
                if (s.mode == GUARDING) {
                    s.haveTowSpot = 1;
                    s.towLat = s.guardLat;
                    s.towLon = s.guardLon;
                } else if (s.mode != TOWING || !s.haveTowSpot) {
                    s.haveTowSpot = freshGood(now) ? 1 : 0;
                    s.towLat = goodLat;
                    s.towLon = goodLon;
                }
                s.mode = TOWING;
                s.towLeft = 0;
                movedCount = 0;
                clockStart = now;  // The 2 hours count from the tap.
                break;
            case CMD_TEST_GUARD:
                if (freshGood(now)) {
                    startGuard(goodLat, goodLon);
                } else {
                    res = RES_REFUSED;
                }
                break;
            case CMD_TEST_SHIFT:
                if (s.mode == GUARDING) {
                    s.guardLat += c.value / METERS_PER_DEG_LAT;
                    movedCount = 0;
                } else if (s.mode == TOWING && s.haveTowSpot) {
                    s.towLat += c.value / METERS_PER_DEG_LAT;
                } else {
                    res = RES_REFUSED;
                }
                break;
            case CMD_TEST_CAP:
                cap = c.value;
                s.dayCount = 0;
                break;
            default:
                res = RES_BAD;
        }
        if (res == RES_OK) {
            dirty = true;
        }
        lastResult = res;
        return res;
    }

    // True when a position is due while following.
    bool followDue(uint32_t now) {
        if (!following) {
            return false;
        }
        if (now >= followUntil) {
            following = false;
            s.followUntilUnix = 0;
            dirty = true;
            return false;
        }
        if (now >= nextFollowAt) {
            nextFollowAt = now + FOLLOW_EVERY_S;
            return true;
        }
        return false;
    }

    // Seconds on the still clock (waiting or towing), -1 when it is not running.
    int stillSeconds(uint32_t now) const {
        if (s.mode == GUARDING || !haveAnchor || !freshGood(now)) {
            return -1;
        }
        return (int)(now - clockStart);
    }

    // Whole meters from the guard spot, -1 when not guarding or no usable fix in the last minute.
    int metersFromGuard(uint32_t now) const {
        if (s.mode != GUARDING || !haveUsable || now - usableAt > 60) {
            return -1;
        }
        return (int)(usableMeters + 0.5);
    }
};

}  // namespace guardrules
