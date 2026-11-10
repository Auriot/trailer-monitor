// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from trailer-firmware/:
 *   g++ -std=c++11 -Wall -Isrc test/heater_rules_test.cpp -o <out> && <out>
 */
#include <stdio.h>
#include <string.h>

#include "heater_rules.h"

using namespace heaterrules;

static int failures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            failures++;                                    \
        }                                                  \
    } while (0)

struct Frame {
    uint8_t b[48];
    Frame(int state, int step, int fault) {
        memset(b, 0, sizeof(b));
        b[BYTE_STATE] = state;
        b[BYTE_STEP] = step;
        b[BYTE_FAULT] = fault;
    }
};

// A good day: clock set, 60 F inside, 12.6 V, unit up for an hour.
static Readings good() {
    Readings r;
    r.clockValid = true;
    r.now = 1000000;
    r.uptime = 3600;
    r.insideKnown = true;
    r.insideC100 = 1556;
    r.insideAgeS = 30;
    r.batteryCv = 1260;
    return r;
}

static Run running(uint32_t startUptime) {
    Run run;
    run.endUnix = 1000000 + 3600;
    run.startUptime = startUptime;
    return run;
}

static void testStart() {
    Frame standby(0, 0, 0);
    Readings r = good();
    CHECK(startRefusal(standby.b, r) == 0);

    Frame on(1, 3, 0);
    CHECK(startRefusal(on.b, r) == 0);  // Already running: the phone's timer takes over.

    Frame cooling(0, 4, 0);
    CHECK(startRefusal(cooling.b, r) == RES_BUSY);
    Frame afterOff(0, 1, 0);  // Real trace after an off message.
    CHECK(startRefusal(afterOff.b, r) == RES_BUSY);

    Frame fault(0, 0, 10);
    CHECK(startRefusal(fault.b, r) == RES_FAULT);

    r = good(); r.clockValid = false;
    CHECK(startRefusal(standby.b, r) == RES_NO_CLOCK);
    // Design rule: no separate start limit; under the stop limit (11.8 V default) refuses.
    r = good(); r.batteryCv = 1179;
    CHECK(startRefusal(standby.b, r) == RES_LOW_BATTERY);
    r = good(); r.batteryCv = 1180;
    CHECK(startRefusal(standby.b, r) == 0);
    r = good(); r.batteryCv = -1;  // No battery reading: the heater's own low-volt cut still applies.
    CHECK(startRefusal(standby.b, r) == 0);
    r = good(); r.insideKnown = false;
    CHECK(startRefusal(standby.b, r) == RES_NO_PROBE);
    r = good(); r.insideAgeS = 601;
    CHECK(startRefusal(standby.b, r) == RES_NO_PROBE);
    r = good(); r.insideC100 = 3222;
    CHECK(startRefusal(standby.b, r) == RES_HOT);
    r = good(); r.insideC100 = 3221;
    CHECK(startRefusal(standby.b, r) == 0);
}

static void testCheck() {
    Frame on(1, 3, 0);
    Readings r = good();
    Run run = running(0);
    Decision d = check(on.b, true, r, &run);
    CHECK(d.action == KEEP);

    // The heater went off on its own, or at its panel.
    Frame off(0, 0, 0);
    d = check(off.b, true, r, &run);
    CHECK(d.action == END && d.why == WHY_HEATER);
    Frame offFault(0, 0, 2);
    d = check(offFault.b, true, r, &run);
    CHECK(d.action == END && d.why == WHY_FAULT);
    // An old frame that says off does not end the run.
    d = check(off.b, false, r, &run);
    CHECK(d.action == KEEP);

    Frame onFault(1, 2, 1);
    d = check(onFault.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_FAULT);

    // Timer.
    r = good(); r.now = run.endUnix - 1;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    r.now = run.endUnix;
    d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_TIME);
    // The timer still works when the heater does not answer.
    d = check(on.b, false, r, &run);
    CHECK(d.action == STOP && d.why == WHY_TIME);

    // Too hot.
    r = good(); r.insideC100 = 3222;
    d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_HOT);

    // Probe lost, and the wait after a restart.
    r = good(); r.insideAgeS = 601;
    d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_PROBE);
    r = good(); r.insideKnown = false; r.uptime = 120;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    r.uptime = 601;
    d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_PROBE);

    // Clock: wait after a restart, then stop.
    r = good(); r.clockValid = false; r.uptime = 100;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    r.uptime = 901;
    d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_CLOCK);
}

