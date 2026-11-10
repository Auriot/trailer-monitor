// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from trailer-firmware/:
 *   g++ -std=c++11 -Wall -Isrc test/guard_rules_test.cpp -o <out> && <out>
 * All positions are made up (a point in the ocean), not a real place.
 */
#include <stdio.h>
#include <string.h>

#include "guard_rules.h"

using namespace guardrules;

static int failures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            failures++;                                    \
        }                                                  \
    } while (0)

static const double LAT0 = 10.0;
static const double LON0 = -30.0;
static const uint32_t DAY1 = 20000 * 86400u;  // Some UTC midnight.

// A fix this many meters north and east of the made-up point.
static Fix at(double north, double east = 0, float acc = 1.5f, bool stable = true, bool locked = true) {
    Fix f;
    f.locked = locked;
    f.stable = stable;
    f.lat = LAT0 + north / METERS_PER_DEG_LAT;
    f.lon = LON0 + east / (METERS_PER_DEG_LAT * cos(LAT0 * M_PI / 180.0));
    f.acc = acc;
    return f;
}

// Feeds the same fix every 5 s for the given seconds, on 12 V. Returns the new "now".
static uint32_t run(Guard& g, const Fix& f, uint32_t now, uint32_t seconds, uint32_t unixStart = DAY1 + 3600, int power = 1) {
    for (uint32_t t = 0; t < seconds; t += 5) {
        g.tick(&f, power, now + t, unixStart + (now + t));
    }
    return now + seconds;
}

static int queued(const Guard& g) { return (int)g.s.queueLen; }

static void distance() {
    double d = metersBetween(LAT0, LON0, at(100).lat, at(100).lon);
    CHECK(d > 99.5 && d < 100.5);
    d = metersBetween(LAT0, LON0, at(0, 100).lat, at(0, 100).lon);
    CHECK(d > 99.5 && d < 100.5);
}

static void parsing() {
    CHECK(parseCommand("tow").kind == CMD_TOW);
    CHECK(parseCommand("test,guard").kind == CMD_TEST_GUARD);
    Command c = parseCommand("test,shift,150");
    CHECK(c.kind == CMD_TEST_SHIFT && c.value == 150);
    CHECK(parseCommand("test,shift,0").kind == CMD_BAD);
    CHECK(parseCommand("test,shift,1001").kind == CMD_BAD);
    CHECK(parseCommand("test,shift,15x").kind == CMD_BAD);
    CHECK(parseCommand("test,shift,").kind == CMD_BAD);
    c = parseCommand("test,cap,2");
    CHECK(c.kind == CMD_TEST_CAP && c.value == 2);
    CHECK(parseCommand("test,cap,6").kind == CMD_BAD);
    CHECK(parseCommand("Tow").kind == CMD_BAD);
    CHECK(parseCommand("").kind == CMD_BAD);
    CHECK(parseCommand(nullptr).kind == CMD_BAD);
}

static void noFixNoGuard() {
    Guard g;
    for (uint32_t t = 0; t < 4 * 3600; t += 5) {
        g.tick(nullptr, 1, t, DAY1 + t);
    }
    CHECK(g.mode() == WAITING);
    Fix unlocked = at(0, 0, 1.5f, true, false);
    Fix rough = at(0, 0, 200.0f);
    Fix noAcc = at(0, 0, 0.0f);
    Guard a, c, d;
    run(a, unlocked, 0, 4 * 3600);
    run(c, rough, 0, 4 * 3600);
    run(d, noAcc, 0, 4 * 3600);
    CHECK(a.mode() == WAITING && c.mode() == WAITING && d.mode() == WAITING);
    CHECK(a.stillSeconds(4 * 3600) == -1);
    // The library's "stable" flag does not matter (it flickers at good accuracy).
    Guard b;
    run(b, at(0, 0, 0.8f, false), 0, STILL_S + 5);
    CHECK(b.mode() == GUARDING);
}

