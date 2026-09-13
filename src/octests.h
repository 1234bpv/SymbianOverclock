#ifndef OCTESTS_H
#define OCTESTS_H

#include <e32std.h>

// Result of one measurement. iValue is the figure in hundredths of its unit
// (Mitr/s or MB/s), iErrors is the number of mismatches the test found in its
// own known answer; -1 means the test was not run at all.
struct TTestResult
    {
    TUint32 iValue;
    TInt    iErrors;
    };

// Must be called once with HALData::EFastCounterFrequency before any test runs.
void OcTestsInit(TUint32 aFastCounterHz);

// Core clock in MHz, measured with a dependent chain whose cost in cycles is
// known exactly, and the cost of one LCG iteration in hundredths of a cycle.
TUint32 MeasureCoreMhz();
TUint32 MeasureLcgCycles100(TUint32 aMhz);

TTestResult TestAlu(TUint32 aIter);
TTestResult TestMemBandwidth(TUint32* aBuf, TInt aWords, TInt aPasses);
TTestResult TestMemIntegrity(TUint32* aBuf, TInt aWords);
TTestResult TestCache(TUint32* aBuf, TInt aRounds);

#endif