static void testBattery() {
    Frame on(1, 3, 0);
    Run run = running(1000);
    Readings r = good();
    r.batteryCv = 1150;
    // Inside the first 10 minutes: the glow plug start is ignored.
    for (uint32_t t = 1000; t < 1600; t += 60) {
        r.uptime = t;
        CHECK(check(on.b, true, r, &run).action == KEEP);
    }
    CHECK(run.lowSince == 0);
    // Low from 1600: stop at 1900, not before.
    r.uptime = 1600;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    CHECK(run.lowSince == 1600);
    r.uptime = 1840;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    r.uptime = 1900;
    Decision d = check(on.b, true, r, &run);
    CHECK(d.action == STOP && d.why == WHY_BATTERY);

    // One good reading starts the 5 minutes again.
    run = running(0);
    r = good(); r.batteryCv = 1150; r.uptime = 4000;
    check(on.b, true, r, &run);
    r.uptime = 4200; r.batteryCv = 1185;
    check(on.b, true, r, &run);
    CHECK(run.lowSince == 0);
    r.uptime = 4260; r.batteryCv = 1150;
    check(on.b, true, r, &run);
    r.uptime = 4500;
    CHECK(check(on.b, true, r, &run).action == KEEP);
    r.uptime = 4560;
    CHECK(check(on.b, true, r, &run).action == STOP);

    // No battery reading: no battery stop.
    run = running(0);
    r = good(); r.batteryCv = -1; r.uptime = 9000;
    check(on.b, true, r, &run);
    r.uptime = 9999;
    CHECK(check(on.b, true, r, &run).action == KEEP);
}

static void testOffConfirmed() {
    Frame off(0, 1, 0);
    CHECK(offConfirmed(off.b));
    Frame cooling(1, 4, 0);
    CHECK(offConfirmed(cooling.b));
    Frame on(1, 3, 0);
    CHECK(!offConfirmed(on.b));
}

static void testMessages() {
    // The three messages proven on a real heater in a bench test.
    const uint8_t status[8] = {0xAA, 0x55, 0x0C, 0x22, 0x01, 0x00, 0x00, 0x2F};
    const uint8_t on[8] = {0xAA, 0x55, 0x0C, 0x22, 0x03, 0x01, 0x00, 0x32};
    const uint8_t off[8] = {0xAA, 0x55, 0x0C, 0x22, 0x03, 0x00, 0x00, 0x31};
    uint8_t m[8];
    CHECK(buildMessage(m, CMD_STATUS, 0) && memcmp(m, status, 8) == 0);
    CHECK(buildMessage(m, CMD_POWER, 1) && memcmp(m, on, 8) == 0);
    CHECK(buildMessage(m, CMD_POWER, 0) && memcmp(m, off, 8) == 0);
    CHECK(buildMessage(m, CMD_LEVEL, 5) && m[4] == 4 && m[5] == 5 && m[6] == 0 && m[7] == ((0x0C + 0x22 + 4 + 5) & 0xFF));
    CHECK(buildMessage(m, CMD_LEVEL, 1) && buildMessage(m, CMD_LEVEL, 10));
    CHECK(buildMessage(m, CMD_MODE, 1) && m[4] == 2 && m[5] == 1);
    // Not allowed: nothing built.
    memset(m, 0x77, 8);
    CHECK(!buildMessage(m, CMD_LEVEL, 0) && m[0] == 0x77);
    CHECK(!buildMessage(m, CMD_LEVEL, 11));
    CHECK(!buildMessage(m, CMD_MODE, 2));  // Temperature mode.
    CHECK(!buildMessage(m, CMD_POWER, 2));
    CHECK(!buildMessage(m, 9, 0));         // Altitude and every other setting.
    CHECK(!buildMessage(m, CMD_STATUS, 1));
}

static void testSteady() {
    CHECK(steadyForLevel(Frame(1, 3, 0).b));
    CHECK(steadyForLevel(Frame(0, 0, 0).b));
    CHECK(!steadyForLevel(Frame(1, 2, 0).b));  // Igniting.
    CHECK(!steadyForLevel(Frame(1, 1, 0).b));  // Self-test.
    CHECK(!steadyForLevel(Frame(0, 4, 0).b));  // Cooling down.
    CHECK(!steadyForLevel(Frame(0, 1, 0).b));
}