// Jitter of 1 to 2 m every 5 s: guards at 30 minutes, not before.
static uint32_t guardWithJitter(Guard& g, uint32_t now) {
    for (uint32_t t = 0; t < STILL_S; t += 5) {
        double j = ((t / 5) % 3) * 0.7;  // 0, 0.7, 1.4 m
        Fix f = at(j, -j);
        g.tick(&f, 1, now + t, DAY1 + 3600 + now + t);
    }
    return now + STILL_S;
}

static void autoGuard() {
    Guard g;
    uint32_t now = guardWithJitter(g, 0);
    CHECK(g.mode() == WAITING);  // Last fix was at 1795 s on the clock.
    CHECK(g.stillSeconds(now - 5) == (int)STILL_S - 5);
    Fix f = at(0.5);
    g.tick(&f, 1, now, DAY1 + 3600 + now);
    CHECK(g.mode() == GUARDING);
    CHECK(g.dirty);
    CHECK(queued(g) == 0);
    CHECK(g.metersFromGuard(now) == -1);  // The guarding fix itself is not a usable "while guarding" fix yet.
    g.tick(&f, 1, now + 5, DAY1 + 3600 + now + 5);
    CHECK(g.metersFromGuard(now + 5) >= 0 && g.metersFromGuard(now + 5) <= 3);
    CHECK(g.metersFromGuard(now + 70) == -1);
}

static void wanderRestartsClock() {
    Guard g;
    uint32_t now = run(g, at(0), 0, 20 * 60);
    now = run(g, at(60), now, 20 * 60);  // 60 m away: a new anchor.
    CHECK(g.mode() == WAITING);
    now = run(g, at(60), now, 10 * 60 + 5);
    CHECK(g.mode() == GUARDING);
}

static void gapRestartsClock() {
    Guard g;
    uint32_t now = run(g, at(0), 0, 20 * 60);
    now += 11 * 60;  // No fix for 11 minutes.
    now = run(g, at(0), now, 20 * 60);
    CHECK(g.mode() == WAITING);
    Guard h;
    now = run(h, at(0), 0, 20 * 60);
    now += 9 * 60;  // A 9 minute gap is fine.
    now = run(h, at(0), now, 2 * 60);
    CHECK(h.mode() == GUARDING);
}

static uint32_t guarded(Guard& g) {
    uint32_t now = run(g, at(0), 0, STILL_S + 5);
    CHECK(g.mode() == GUARDING);
    return now;
}

static void moved() {
    Guard g;
    g.nextN = 7;
    uint32_t now = guarded(g);
    // Two fixes 150 m away, then one back: no alert.
    Fix far = at(150, 0, 3.0f, false);  // Not stable: still counts for a move.
    Fix home = at(1);
    g.tick(&far, 1, now, DAY1 + now);
    g.tick(&far, 1, now + 5, DAY1 + now + 5);
    g.tick(&home, 1, now + 10, DAY1 + now + 10);
    CHECK(queued(g) == 0 && g.mode() == GUARDING);
    // 105 m with 10 m accuracy is not far enough to count.
    Fix edge = at(105, 0, 10.0f, false);
    for (int i = 0; i < 5; i++) g.tick(&edge, 1, now + 15 + 5 * i, DAY1 + now + 15 + 5 * i);
    CHECK(queued(g) == 0);
    // Rough fixes between moved ones do not reset the count.
    Fix rough = at(500, 0, 80.0f);
    now += 100;
    g.tick(&far, 1, now, DAY1 + now);
    g.tick(&rough, 1, now + 5, DAY1 + now + 5);
    g.tick(&far, 1, now + 10, DAY1 + now + 10);
    CHECK(queued(g) == 0);
    g.tick(&far, 1, now + 15, DAY1 + now + 15);
    CHECK(queued(g) == 1);
    Alert a;
    CHECK(g.peek(&a) && a.n == 7 && a.kind == MOVED);
    CHECK(g.nextN == 8 && g.nDirty);
    CHECK(g.mode() == WAITING);
    CHECK(g.following);
    // More moved fixes: no second alert.
    now = run(g, at(300, 0, 3.0f, false), now + 20, 10 * 60);
    CHECK(queued(g) == 1);
    // Parked 30 minutes at the new place: guards there. The next move makes alert 8.
    now = run(g, at(2000), now, STILL_S + 5);
    CHECK(g.mode() == GUARDING);
    now = run(g, at(2200), now, 15);
    CHECK(queued(g) == 2);
    CHECK(g.s.queue[1].n == 8);
    g.sent();
    CHECK(queued(g) == 1 && g.peek(&a) && a.n == 8);
    g.sent();
    CHECK(queued(g) == 0 && !g.peek(&a));
}

