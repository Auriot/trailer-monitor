// SPDX-License-Identifier: Apache-2.0
/*
 * Counts what a unit sends each month and slows it down before it uses its share.
 * All units share one plan limit; if the account reaches it, every unit stops.
 * Particle's plan limits: https://www.particle.io/pricing/
 *
 * No Particle code here, so it also runs on a computer: see test/budget_test.cpp.
 * The program saves counts() in flash and sets the month from the clock.
 *
 * The byte count is an estimate. A unit cannot read its real cellular use, and this does
 * not count program loads or connection upkeep, so leave a spare share on the account.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

class DataBudget {
public:
    enum Level { kNormal = 0, kSlow = 1, kStop = 2 };

    struct Counts {
        uint32_t magic;
        int32_t month;  // Such as 202611. 0 when the clock was not set.
        uint32_t ops;
        uint32_t bytes;
    };

    static const uint32_t kMagic = 0x42554447;
    static const uint32_t kOpBytes = 1024;
    static const uint32_t kOverheadBytes = 200;  // Estimate for each event and its reply.
    static const uint32_t kSlowPercent = 80;
    static const uint32_t kSlowFactor = 10;
    static const uint32_t kDayMs = 24UL * 60 * 60 * 1000;
    static const uint32_t kSaveEveryOps = 25;

    DataBudget(uint32_t opsShare, uint32_t bytesShare)
        : opsShare_(opsShare), bytesShare_(bytesShare), unsaved_(0) {
        counts_.magic = kMagic;
        counts_.month = 0;
        counts_.ops = 0;
        counts_.bytes = 0;
    }

    // Takes counts read from flash. New or damaged flash does not have the magic number.
    void restore(const Counts& saved) {
        if (saved.magic == kMagic) {
            counts_ = saved;
        }
    }

    const Counts& counts() const { return counts_; }

    // Call with the current month when the clock is valid. Returns true if counts changed.
    bool setMonth(int32_t month) {
        if (month == counts_.month) {
            return false;
        }
        if (counts_.month != 0) {
            counts_.ops = 0;
            counts_.bytes = 0;
        }
        counts_.month = month;
        unsaved_ = kSaveEveryOps;
        return true;
    }

    // Call once for each event sent, with the name and data length together.
    void spend(size_t eventBytes) {
        const uint32_t ops = eventBytes == 0 ? 1 : (uint32_t)((eventBytes + kOpBytes - 1) / kOpBytes);
        counts_.ops += ops;
        counts_.bytes += (uint32_t)eventBytes + kOverheadBytes;
        unsaved_ += ops;
    }

    // True when the program should save counts() to flash.
    bool saveDue() const { return unsaved_ >= kSaveEveryOps; }
    void markSaved() { unsaved_ = 0; }

    Level level() const {
        const uint32_t percent = usedPercent();
        if (percent >= 100) {
            return kStop;
        }
        return percent >= kSlowPercent ? kSlow : kNormal;
    }

    // The higher of messages and bytes used, as a percent of this unit's share.
    uint32_t usedPercent() const {
        const uint32_t ops = percentOf(counts_.ops, opsShare_);
        const uint32_t bytes = percentOf(counts_.bytes, bytesShare_);
        return ops > bytes ? ops : bytes;
    }

    // Turns a normal report gap into the gap to use now.
    uint32_t gapMs(uint32_t normalMs) const {
        switch (level()) {
            case kNormal:
                return normalMs;
            case kSlow:
                return normalMs * kSlowFactor;
            default:
                return kDayMs;
        }
    }

private:
    static uint32_t percentOf(uint32_t used, uint32_t share) {
        return share == 0 ? 100 : (uint32_t)(((uint64_t)used * 100) / share);
    }

    uint32_t opsShare_;
    uint32_t bytesShare_;
    uint32_t unsaved_;
    Counts counts_;
};
