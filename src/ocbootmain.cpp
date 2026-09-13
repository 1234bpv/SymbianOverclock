// ocboot.exe - applies the saved overclock after a restart, and nothing else.
//
// It has no user interface and never appears in the menu: the whole of the
// configuration lives in the application, which writes boot.dat. This is a
// separate executable rather than a mode of ocgui.exe for two reasons. The
// risky path is then a few hundred lines with no window server, no resources
// and no event loop, so there is very little in it that can go wrong; and
// deleting one file switches autostart off without removing the application.
//
// Three modes, chosen by the command line:
//   (none)   started from the startup list after a restart
//   soak     started by the application after a configuration was saved
//   dryrun   everything except touching the registers, for testing the logic
//
// See ocboot.h for why the record is written before the clocks are changed and
// why a configuration has to prove itself twice.
#include <e32base.h>
#include <e32std.h>
#include <hal.h>

#include "ocengine.h"
#include "ocboot.h"
#include "octests.h"

enum TBootMode
    {
    EModeBoot = 0,
    EModeSoak,
    EModeDry
    };

_LIT(KArgSoak, "soak");
_LIT(KArgDry, "dryrun");

static TBootMode ParseMode()
    {
    TBuf<32> cmd;
    if (User::CommandLineLength() <= (TInt)cmd.MaxLength())
        User::CommandLine(cmd);
    if (cmd.FindF(KArgSoak) >= 0)
        return EModeSoak;
    if (cmd.FindF(KArgDry) >= 0)
        return EModeDry;
    return EModeBoot;
    }

// Wall clock in seconds. Kept as two 32-bit halves in the record so that its
// layout does not depend on how the compiler aligns 64-bit members.
static void NowSeconds(TUint32& aLo, TUint32& aHi)
    {
    TTime t;
    t.UniversalTime();
    const TInt64 sec = t.Int64() / 1000000;
    aLo = (TUint32)I64LOW(sec);
    aHi = (TUint32)I64HIGH(sec);
    }

static TInt64 RecordSeconds(const TOcBootRecord& aRec)
    {
    return MAKE_TINT64(aRec.iApplyHi, aRec.iApplyLo);
    }

// Sleeps in short steps. User::After takes a single interval of up to about
// thirty-five minutes, but waiting in one call would also mean a process that
// cannot be killed cleanly halfway.
static void Wait(TUint32 aSeconds)
    {
    const TUint32 kStep = 5;
    for (TUint32 left = aSeconds; left > 0; )
        {
        const TUint32 chunk = (left > kStep) ? kStep : left;
        User::After(TTimeIntervalMicroSeconds32((TInt)chunk * 1000000));
        left -= chunk;
        }
    }

// One round of the same tests the application runs, on a buffer large enough to
// miss the cache. Returns the number of errors, or -1 if there is no memory:
// no buffer means no verdict, and no verdict must not read as "clean".
static TInt TestRound()
    {
    TInt words = 512 * 1024;
    TUint32* buf = (TUint32*)User::Alloc(words * sizeof(TUint32));
    if (!buf)
        {
        words = 64 * 1024;
        buf = (TUint32*)User::Alloc(words * sizeof(TUint32));
        }
    if (!buf)
        return -1;
    for (TInt i = 0; i < words; ++i)
        buf[i] = (TUint32)i + 1u;

    TInt errors = 0;
    TTestResult r = TestAlu(800000);
    errors += (r.iErrors > 0) ? r.iErrors : 0;
    r = TestCache(buf, 200);
    errors += (r.iErrors > 0) ? r.iErrors : 0;
    r = TestMemBandwidth(buf, words, 4);
    errors += (r.iErrors > 0) ? r.iErrors : 0;
    r = TestMemIntegrity(buf, words);
    errors += (r.iErrors > 0) ? r.iErrors : 0;

    User::Free(buf);
    return errors;
    }

static void Finish(TOcBootRecord& aRec, TOcBootResult aResult, const TDesC8& aWhy)
    {
    aRec.iLastResult = (TUint32)aResult;
    OcBootSave(aRec);
    OcBootLog(aWhy);
    }

static void Disable(TOcBootRecord& aRec, TUint32 aFlag, TOcBootResult aResult,
                    const TDesC8& aWhy)
    {
    aRec.iState = EOcBootOff;
    aRec.iFlags |= aFlag;
    Finish(aRec, aResult, aWhy);
    }

// Soak: the configuration was saved a moment ago and is live on the phone. If
// the phone is still here after the hold and one more round of tests comes back
// clean, the record is promoted from candidate to armed. A restart during the
// soak leaves it a candidate, and boot ignores candidates - which is the whole
// point of the state.
static void Soak(TOcBootRecord& aRec)
    {
    if (aRec.iState != EOcBootCandidate)
        return;
    Wait(aRec.iHoldSec);

    // Re-read: the user may have switched autostart off while we were waiting,
    // and the record on disk is then newer than ours.
    TOcBootRecord now;
    if (OcBootLoad(now) != KErrNone || now.iState != EOcBootCandidate)
        return;

    const TInt errors = TestRound();
    if (errors != 0)
        {
        Disable(now, KOcBootFlagSoakBad, EOcBootResSoakFailed, _L8("soak: tests failed"));
        return;
        }
    now.iState = EOcBootArmed;
    now.iFlags &= ~(KOcBootFlagFailed | KOcBootFlagSoakBad);
    NowSeconds(now.iApplyLo, now.iApplyHi);
    Finish(now, EOcBootResSoakOk, _L8("soak: passed, record armed"));
    }

