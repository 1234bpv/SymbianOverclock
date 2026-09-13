// Measurement and stress tests, shared by the application and by the boot-time
// applier. Split out of ocgui.cpp unchanged: ocboot re-runs the same tests at
// the end of a soak period, and a second, separately written copy of them
// would be a second set of bugs.
#include <e32std.h>
#include <hal.h>
#include "octests.h"

// --- instruments: each is both a stress test and a benchmark ----------------
//
// Main requirement for all: SELF-VERIFICATION. Under overclocking, what matters is not speed, but
// the moment when a subsystem begins to silently fail, and this can only be observed
// with a test having a known answer. Therefore:
//   * ALU runs LCG forward, and then exactly the same number of steps backward via
//     the modular inverse multiplier - any single bit error breaks the return
//     to the initial value;
//   * memory and cache write a pseudorandom pattern and verify it back.
// Neither the former nor the latter require a reference taken at another frequency.

const TUint32 KLcgA = 1664525u;
const TUint32 KLcgC = 1013904223u;

// Modular inverse of KLcgA modulo 2^32 (Newton iteration; for an odd
// multiplier it always exists). Computed rather than hardcoded: a hand-written constant
// would turn a correctness test into a test of my arithmetic.
static TUint32 LcgInverse()
    {
    TUint32 inv = 1;
    for (TInt i = 0; i < 6; ++i)
        inv = inv * (2u - KLcgA * inv);
    return inv;
    }

static TUint32 gFcFreq = 32768;

static TUint32 ElapsedUs(TUint32 aStart, TUint32 aEnd)
    {
    TUint32 d = aEnd - aStart;
    if (!gFcFreq)
        return 0;
    return (TUint32)(((TUint64)d * 1000000u) / gFcFreq);
    }

// Chain of independent additions: 256 one-cycle instructions plus counter and
// branch. The number of cycles per iteration is known exactly, so time is divided by it
// and yields the ABSOLUTE core frequency - regardless of what the system reports.
// This is why assembly is used here: in C the compiler is allowed to rewrite the loop arbitrarily.
const TUint32 KNopCyclesPerIter = 258;      // 256 add + subs + bne