static const int F1 = 56;  // 1 F in 0.01 C, rounded.

static void testThermoLevel() {
    int target = 1833;  // 65 F
    // Cold now: the highest level at once, whatever the trend says.
    CHECK(thermoLevel(1, target - FULL_BELOW_C100, 0, target, 8) == 8);
    CHECK(thermoLevel(3, target - 3 * F1 - 10, 150, target, 8) == 8);
    // The phone lowered the highest level: down to it at once.
    CHECK(thermoLevel(9, target, 0, target, 4) == 4);
    // Warm: down at once, not one level at a time.
    CHECK(thermoLevel(8, target + LOW_ABOVE_C100, 0, target, 8) == 1);
    CHECK(thermoLevel(8, target, 0, target, 8) == 3);
    // Cooling: up only one level at a time.
    CHECK(thermoLevel(1, target - 120, 0, target, 10) == 2);
    CHECK(thermoLevel(2, target - 120, 0, target, 10) == 3);
    // Hold zone: a small move does not change the level.
    // Level 4 sits on the line at 18 hundredths below the target; the zone is about a third of a F.
    CHECK(thermoLevel(4, target - 18, 0, target, 10) == 4);
    CHECK(thermoLevel(4, target - 18 + 10, 0, target, 10) == 4);
    CHECK(thermoLevel(4, target - 18 - 10, 0, target, 10) == 4);
    CHECK(thermoLevel(4, target - 18 + 30, 0, target, 10) == 3);
    CHECK(thermoLevel(4, target - 18 - 30, 0, target, 10) == 5);
    // Looking ahead: warming fast turns it down sooner than the same reading with no trend.
    CHECK(thermoLevel(10, target - 160, 0, target, 10) == 10);
    CHECK(thermoLevel(10, target - 160, 150, target, 10) == 4);
    // Cooling fast turns it up sooner.
    CHECK(thermoLevel(1, target + 50, 0, target, 10) == 1);
    CHECK(thermoLevel(1, target + 50, -150, target, 10) == 2);
    // Level 1 only: never anything else.
    CHECK(thermoLevel(1, target - 100, -150, target, 1) == 1);
    // Never outside 1 to the highest level, and never more than one level up.
    for (int now = 0; now <= 10; now++) {
        for (int c = target - 400; c <= target + 400; c += 7) {
            for (int ah = -167; ah <= 167; ah += 167) {
                int l = thermoLevel(now, c, ah, target, 10);
                CHECK(l >= 1 && l <= 10);
                CHECK(c <= target - FULL_BELOW_C100 || l <= (now < 1 ? 1 : now) + 1);
            }
        }
    }
}

static void testTrend() {
    Trend t;
    CHECK(t.aheadC100() == 0);
    // Warming 0.10 C a minute: 0.50 C over the next 5 minutes, once there are 5 minutes of samples.
    for (int i = 0; i < 5; i++) {
        t.add(1000 + 60 * i, 1500 + 10 * i);
    }
    CHECK(t.aheadC100() == 0);
    t.add(1300, 1550);
    CHECK(t.aheadC100() == 50);
    // Samples too close together are skipped.
    t.add(1310, 3000);
    CHECK(t.aheadC100() == 50);
    // One probe step (6 hundredths) on the last sample barely moves it.
    t.add(1360, 1566);
    int a = t.aheadC100();
    CHECK(a >= 50 && a <= 56);
    // Keeps only the last 10 minutes: a long steady spell ends the warming.
    for (int i = 0; i < 11; i++) {
        t.add(1420 + 60 * i, 1700);
    }
    CHECK(t.aheadC100() == 0);
    // Cooling fast: capped at 3 F.
    for (int i = 0; i < 11; i++) {
        t.add(2200 + 60 * i, 1700 - 100 * i);
    }
    CHECK(t.aheadC100() == -AHEAD_MAX_C100);
    // A hole of more than 3 minutes starts again.
    t.add(3000, 1000);
    CHECK(t.aheadC100() == 0);
}

