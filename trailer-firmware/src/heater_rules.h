// SPDX-License-Identifier: Apache-2.0
/*
 * When the unit may start the heater, and when it must stop a run it started.
 * No Particle code here, so it also runs on a computer: see test/heater_rules_test.cpp.
 *
 * The unit protects only runs it started from the phone (or a run the phone put on a timer).
 * A run started at the heater's own panel is left alone until the phone sends start or stop.
 */
#pragma once

#include <stdint.h>

namespace heaterrules {

// Limits. Change them here; the phone page shows the same numbers.
constexpr int HOT_C100 = 3222;                  // 90 F inside: stop, and do not start.
constexpr int LOW_CV = 1180;                    // 11.80 V: stop if the battery stays below this (default; the phone may change it)...
constexpr uint32_t LOW_HOLD_S = 300;            // ...for 5 minutes in a row...
constexpr uint32_t START_GRACE_S = 600;         // ...not counting the first 10 minutes (glow plug start).
constexpr uint32_t PROBE_LOST_S = 600;          // No inside reading for 10 minutes: stop.
constexpr uint32_t BOOT_GRACE_S = 600;          // After a restart, wait this long for a first probe reading.
constexpr uint32_t CLOCK_WAIT_S = 900;          // After a restart, wait this long for the clock.
constexpr uint32_t RUN_MAX_MIN = 720;           // Longest run the phone can ask for: 12 hours.

// Heater status bytes, after the XOR, from the open AirHeaterBLE projects; state and fault
// are proven on this heater.
constexpr int BYTE_STATE = 3;   // 0 off, 1 on
constexpr int BYTE_STEP = 5;    // 0 standby; others are self-test, ignition, running, cool-down
constexpr int BYTE_FAULT = 35;  // 0 no fault
constexpr int BYTE_MODE = 8;    // 1 level mode
constexpr int BYTE_LEVEL = 10;  // 1 to 10, matches the panel on this heater
constexpr int STEP_RUNNING = 3;
constexpr int STEP_COOLDOWN = 4;  // Steps 2, 3 and 4 seen on this heater on 2026-11-06.
constexpr int LEVEL_MIN = 1;
constexpr int LEVEL_MAX = 10;
constexpr int LEVEL_TRIES = 3;  // Passes of the worker before the unit gives up on a level.

// BATTERY LIMITS for the heater. Design rule: do not choke it down; the BatteryProtect is
// the final protection. So the unit has only stop limits, which the phone sets ("heat" with "bat,...") and
// the unit keeps in flash. They exist so the unit stops the heater with its cool-down before the
// BatteryProtect cuts its power with none. Volts always apply. Charge % comes from the battery monitor
// (SmartShunt); it applies when it is set (not 0) and the monitor gave a fresh charge reading.
// Under a stop limit now: no start (it would only stop again). Under one for 5 minutes while the heater
// runs (after the 10 minute glow plug start): stop. While the thermostat has paused the heater, a low
// battery does not end the run: the restart waits until the battery is back over the limits (sun or a
// charger), and the timer still ends the run.
// With nothing saved: 11.8 V, charge off.
struct Limits {
    int16_t stopCv = LOW_CV;  // Stop below this, 0.01 V.
    int16_t stopPct = 0;      // Stop below this charge %. 0 is off.
};

// What the phone may set. The unit refuses a change outside these, so a slip on the phone cannot let
// the heater run the battery flat.
constexpr int STOP_CV_MIN = 1160;  // 11.6 V. Set the BatteryProtect shut-down below the unit's stop, so
                                   // the unit stops the heater first and it gets its cool-down.
constexpr int STOP_CV_MAX = 1240;  // 12.4 V
constexpr int STOP_PCT_MIN = 10;
constexpr int STOP_PCT_MAX = 80;

inline bool validLimits(const Limits& l) {
    if (l.stopCv < STOP_CV_MIN || l.stopCv > STOP_CV_MAX) {
        return false;
    }
    return l.stopPct == 0 || (l.stopPct >= STOP_PCT_MIN && l.stopPct <= STOP_PCT_MAX);
}

// Messages the unit may send: AA 55, password 1234 as 0C 22, command, value low, value high, checksum.
constexpr int CMD_STATUS = 1;
constexpr int CMD_MODE = 2;   // Value 1 is level mode. Only that value is allowed.
constexpr int CMD_POWER = 3;  // 1 on, 0 off.
constexpr int CMD_LEVEL = 4;  // In level mode, the level 1 to 10. In temperature mode it would set the
                              // temperature, so the unit sends it only after it reads level mode.
constexpr int MODE_LEVEL = 1;

// Builds a message. False, and nothing built, for anything outside the allowed list above.
inline bool buildMessage(uint8_t* out, int command, int value) {
    bool allowed = (command == CMD_STATUS && value == 0) || (command == CMD_POWER && (value == 0 || value == 1)) ||
                   (command == CMD_MODE && value == MODE_LEVEL) ||
                   (command == CMD_LEVEL && value >= LEVEL_MIN && value <= LEVEL_MAX);
    if (!allowed) {
        return false;
    }
    out[0] = 0xAA;
    out[1] = 0x55;
    out[2] = 0x0C;
    out[3] = 0x22;
    out[4] = (uint8_t)command;
    out[5] = (uint8_t)value;
    out[6] = 0;
    out[7] = (uint8_t)((out[2] + out[3] + out[4] + out[5] + out[6]) & 0xFF);
    return true;
}

// The heater takes a level change only when it is steady: running, or off in standby.
// During self-test, ignition and cool-down the unit waits.
inline bool steadyForLevel(const uint8_t* frame) {
    return (frame[BYTE_STATE] != 0 && frame[BYTE_STEP] == STEP_RUNNING) ||
           (frame[BYTE_STATE] == 0 && frame[BYTE_STEP] == 0);
}

// Result of the last phone command, sent in the report as ctl.res.
enum Result : int {
    RES_NONE = 0,
    RES_STARTED = 1,
    RES_TIMER = 2,     // It was already running; the phone's timer now applies.
    RES_STOPPED = 3,
    RES_LEVEL = 4,
    RES_THERMO = 5,    // Thermostat on; the heater may wait if the trailer is already warm.
    RES_MAX_LEVEL = 6, // Thermostat run: highest level changed.
    RES_LIMITS = 7,    // Battery limits saved.
    RES_HEATER_SET = 8, // Heater name saved (or removed, or it was already that name).
    RES_NO_LINK = -1,
    RES_FAULT = -2,
    RES_LOW_BATTERY = -3,
    RES_BUSY = -4,     // Cooling down or starting; not in standby.
    RES_HOT = -5,
    RES_NO_PROBE = -6,
    RES_NO_CLOCK = -7,
    RES_ON_NOT_CONFIRMED = -8,
    RES_OFF_NOT_CONFIRMED = -9,
    RES_LEVEL_NOT_CONFIRMED = -10,
    RES_MODE_NOT_SET = -11,     // The heater would not go to level mode, so no level was sent.
    RES_LOW_CHARGE = -12,       // Battery charge % under the stop limit.
    RES_HEATER_BUSY = -13,      // Heater name not changed: the heater runs, or the box has a run.
    RES_NOT_SAVED = -14,        // A setting could not be saved in flash; the old one stays.
};

// Why the last run ended, sent in the report as ctl.why.
enum Why : int {
    WHY_NONE = 0,
    WHY_APP = 1,       // Stop pressed on the phone.
    WHY_TIME = 2,
    WHY_HOT = 3,
    WHY_PROBE = 4,
    WHY_BATTERY = 5,
    WHY_FAULT = 6,
    WHY_HEATER = 7,    // The heater went off by itself or at its own panel.
    WHY_CLOCK = 8,
    WHY_NO_START = 9,  // Start was not confirmed, so the unit sent stop.
    WHY_CHARGE = 10,   // Battery charge % under the stop limit (volts were not under theirs).
};

struct Readings {
    bool clockValid = false;
    uint32_t now = 0;          // Seconds since 1970, when clockValid.
    uint32_t uptime = 0;       // Seconds since the unit started.
    bool insideKnown = false;  // A probe reading exists.
    int insideC100 = 0;
    uint32_t insideAgeS = 0;
    int aheadC100 = 0;         // Inside change expected over the next 5 minutes (see Trend).
    int batteryCv = -1;        // -1 when no fresh battery reading.
    int chargeDeci = -1;       // Battery monitor charge in 0.1 %; -1 when no fresh charge reading.
};

// True when the battery is under a stop limit. *byCharge is false when volts are under theirs.
inline bool belowStop(const Readings& r, const Limits& lim, bool* byCharge) {
    *byCharge = false;
    if (r.batteryCv >= 0 && r.batteryCv < lim.stopCv) {
        return true;
    }
    if (lim.stopPct != 0 && r.chargeDeci >= 0 && r.chargeDeci < lim.stopPct * 10) {
        *byCharge = true;
        return true;
    }
    return false;
}

struct Run {
    uint32_t endUnix = 0;
    uint32_t startUptime = 0;  // Uptime when the run started, or when the unit restarted.
    uint32_t lowSince = 0;     // Uptime when the battery went low, 0 when it is not low.
};

// 0 if the heater may start, otherwise the refusal. frame is the decoded heater status.
inline int startRefusal(const uint8_t* frame, const Readings& r, const Limits& lim = Limits()) {
    if (!r.clockValid) {
        return RES_NO_CLOCK;
    }
    if (frame[BYTE_FAULT] != 0) {
        return RES_FAULT;
    }
    bool byCharge = false;
    if (belowStop(r, lim, &byCharge)) {
        return byCharge ? RES_LOW_CHARGE : RES_LOW_BATTERY;
    }
    if (!r.insideKnown || r.insideAgeS > PROBE_LOST_S) {
        return RES_NO_PROBE;
    }
    if (r.insideC100 >= HOT_C100) {
        return RES_HOT;
    }
    if (frame[BYTE_STATE] == 0 && frame[BYTE_STEP] != 0) {
        return RES_BUSY;
    }
    return 0;
}

// THERMOSTAT. The inside probe sets the level. It looks 5 minutes ahead: the box measures how fast
// the trailer warms or cools over the last 10 minutes and uses the temperature it will reach.
// Cold (3 F or more below the target, now): the highest level the phone chose, at once.
// Near the target: a straight line from the highest level at 3 F below to level 1 at 1 F above.
// It turns the heater down at once when the line says so, and up only one level at a time, so the
// heat climbs gently. A hold zone of 3/4 of a level stops it from changing back and forth.
// If level 1 still overshoots, the unit pauses the heater (an off message). It starts again early,
// when the trailer will be 1.5 F below the target, at level 1, and climbs one level at a time.
// Every start uses the glow plug, so a pause needs 20 minutes on and a restart 15 minutes off.
constexpr int FULL_BELOW_C100 = 167;    // 3 F or more below the target: highest level.
constexpr int LOW_ABOVE_C100 = 56;      // 1 F or more above the target: level 1.
constexpr int PAUSE_ABOVE_C100 = 222;   // 4 F above the target at level 1, now: pause...
constexpr uint32_t MIN_ON_S = 1200;     // ...but only after 20 minutes of running.
constexpr int RESUME_BELOW_C100 = 83;   // 1.5 F below the target, 5 minutes ahead: start again...
constexpr uint32_t MIN_OFF_S = 900;     // ...but only after 15 minutes off (cool-down is 3 to 5).
constexpr uint32_t LEVEL_GAP_S = 120;   // Change the level at most every 2 minutes.
constexpr int HOLD_X100 = 75;           // Hold zone: 3/4 of a level each way.
constexpr int TARGET_MIN_C10 = 44;      // 40 F
constexpr int TARGET_MAX_C10 = 294;     // 85 F, under the 90 F stop.

// How fast the inside temperature moves. One sample a minute, the last 10 minutes.
constexpr uint32_t LOOK_AHEAD_S = 300;    // Look 5 minutes ahead.
constexpr int AHEAD_MAX_C100 = 167;       // Never look more than 3 F away from now.
constexpr uint32_t TREND_GAP_S = 50;      // Samples at least this far apart.
constexpr uint32_t TREND_BREAK_S = 180;   // A longer hole in the readings starts the trend again.
constexpr uint32_t TREND_MIN_SPAN_S = 300;  // Need 5 minutes of samples before looking ahead.

class Trend {
public:
    static constexpr int SIZE = 11;