static void SpinNop(TUint32 aIter)
    {
    __asm__ __volatile__(
        "1:                  \n"
        ".rept 256           \n"
        "   add r3, r3, #1   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "cc");
    }

// LCG core in assembly: 32 pairs of multiplication and addition per iteration, i.e. exactly
// 64 operations. Needed to show the operation cost in cycles: in the C variant this
// figure is swallowed by the loop counter and constant reloads.
static void SpinLcgAsm(TUint32 aIter)
    {
    __asm__ __volatile__(
        "   mov r4, #1       \n"
        "   mov r5, #7       \n"
        "1:                  \n"
        ".rept 32            \n"
        "   mul r3, r4, r3   \n"
        "   add r3, r3, r5   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "r4", "r5", "cc");
    }

// Absolute core frequency in MHz.
TUint32 MeasureCoreMhz()
    {
    const TUint32 kIter = 400000;
    TUint32 t0 = User::FastCounter();
    SpinNop(kIter);
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    return us ? (TUint32)(((TUint64)kIter * KNopCyclesPerIter) / us) : 0;
    }

// Cost of one LCG core operation in hundredths of a cycle.
TUint32 MeasureLcgCycles100(TUint32 aMhz)
    {
    const TUint32 kIter = 100000;
    TUint32 t0 = User::FastCounter();
    SpinLcgAsm(kIter);
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    const TUint64 ops = (TUint64)kIter * 64u;
    const TUint32 psPer = ops ? (TUint32)(((TUint64)us * 1000000u) / ops) : 0;
    return (TUint32)(((TUint64)psPer * aMhz) / 10000u);
    }

// ALU: forward and backward. Returns Mitr/s in hundredths and error count (0 or 1).
TTestResult TestAlu(TUint32 aIter)
    {
    TTestResult r;
    const TUint32 seed = 0x12345678u;
    const TUint32 ainv = LcgInverse();

    TUint32 t0 = User::FastCounter();
    TUint32 x = seed;
    TUint32 i;
    for (i = 0; i < aIter; ++i)
        x = x * KLcgA + KLcgC;
    for (i = 0; i < aIter; ++i)
        x = (x - KLcgC) * ainv;
    TUint32 t1 = User::FastCounter();

    TUint32 us = ElapsedUs(t0, t1);
    r.iValue = us ? (TUint32)(((TUint64)aIter * 2u * 100u) / us) : 0;
    r.iErrors = (x == seed) ? 0 : 1;
    return r;
    }

// Memory bandwidth: sequential read of a buffer larger than cache.
//
// FIXED 2026-09-12. Previously there was a traversal with a 64-byte step, and read
// bytes were counted as "reads x 64", i.e. assuming a cache line of
// 64 bytes. Stepping sweep on the device (findings 46.1) showed a plateau already
// at a 32-byte step, meaning the line is twice as short, and the number came out exactly
// twice as high.
//
// Now there is no assumption at all: EVERY word is read, and the buffer size goes into
// bytes. The loop is unrolled by eight, because in rolled form the measurement hit
// its own loop counter and branch rather than memory.
TTestResult TestMemBandwidth(TUint32* aBuf, TInt aWords, TInt aPasses)
    {
    TTestResult r;
    const TInt n = aWords & ~7;
    TUint32 t0 = User::FastCounter();
    TUint32 acc = 0;
    for (TInt p = 0; p < aPasses; ++p)
        for (TInt i = 0; i < n; i += 8)
            {
            acc += aBuf[i]   + aBuf[i+1] + aBuf[i+2] + aBuf[i+3];
            acc += aBuf[i+4] + aBuf[i+5] + aBuf[i+6] + aBuf[i+7];
            }
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    const TUint64 bytes = (TUint64)n * 4u * (TUint64)aPasses;
    r.iValue = us ? (TUint32)((bytes * 100u) / us) : 0;   // MB/s in hundredths
    r.iErrors = (acc == 0) ? 1 : 0;     // zero is impossible: pattern is non-zero
    return r;
    }

// SDRAM integrity: write a pattern, read back, count mismatched words.
// This test specifically catches the primary risk of bus overclocking - silent data corruption.
TTestResult TestMemIntegrity(TUint32* aBuf, TInt aWords)
    {
    TTestResult r;
    TUint32 t0 = User::FastCounter();
    TUint32 x = 0x2545f491u;
    TInt i;
    for (i = 0; i < aWords; ++i)
        {
        x = x * KLcgA + KLcgC;
        aBuf[i] = x;
        }
    x = 0x2545f491u;
    TInt bad = 0;
    for (i = 0; i < aWords; ++i)
        {
        x = x * KLcgA + KLcgC;
        if (aBuf[i] != x)
            ++bad;
        }
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    TUint32 bytes = (TUint32)aWords * 4u * 2u;          // write + read
    r.iValue = us ? (TUint32)(((TUint64)bytes * 100u) / us) : 0;
    r.iErrors = bad;
    return r;
    }

// Cache: L1 read bandwidth with a working set of 4 KB (L1 on ARM1136 is 16 KB).
//
// FIXED 2026-09-12. Previously an LCG generator ran here with writes and verification,
// and the result was reported as cache MB/s. In reality this measured arithmetic: for one
// word there was a multiplication (5 cycles of latency), addition, and verification, so
// the number reflected the ALU rather than the cache, and could not be compared with the memory test.
//
// Now it does exactly the same thing as the memory test, with the sole difference that the working
// set fits into L1. This makes the two numbers comparable, and their
// ratio shows how many times faster cache is than external memory.
// Correctness is not checked separately: TEST sdram integrity exists for that.
TTestResult TestCache(TUint32* aBuf, TInt aRounds)
    {
    TTestResult r;
    const TInt kWords = 1024;                            // 4 KB
    TUint32 acc = 0;
    TInt i;
    for (i = 0; i < kWords; ++i)                         // warm-up: fill cache lines
        acc += aBuf[i];
    TUint32 t0 = User::FastCounter();
    for (TInt p = 0; p < aRounds; ++p)
        for (i = 0; i < kWords; i += 8)
            {
            acc += aBuf[i]   + aBuf[i+1] + aBuf[i+2] + aBuf[i+3];
            acc += aBuf[i+4] + aBuf[i+5] + aBuf[i+6] + aBuf[i+7];
            }
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    const TUint64 bytes = (TUint64)kWords * 4u * (TUint64)aRounds;
    r.iValue = us ? (TUint32)((bytes * 100u) / us) : 0;
    r.iErrors = (acc == 0) ? 1 : 0;
    return r;
    }

void OcTestsInit(TUint32 aFastCounterHz)
    {
    if (aFastCounterHz)
        gFcFreq = aFastCounterHz;
    }
