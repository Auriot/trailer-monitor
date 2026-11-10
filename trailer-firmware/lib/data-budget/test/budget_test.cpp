// SPDX-License-Identifier: Apache-2.0
/*
 * Build and run from this library's folder (the one that holds src/ and test/):
 *   g++ -std=c++11 -Wall -Isrc test/budget_test.cpp -o <out> && <out>
 */
#include <stdio.h>

#include "data_budget.h"

static int failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL line %d: %s\n", __LINE__, #cond);         \
            failures++;                                            \
        }                                                          \
    } while (0)

int main() {
    // A small event is one message. Each started 1024 bytes is one more.
    {
        DataBudget b(1000, 1000000);
        b.spend(1);
        CHECK(b.counts().ops == 1);
        b.spend(1024);
        CHECK(b.counts().ops == 2);
        b.spend(1025);
        CHECK(b.counts().ops == 4);
        CHECK(b.counts().bytes == 1 + 1024 + 1025 + 3 * DataBudget::kOverheadBytes);
    }

    // Messages drive the level: normal, slow at 80 percent, stop at 100 percent.
    {
        DataBudget b(100, 100000000);
        CHECK(b.level() == DataBudget::kNormal);
        CHECK(b.gapMs(60000) == 60000);
        for (int i = 0; i < 79; i++) b.spend(100);
        CHECK(b.level() == DataBudget::kNormal);
        b.spend(100);
        CHECK(b.level() == DataBudget::kSlow);
        CHECK(b.gapMs(60000) == 600000);
        for (int i = 0; i < 19; i++) b.spend(100);
        CHECK(b.level() == DataBudget::kSlow);
        b.spend(100);
        CHECK(b.level() == DataBudget::kStop);
        CHECK(b.gapMs(60000) == DataBudget::kDayMs);
    }

    // Bytes alone also drive it.
    {
        DataBudget b(100000, 10000);
        b.spend(1000);
        for (int i = 0; i < 6; i++) b.spend(1000);
        CHECK(b.usedPercent() == 84);
        CHECK(b.level() == DataBudget::kSlow);
        b.spend(1000);
        b.spend(1000);
        CHECK(b.level() == DataBudget::kStop);
    }

    // A new month clears the counts. The first clock reading keeps them.
    {
        DataBudget b(100, 100000);
        b.spend(10);
        CHECK(b.setMonth(202611));
        CHECK(b.counts().ops == 1);
        CHECK(b.counts().month == 202611);
        CHECK(!b.setMonth(202611));
        b.spend(10);
        CHECK(b.counts().ops == 2);
        CHECK(b.setMonth(202612));
        CHECK(b.counts().ops == 0);
        CHECK(b.counts().bytes == 0);
        CHECK(b.saveDue());
    }

    // Flash: good counts come back. New or damaged flash is ignored.
    {
        DataBudget a(100, 100000);
        a.setMonth(202611);
        a.spend(10);
        DataBudget::Counts saved = a.counts();

        DataBudget b(100, 100000);
        b.restore(saved);
        CHECK(b.counts().ops == 1);
        CHECK(b.counts().month == 202611);

        saved.magic = 0xFFFFFFFF;
        DataBudget c(100, 100000);
        c.restore(saved);
        CHECK(c.counts().ops == 0);
        CHECK(c.counts().month == 0);
    }

    // Save timing.
    {
        DataBudget b(1000, 1000000);
        for (uint32_t i = 0; i + 1 < DataBudget::kSaveEveryOps; i++) b.spend(10);
        CHECK(!b.saveDue());
        b.spend(10);
        CHECK(b.saveDue());
        b.markSaved();
        CHECK(!b.saveDue());
    }

    // A share of zero never allows normal sending.
    {
        DataBudget b(0, 0);
        CHECK(b.level() == DataBudget::kStop);
    }

    // Large counts do not wrap.
    {
        DataBudget b(4000000000UL, 4000000000UL);
        DataBudget::Counts big = b.counts();
        big.ops = 3900000000UL;
        big.bytes = 100;
        b.restore(big);
        CHECK(b.usedPercent() == 97);
        CHECK(b.level() == DataBudget::kSlow);
    }

    printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