static void follow() {
    Guard g;
    uint32_t now = guarded(g);
    now = run(g, at(500), now, 15);
    CHECK(g.following);
    uint32_t start = now - 5;  // The alert came on the third fix.
    int due = 0;
    uint32_t firstDue = 0, lastDue = 0;
    for (uint32_t t = start; t < start + FOLLOW_S + 600; t += 5) {
        if (g.followDue(t)) {
            if (due == 0) firstDue = t;
            lastDue = t;
            due++;
        }
    }
    CHECK(firstDue == start);
    CHECK(due == (int)(FOLLOW_S / FOLLOW_EVERY_S));
    CHECK(lastDue < start + FOLLOW_S);
    CHECK(!g.following && g.s.followUntilUnix == 0);
}

static void followAcrossRestart() {
    Guard g;
    uint32_t now = guarded(g);
    now = run(g, at(500), now, 15, DAY1);
    uint32_t until = g.s.followUntilUnix;
    CHECK(until != 0);
    Saved saved = g.s;
    // Restart 10 minutes later; the clock is not set for the first minute.
    Guard r;
    r.load(saved, true, g.nextN, true, 0);
    r.tick(nullptr, 1, 5, 0);
    CHECK(!r.following);
    uint32_t unix = until - FOLLOW_S + 600;
    r.tick(nullptr, 1, 60, unix);
    CHECK(r.following);
    CHECK(r.followDue(60));
    CHECK(r.followUntil == 60 + FOLLOW_S - 600);
    // After the follow ended: not resumed.
    Guard late;
    late.load(saved, true, g.nextN, true, 0);
    late.tick(nullptr, 1, 5, until + 10);
    CHECK(!late.following && late.s.followUntilUnix == 0);
}

static void cap() {
    Guard g;
    uint32_t now = 0;
    int made = 0;
    for (int i = 0; i < 7; i++) {
        if (g.makeAlert(MOVED, now, DAY1 + 100)) made++;
        if (g.s.queueLen == QUEUE_LEN) g.sent();
    }
    CHECK(made == CAP_PER_DAY);
    CHECK(g.dropped == 2);
    // A restart with the clock not set yet keeps the count.
    Guard r;
    r.load(g.s, true, g.nextN, true, 0);
    CHECK(!r.makeAlert(POWER, 0, 0));
    r.tick(nullptr, 1, 5, 0);
    CHECK(r.s.dayCount == (uint32_t)CAP_PER_DAY);
    // Same day once the clock is set: still capped.
    CHECK(!r.makeAlert(POWER, 10, DAY1 + 200));
    // Next UTC day: allowed again.
    while (r.s.queueLen) r.sent();
    CHECK(r.makeAlert(POWER, 20, DAY1 + 86400 + 5));
    CHECK(r.s.dayCount == 1);
    // An earlier day (clock trouble) does not reset it.
    r.setDay(DAY1 + 10);
    CHECK(r.s.dayCount == 1);
}

static void queueFull() {
    Guard g;
    g.nextN = 50;
    CHECK(g.makeAlert(MOVED, 0, DAY1));
    CHECK(g.makeAlert(MOVED, 0, DAY1));
    CHECK(g.makeAlert(MOVED, 0, DAY1));
    CHECK(!g.makeAlert(MOVED, 0, DAY1));  // Full: no number used, no cap slot.
    CHECK(g.nextN == 53 && g.s.dayCount == 3);
}