static void testThermo() {
    int target = 1833;
    Readings r = good();
    r.uptime = 10000;
    ThermoTimes t;
    t.onSince = 5000;
    t.levelAt = 5000;
    Frame running(1, 3, 0);
    running.b[BYTE_LEVEL] = 8;

    // Cold: stays at 8.
    r.insideC100 = 1500;
    CHECK(thermo(running.b, true, false, r, target, 8, t).kind == T_NONE);
    // Near the target: lower, but only 5 minutes after the last change.
    r.insideC100 = target;
    ThermoStep s = thermo(running.b, true, false, r, target, 8, t);
    CHECK(s.kind == T_LEVEL && s.level < 8 && s.level > 1);
    t.levelAt = r.uptime - 100;
    CHECK(thermo(running.b, true, false, r, target, 8, t).kind == T_NONE);
    t.levelAt = 0;
    // No new reading, or not running yet: wait.
    CHECK(thermo(running.b, false, false, r, target, 8, t).kind == T_NONE);
    Frame igniting(1, 2, 0);
    CHECK(thermo(igniting.b, true, false, r, target, 8, t).kind == T_NONE);

    // Warm at level 1: pause after 20 minutes of running, not before.
    Frame low(1, 3, 0);
    low.b[BYTE_LEVEL] = 1;
    r.insideC100 = target + PAUSE_ABOVE_C100;
    t.onSince = r.uptime - 600;
    CHECK(thermo(low.b, true, false, r, target, 8, t).kind == T_NONE);
    t.onSince = r.uptime - MIN_ON_S;
    CHECK(thermo(low.b, true, false, r, target, 8, t).kind == T_PAUSE);
    // Warm but still above level 1: lower first.
    s = thermo(running.b, true, false, r, target, 8, t);
    CHECK(s.kind == T_LEVEL && s.level == 1);

    // Paused: start again only when 3 F below and 15 minutes off.
    Frame off(0, 0, 0);
    t.offSince = r.uptime - MIN_OFF_S;
    r.insideC100 = target - RESUME_BELOW_C100 + 1;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_NONE);
    r.insideC100 = target - RESUME_BELOW_C100;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_RESUME);
    // Cooling fast: starts earlier, from the temperature 5 minutes ahead.
    r.insideC100 = target - RESUME_BELOW_C100 + 40;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_NONE);
    r.aheadC100 = -50;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_RESUME);
    r.aheadC100 = 0;
    r.insideC100 = target - RESUME_BELOW_C100;
    t.offSince = r.uptime - 600;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_NONE);
    // No probe: never start.
    t.offSince = 0;
    r.insideAgeS = 601;
    CHECK(thermo(off.b, true, true, r, target, 8, t).kind == T_NONE);
}

static void testIdleCheck() {
    Frame off(0, 0, 0);
    Readings r = good();
    Run run = running(0);
    // Paused by the thermostat: off is expected.
    CHECK(check(off.b, true, r, &run, true).action == KEEP);
    // The timer, the probe and a fault still end it.
    r.now = run.endUnix;
    CHECK(check(off.b, true, r, &run, true).action == STOP);
    r = good(); r.insideAgeS = 601;
    CHECK(check(off.b, true, r, &run, true).action == STOP);
    Frame fault(0, 0, 5);
    r = good();
    CHECK(check(fault.b, true, r, &run, true).action == STOP);
    // Not paused: off ends the run as before.
    CHECK(check(off.b, true, r, &run, false).action == END);
    // Paused: a low battery (volts or charge) never ends the run; the restart waits for it instead.
    Limits lim; lim.stopCv = 1200; lim.stopPct = 30;
    run = running(0);
    r = good(); r.batteryCv = 1100; r.chargeDeci = 50;
    for (uint32_t t = 1000; t <= 9000; t += 60) {
        r.uptime = t;
        CHECK(check(off.b, true, r, &run, true, lim).action == KEEP);
    }
    CHECK(run.lowSince == 0);
    // Restart refusals: busy and battery wait; the others end the run.
    CHECK(resumeWaits(RES_BUSY) && resumeWaits(RES_LOW_BATTERY) && resumeWaits(RES_LOW_CHARGE));
    CHECK(!resumeWaits(RES_FAULT) && !resumeWaits(RES_HOT) && !resumeWaits(RES_NO_PROBE) && !resumeWaits(RES_NO_CLOCK));
    CHECK(!resumeWaits(0));
}