    // at is the uptime of the probe reading.
    void add(uint32_t at, int c100) {
        if (count_ > 0) {
            uint32_t last = at_[(next_ + SIZE - 1) % SIZE];
            if (at <= last || at - last < TREND_GAP_S) {
                return;
            }
            if (at - last > TREND_BREAK_S) {
                count_ = 0;
            }
        }
        at_[next_] = at;
        c100_[next_] = c100;
        next_ = (next_ + 1) % SIZE;
        if (count_ < SIZE) {
            count_++;
        }
    }

    // The change in 0.01 C expected over the next 5 minutes. 0 until there are 5 minutes of samples.
    int aheadC100() const {
        if (count_ < 6) {
            return 0;
        }
        int first = (next_ + SIZE - count_) % SIZE;
        uint32_t t0 = at_[first];
        if (at_[(next_ + SIZE - 1) % SIZE] - t0 < TREND_MIN_SPAN_S) {
            return 0;
        }
        // Straight line fitted to all samples, so one probe step does not swing it.
        double mt = 0, mv = 0;
        for (int i = 0; i < count_; i++) {
            int k = (first + i) % SIZE;
            mt += (double)(at_[k] - t0);
            mv += c100_[k];
        }
        mt /= count_;
        mv /= count_;
        double num = 0, den = 0;
        for (int i = 0; i < count_; i++) {
            int k = (first + i) % SIZE;
            double dt = (double)(at_[k] - t0) - mt;
            num += dt * (c100_[k] - mv);
            den += dt * dt;
        }
        if (den <= 0) {
            return 0;
        }
        double ahead = num / den * LOOK_AHEAD_S;
        if (ahead > AHEAD_MAX_C100) {
            return AHEAD_MAX_C100;
        }
        if (ahead < -AHEAD_MAX_C100) {
            return -AHEAD_MAX_C100;
        }
        return (int)(ahead < 0 ? ahead - 0.5 : ahead + 0.5);
    }

private:
    uint32_t at_[SIZE] = {};
    int c100_[SIZE] = {};
    int next_ = 0;
    int count_ = 0;
};

// The level for the heater now at level now. insideC100 is the reading now, aheadC100 the change
// expected over the next 5 minutes.
inline int thermoLevel(int now, int insideC100, int aheadC100, int targetC100, int maxLevel) {
    if (now < 1) {
        now = 1;
    }
    if (now > maxLevel || insideC100 <= targetC100 - FULL_BELOW_C100) {
        return maxLevel;
    }
    int soon = insideC100 + aheadC100;
    int cold = targetC100 - FULL_BELOW_C100;
    int warm = targetC100 + LOW_ABOVE_C100;
    // The level on the line, times 100.
    int x100 = soon <= cold  ? maxLevel * 100
               : soon >= warm ? 100
                              : 100 + (maxLevel - 1) * 100 * (warm - soon) / (warm - cold);
    if (x100 >= now * 100 + HOLD_X100) {
        return now + 1;  // Up: one level at a time.
    }
    if (x100 <= now * 100 - HOLD_X100) {
        int down = (x100 + HOLD_X100) / 100;  // Down: at once.
        return down < 1 ? 1 : down;
    }
    return now;
}

enum ThermoKind { T_NONE, T_LEVEL, T_PAUSE, T_RESUME };

struct ThermoStep {
    ThermoKind kind;
    int level;
};

struct ThermoTimes {
    uint32_t onSince = 0;   // Uptime when the heater last started, or the thermostat took it over.
    uint32_t offSince = 0;  // Uptime when the thermostat paused it.
    uint32_t levelAt = 0;   // Uptime of the last level change.
};

// Called about once a minute, after the safety checks said keep. idle is true while paused.
inline ThermoStep thermo(const uint8_t* frame, bool fresh, bool idle, const Readings& r, int targetC100, int maxLevel,
                         const ThermoTimes& t) {
    if (!r.insideKnown || r.insideAgeS > PROBE_LOST_S) {
        return {T_NONE, 0};
    }
    if (idle) {
        bool cool = r.insideC100 + r.aheadC100 <= targetC100 - RESUME_BELOW_C100;
        return cool && r.uptime - t.offSince >= MIN_OFF_S ? ThermoStep{T_RESUME, 0} : ThermoStep{T_NONE, 0};
    }
    if (!fresh || frame[BYTE_STATE] == 0 || frame[BYTE_STEP] != STEP_RUNNING) {
        return {T_NONE, 0};  // Starting, cooling, or no new reading: wait.
    }
    int now = frame[BYTE_LEVEL];
    if (now <= 1 && r.insideC100 >= targetC100 + PAUSE_ABOVE_C100 && r.uptime - t.onSince >= MIN_ON_S) {
        return {T_PAUSE, 0};
    }
    int want = thermoLevel(now, r.insideC100, r.aheadC100, targetC100, maxLevel);
    if (want != now && r.uptime - t.levelAt >= LEVEL_GAP_S) {
        return {T_LEVEL, want};
    }
    return {T_NONE, 0};
}

// What the unit does about a run it started. Called about once a minute.
enum Action { KEEP, END, STOP };

struct Decision {
    Action action;
    int why;
};

// fresh is true when frame was read from the heater since the last call. idle is true while the
// thermostat has paused the heater: then off is expected and the battery stop is off; the timer, probe,
// heat, clock and fault stops still apply.
inline Decision check(const uint8_t* frame, bool fresh, const Readings& r, Run* run, bool idle = false,
                      const Limits& lim = Limits()) {
    if (fresh && frame[BYTE_STATE] == 0 && !idle) {
        // Already off: nothing to send.
        return {END, frame[BYTE_FAULT] != 0 ? WHY_FAULT : WHY_HEATER};
    }
    if (fresh && frame[BYTE_FAULT] != 0) {
        return {STOP, WHY_FAULT};
    }
    if (!r.clockValid) {
        return r.uptime > CLOCK_WAIT_S ? Decision{STOP, WHY_CLOCK} : Decision{KEEP, WHY_NONE};
    }
    if (r.now >= run->endUnix) {
        return {STOP, WHY_TIME};
    }
    bool probeLost = r.insideKnown ? r.insideAgeS > PROBE_LOST_S : r.uptime > BOOT_GRACE_S;
    if (probeLost) {
        return {STOP, WHY_PROBE};
    }
    if (r.insideKnown && r.insideC100 >= HOT_C100) {
        return {STOP, WHY_HOT};
    }
    // Only while it runs: a paused heater draws nothing, and its restart waits for the battery instead.
    bool byCharge = false;
    bool low = !idle && belowStop(r, lim, &byCharge) && r.uptime - run->startUptime >= START_GRACE_S;
    if (!low) {
        run->lowSince = 0;
    } else if (run->lowSince == 0) {
        run->lowSince = r.uptime;
    } else if (r.uptime - run->lowSince >= LOW_HOLD_S) {
        return {STOP, byCharge ? WHY_CHARGE : WHY_BATTERY};
    }
    return {KEEP, WHY_NONE};
}

// A thermostat restart refused for this reason waits, and the run stays paused: still cooling down, or
// the battery under a stop limit. Any other refusal ends the run.
inline bool resumeWaits(int refusal) {
    return refusal == RES_BUSY || refusal == RES_LOW_BATTERY || refusal == RES_LOW_CHARGE;
}

// True when the reply to an off message shows the heater is stopping.
inline bool offConfirmed(const uint8_t* frame) {
    return frame[BYTE_STATE] == 0 || frame[BYTE_STEP] == STEP_COOLDOWN;
}

}  // namespace heaterrules