static void towing() {
    // Tap, then no move: guard back after 2 hours, not 30 minutes.
    Guard g;
    uint32_t now = guarded(g);
    CHECK(g.command(parseCommand("tow"), now) == RES_OK);
    CHECK(g.mode() == TOWING && g.s.haveTowSpot);
    now = run(g, at(1), now, STILL_S + 60);
    CHECK(g.mode() == TOWING);
    CHECK(queued(g) == 0);
    now = run(g, at(1), now, TOW_STILL_S - STILL_S);
    CHECK(g.mode() == GUARDING);
    CHECK(queued(g) == 0);

    // Tap, tow 5 km, park 30 minutes: guards at the new spot. No alert on the way.
    Guard h;
    now = guarded(h);
    h.command(parseCommand("tow"), now);
    for (int i = 1; i <= 100; i++) {
        Fix f = at(50.0 * i, 0, 4.0f, false);
        h.tick(&f, 1, now, DAY1 + now);
        now += 5;
    }
    CHECK(h.s.towLeft && h.mode() == TOWING && queued(h) == 0);
    now = run(h, at(5000), now, STILL_S + 5);
    CHECK(h.mode() == GUARDING);
    CHECK(queued(h) == 0);
    now = run(h, at(5200), now, 15);
    CHECK(queued(h) == 1);  // Guarding at the new spot.

    // Tow with no fix at the tap: the tow spot comes from the next usable fix.
    Guard k;
    k.command(parseCommand("tow"), 0);
    CHECK(!k.s.haveTowSpot);
    Fix f = at(0, 0, 5.0f, false);
    k.tick(&f, 1, 5, DAY1 + 5);
    CHECK(k.s.haveTowSpot && !k.s.towLeft);

    // Towing blocks "12 V lost".
    Guard p;
    p.command(parseCommand("tow"), 0);
    run(p, at(0), 0, 10 * 60, DAY1, POWER_BATTERY);
    CHECK(queued(p) == 0);
}

static void powerLost() {
    Guard g;
    Fix f = at(0);
    uint32_t now = 0;
    for (; now < POWER_LOST_S; now += 5) g.tick(&f, POWER_BATTERY, now, DAY1 + now);
    CHECK(queued(g) == 0);  // 115 s: not yet.
    g.tick(&f, POWER_BATTERY, now, DAY1 + now);
    CHECK(queued(g) == 1);
    Alert a;
    CHECK(g.peek(&a) && a.kind == POWER);
    CHECK(!g.s.powerArmed);
    now = run(g, f, now + 5, 10 * 60, DAY1, POWER_BATTERY);
    CHECK(queued(g) == 1);  // One per loss.
    // Unknown source changes nothing.
    now = run(g, f, now, 20 * 60, DAY1, POWER_UNKNOWN);
    CHECK(!g.s.powerArmed);
    // Back for 9 minutes 55 s, lost again: none.
    now = run(g, f, now, POWER_BACK_S - 5, DAY1, 1);
    CHECK(!g.s.powerArmed);
    now = run(g, f, now, 5 * 60, DAY1, POWER_BATTERY);
    CHECK(queued(g) == 1);
    // Back 10 minutes: armed again; the next loss alerts.
    now = run(g, f, now, POWER_BACK_S + 5, DAY1, 1);
    CHECK(g.s.powerArmed);
    now = run(g, f, now, POWER_LOST_S + 5, DAY1, POWER_BATTERY);
    CHECK(queued(g) == 2);
    // A restart on battery does not alert again: the disarm is saved.
    Guard r;
    r.load(g.s, true, g.nextN, true, 0);
    run(r, f, 0, 10 * 60, DAY1, POWER_BATTERY);
    CHECK(queued(r) == 2);
}