// Battery stop limits the phone sets: what the unit takes, start refusals, and stops by volts or charge.
static void testLimits() {
    Limits def;
    CHECK(def.stopCv == 1180 && def.stopPct == 0);
    CHECK(validLimits(def));

    Limits l;
    l.stopCv = 1159; CHECK(!validLimits(l));   // Under 11.6 V.
    l.stopCv = 1160; CHECK(validLimits(l));
    l.stopCv = 1240; CHECK(validLimits(l));
    l.stopCv = 1241; CHECK(!validLimits(l));
    l.stopCv = -5; CHECK(!validLimits(l));
    l = Limits(); l.stopPct = 9; CHECK(!validLimits(l));
    l.stopPct = 10; CHECK(validLimits(l));
    l.stopPct = 80; CHECK(validLimits(l));
    l.stopPct = 81; CHECK(!validLimits(l));

    // Start: refused only when already under a stop limit, volts or charge.
    Limits lim;
    lim.stopCv = 1200; lim.stopPct = 30;
    Frame standby(0, 0, 0);
    Readings r = good(); r.batteryCv = 1200; r.chargeDeci = 300;
    CHECK(startRefusal(standby.b, r, lim) == 0);   // At the limits is fine.
    r.batteryCv = 1199;
    CHECK(startRefusal(standby.b, r, lim) == RES_LOW_BATTERY);
    r.batteryCv = 1250; r.chargeDeci = 299;
    CHECK(startRefusal(standby.b, r, lim) == RES_LOW_CHARGE);
    r.batteryCv = 1199; r.chargeDeci = 299;        // Both low: volts named first.
    CHECK(startRefusal(standby.b, r, lim) == RES_LOW_BATTERY);
    r.batteryCv = 1250; r.chargeDeci = -1;         // No charge reading: volts alone.
    CHECK(startRefusal(standby.b, r, lim) == 0);
    Limits voltsOnly; voltsOnly.stopCv = 1200;
    r.chargeDeci = 100;                            // Charge off: a low charge does not refuse.
    CHECK(startRefusal(standby.b, r, voltsOnly) == 0);
    r = good(); r.chargeDeci = 0;                  // The defaults ignore charge.
    CHECK(startRefusal(standby.b, r) == 0);

    // Stop by charge while running: after the 10 minute grace, 5 minutes in a row.
    Frame on(1, 3, 0);
    Run run = running(0);
    r = good(); r.batteryCv = 1250; r.chargeDeci = 299; r.uptime = 599;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP && run.lowSince == 0);
    r.uptime = 600;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP && run.lowSince == 600);
    r.uptime = 899;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP);
    r.uptime = 900;
    Decision d = check(on.b, true, r, &run, false, lim);
    CHECK(d.action == STOP && d.why == WHY_CHARGE);

    // Charge back to the limit restarts the 5 minutes; a lost charge reading is not low.
    run = running(0);
    r = good(); r.batteryCv = 1250; r.chargeDeci = 299; r.uptime = 1000;
    check(on.b, true, r, &run, false, lim);
    r.chargeDeci = 300; r.uptime = 1100;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP && run.lowSince == 0);
    r.chargeDeci = -1; r.uptime = 1500;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP && run.lowSince == 0);

    // Stop by the phone's volts; volts low wins the reason.
    run = running(0);
    r = good(); r.batteryCv = 1199; r.chargeDeci = 299; r.uptime = 1000;
    check(on.b, true, r, &run, false, lim);
    r.uptime = 1300;
    d = check(on.b, true, r, &run, false, lim);
    CHECK(d.action == STOP && d.why == WHY_BATTERY);
    // At the stop limit itself is not low.
    run = running(0);
    r = good(); r.batteryCv = 1200; r.chargeDeci = 700; r.uptime = 1000;
    check(on.b, true, r, &run, false, lim);
    r.uptime = 1400;
    CHECK(check(on.b, true, r, &run, false, lim).action == KEEP);
    // A lower phone stop than the default lets it run on: 11.7 V is fine with an 11.6 V stop.
    Limits low; low.stopCv = 1160;
    run = running(0);
    r = good(); r.batteryCv = 1170; r.uptime = 1000;
    check(on.b, true, r, &run, false, low);
    r.uptime = 2000;
    CHECK(check(on.b, true, r, &run, false, low).action == KEEP);
    // A charge stop at the default limits never happens: charge is off.
    run = running(0);
    r = good(); r.chargeDeci = 0; r.uptime = 1000;
    check(on.b, true, r, &run);
    r.uptime = 1400;
    CHECK(check(on.b, true, r, &run).action == KEEP);
}

int main() {
    testThermoLevel();
    testTrend();
    testThermo();
    testIdleCheck();
    testMessages();
    testSteady();
    testStart();
    testCheck();
    testBattery();
    testLimits();
    testOffConfirmed();
    printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