static void BootApply(TOcBootRecord& aRec, TBool aDry)
    {
    // The record says the previous session applied a configuration and never
    // reached the end of its hold. That is the loop detector, and the answer to
    // it is to apply nothing at all and switch autostart off. The phone comes
    // up stock, and the application reports what happened.
    if (aRec.iState == EOcBootApplied)
        {
        Disable(aRec, KOcBootFlagFailed, EOcBootResRefusedFailed,
                _L8("boot: previous attempt did not finish, autostart off"));
        return;
        }
    if (aRec.iState != EOcBootArmed)
        return;

    // Second detector, independent of the first: the previous session confirmed
    // the configuration and then ended almost at once. A configuration that
    // survives the hold and dies a minute later would otherwise be applied on
    // every boot for ever, and the first detector would never see it.
    const TInt64 then = RecordSeconds(aRec);
    TUint32 lo = 0, hi = 0;
    NowSeconds(lo, hi);
    const TInt64 now = MAKE_TINT64(hi, lo);
    if (then > 0 && now > then && (now - then) < (TInt64)KOcBootMinSessionSec)
        {
        Disable(aRec, KOcBootFlagFailed, EOcBootResRefusedRecent,
                _L8("boot: previous session too short after confirming, autostart off"));
        return;
        }

    // The window in which the phone is up and the clocks are still stock. It
    // costs nothing on a normal boot and is the only way in if a configuration
    // brings the phone down every time.
    Wait(aRec.iDelaySec);

    aRec.iState = EOcBootApplied;
    aRec.iApplyLo = lo;
    aRec.iApplyHi = hi;
    if (OcBootSave(aRec) != KErrNone)
        return;                         // not on flash means not applied

    if (!aDry)
        {
        const TOcProfile* prof = ProfileForThisDevice();
        if (!prof)
            {
            aRec.iState = EOcBootArmed;
            Finish(aRec, EOcBootResNoProfile, _L8("boot: no profile for this phone"));
            return;
            }
        COcEngine* engine = NULL;
        TRAPD(err, engine = COcEngine::NewL());
        if (err != KErrNone || !engine)
            {
            aRec.iState = EOcBootArmed;
            Finish(aRec, EOcBootResNotReady, _L8("boot: engine could not be created"));
            return;
            }
        engine->SetProfile(*prof);
        engine->KeepOnExit(ETrue);
        engine->Prepare();
        if (!engine->IsReady())
            {
            // Nothing was written, so the record must not stay in the applied
            // state: the next boot would refuse a configuration that never ran.
            TBuf8<64> why;
            why.Format(_L8("boot: engine not ready, stopped at step %d"),
                       (TInt)engine->FailedAt());
            delete engine;
            aRec.iState = EOcBootArmed;
            Finish(aRec, EOcBootResNotReady, why);
            return;
            }

        TUint32 status = 0;
        const TInt r = engine->ApplyRampWithCore((TInt)aRec.iMult, aRec.iCore,
                                                 aRec.iMpu, status);
        const TBool applied = engine->IsOverclocked();
        // Read the multiplier back rather than trusting the return code: the
        // whole project rule is that what the hardware says is the answer.
        TInt liveAfter = 0;
        engine->LiveMult(liveAfter);
        // The DSP divider is deliberately not applied here. A non-stock DSP
        // divider rebooted the phone twice out of two at idle (findings 24.5),
        // which is why even the application holds it only under a watchdog. It
        // is kept in the record so that the saved configuration is complete,
        // not because boot is allowed to set it.
        delete engine;                  // keeps the clocks, removes the patch

        {
        TBuf8<96> line;
        line.Format(_L8("boot: apply m%d core %08x r=%d status=%u live=%d"),
                    (TInt)aRec.iMult, aRec.iCore, r, status, liveAfter);
        OcBootLog(line);
        }
        if (r != KErrNone && !applied)
            {
            aRec.iState = EOcBootArmed;
            Finish(aRec, EOcBootResApplyFailed, _L8("boot: apply failed, nothing held"));
            return;
            }
        }

    // Survive the hold and the configuration is confirmed for the next boot.
    Wait(aRec.iHoldSec);

    TOcBootRecord now2;
    if (OcBootLoad(now2) != KErrNone || now2.iState != EOcBootApplied)
        return;                         // switched off from the application
    now2.iState = EOcBootArmed;
    NowSeconds(now2.iApplyLo, now2.iApplyHi);
    Finish(now2, EOcBootResConfirmed, _L8("boot: held for the whole period, confirmed"));
    }

static void MainL()
    {
    TOcBootRecord rec;
    const TInt loaded = OcBootLoad(rec);
    if (loaded != KErrNone)
        {
        if (loaded == KErrCorrupt)
            OcBootLog(_L8("start: record failed its checks, renamed to boot.bad"));
        return;
        }

    TInt uid = 0;
    if (HAL::Get(HALData::EMachineUid, uid) != KErrNone ||
        rec.iMachineUid != (TUint32)uid)
        return;                         // written on another phone

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        OcTestsInit((TUint32)fc);

    const TBootMode mode = ParseMode();
    if (mode == EModeSoak)
        Soak(rec);
    else
        BootApply(rec, mode == EModeDry);
    }

GLDEF_C TInt E32Main()
    {
    CTrapCleanup* cleanup = CTrapCleanup::New();
    if (!cleanup)
        return KErrNoMemory;
    TRAPD(err, MainL());
    delete cleanup;
    return err;
    }