static void testCommands() {
    Guard g;
    CHECK(g.command(parseCommand("test,guard"), 0) == RES_REFUSED);  // No fix.
    CHECK(g.lastResult == RES_REFUSED);
    CHECK(g.command(parseCommand("test,shift,150"), 0) == RES_REFUSED);  // Not guarding.
    CHECK(g.command(parseCommand("nonsense"), 0) == RES_BAD);
    uint32_t now = run(g, at(0), 0, 60);
    CHECK(g.command(parseCommand("test,guard"), now) == RES_OK);
    CHECK(g.mode() == GUARDING);
    // Stale fix: refused.
    Guard s;
    run(s, at(0), 0, 60);
    CHECK(s.command(parseCommand("test,guard"), 60 + MAX_GAP_S + 5) == RES_REFUSED);
    // Shift 150 m: the next 3 fixes at the same real place read as moved.
    CHECK(g.command(parseCommand("test,shift,150"), now) == RES_OK);
    now = run(g, at(0), now, 10);
    CHECK(queued(g) == 0);
    now = run(g, at(0), now, 5);
    CHECK(queued(g) == 1);
    // Cap 2 also zeroes today's count: exactly 2 more, then none.
    CHECK(g.command(parseCommand("test,cap,2"), now) == RES_OK);
    g.sent();
    int made = 0;
    for (int i = 0; i < 3; i++) {
        g.command(parseCommand("test,guard"), now);
        g.command(parseCommand("test,shift,150"), now);
        size_t before = g.s.queueLen;
        now = run(g, at(0), now, 15);
        if (g.s.queueLen > before) made++;
        while (g.s.queueLen) g.sent();
    }
    CHECK(made == 2);
    // A restart puts the cap back.
    Guard r;
    r.load(g.s, true, g.nextN, true, 0);
    CHECK(r.cap == CAP_PER_DAY);
    // Shift while towing moves the tow spot: towLeft is set and no alert.
    Guard t;
    now = run(t, at(0), 0, 60);
    t.command(parseCommand("tow"), now);
    CHECK(t.s.haveTowSpot);
    CHECK(t.command(parseCommand("test,shift,150"), now) == RES_OK);
    now = run(t, at(0), now, 15);
    CHECK(t.s.towLeft && queued(t) == 0 && t.mode() == TOWING);
}

static void saving() {
    Guard g;
    g.nextN = 1234;
    uint32_t now = guarded(g);
    g.makeAlert(POWER, now, DAY1 + now);
    Saved copy;
    memcpy(&copy, &g.s, sizeof(copy));
    Guard r;
    r.load(copy, true, g.nextN, true, 99);
    CHECK(r.mode() == GUARDING && r.s.guardLat == g.s.guardLat && r.s.guardLon == g.s.guardLon);
    CHECK(r.s.queueLen == 1 && r.s.queue[0].n == 1234 && r.s.queue[0].kind == POWER);
    CHECK(r.nextN == 1235 && !r.nDirty);
    // Bad file: defaults, but the number file still holds.
    Saved bad = copy;
    bad.format = 0x12345678;
    Guard b;
    b.load(bad, true, 1235, true, 99);
    CHECK(b.mode() == WAITING && b.s.queueLen == 0 && b.s.powerArmed && b.nextN == 1235);
    // Wrong size, and a bad queue length: defaults.
    Guard w;
    w.load(copy, false, 1235, true, 99);
    CHECK(w.mode() == WAITING);
    bad = copy;
    bad.queueLen = 9;
    Guard q;
    q.load(bad, true, 1235, true, 99);
    CHECK(q.s.queueLen == 0);
    // No number file: starts from the seed and marks it for saving.
    Guard n;
    n.load(copy, true, 0, false, 1700000000);
    CHECK(n.nextN == 1700000000 && n.nDirty);
    Guard z;
    z.load(copy, true, 0, false, 0);
    CHECK(z.nextN == 1);
    // A guarded box stays guarded after a restart and alerts on a move.
    Guard m;
    m.load(copy, true, 2000, true, 0);
    run(m, at(400, 0, 3.0f, false), 0, 15);
    CHECK(m.s.queueLen == 2 && m.s.queue[1].n == 2000 && m.s.queue[1].kind == MOVED);
}

int main() {
    distance();
    parsing();
    noFixNoGuard();
    autoGuard();
    wanderRestartsClock();
    gapRestartsClock();
    moved();
    follow();
    followAcrossRestart();
    cap();
    queueFull();
    towing();
    powerLost();
    testCommands();
    saving();
    if (failures == 0) {
        printf("guard_rules: all passed\n");
        return 0;
    }
    printf("guard_rules: %d failed\n", failures);
    return 1;
}
