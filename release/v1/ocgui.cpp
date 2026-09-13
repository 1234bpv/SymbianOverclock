// SymbianOverclock: overclocking and stress tests directly on the device.
//
// Controls (joystick):
//   up/down       - select a row
//   left/right    - on the MULT row moves the slider (step 1; faster when held)
//   OK/select     - activates the selected row
//   right soft key or Esc - exit (the stock frequency is RESTORED
//                   automatically before exiting)
//
// The guard against an accidental overclock is deliberate and has two levels:
// the slider only PREPARES a value, "APPLY SETTING" arms it, and only the
// separate "CONFIRM OVERCLOCK" button performs the relock. After a restart
// nothing is applied by itself - the application installs no startup entry.
//
// The log lines are deliberately ASCII: GCCE 3.4.3 and 8-bit descriptors are
// not worth the risk of non-ASCII literals.

#include <e32base.h>
#include <e32std.h>
#include <coecntrl.h>
#include <coemain.h>
#include <eikenv.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikstart.h>
#include <aknapp.h>
#include <aknappui.h>
#include <akndoc.h>
#include <akndef.h>
#include <gdi.h>
#include <w32std.h>
#include <hal.h>

#include "ocengine.h"

const TUid KOcGuiAppUid = { 0xE0001011 };

// --- instruments: each is both a stress test and a benchmark ----------------
//
// The main requirement for all of them: SELF-CHECKING. When overclocking, what
// matters is not speed but the moment a subsystem starts to fail silently, and
// that is only visible in a test with a known answer. Hence:
//   * the ALU runs an LCG forward, then exactly as many steps back through the
//     modular inverse of the multiplier - any single error breaks the return
//     to the initial value;
//   * memory and cache write a pseudo-random pattern and verify it on read-back.
// Neither needs a reference figure taken at a different frequency.

const TUint32 KLcgA = 1664525u;
const TUint32 KLcgC = 1013904223u;

struct TTestResult
    {
    TUint32 iValue;        // throughput, in hundredths of a unit
    TInt    iErrors;       // -1 = the test was not run
    };

// The modular inverse of KLcgA modulo 2^32 (Newton iteration; it always exists
// for an odd multiplier). Computed rather than hard-coded: a constant typed in
// by hand would turn a correctness test into a test of my own arithmetic.
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

// ALU: forward and back. Returns Mitr/s in hundredths and the error count (0 or 1).
static TTestResult TestAlu(TUint32 aIter)
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

// Memory bandwidth: walk a buffer larger than the cache with a cache-line stride.
// It is limited by L3/SDRAM, so it shows the bus rather than the core.
static TTestResult TestMemBandwidth(TUint32* aBuf, TInt aWords, TInt aPasses)
    {
    TTestResult r;
    TUint32 t0 = User::FastCounter();
    TUint32 acc = 0;
    for (TInt p = 0; p < aPasses; ++p)
        for (TInt i = 0; i < aWords; i += 16)
            acc += aBuf[i];
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    TUint32 lines = (TUint32)(aWords / 16) * (TUint32)aPasses;
    r.iValue = us ? (TUint32)(((TUint64)lines * 64u * 100u) / us) : 0;  // MB/s*100
    r.iErrors = (acc == 0) ? 1 : 0;     // zero is impossible here: the pattern is non-zero
    return r;
    }

// SDRAM integrity: write a pattern, read it back, count the differing words.
// This test catches the main risk of bus overclocking - silent data corruption.
static TTestResult TestMemIntegrity(TUint32* aBuf, TInt aWords)
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

// Cache: the same pattern, but a working set clearly smaller than L1 (16 KB on
// ARM1136). It separates core and cache errors from external bus errors.
static TTestResult TestCache(TUint32* aBuf, TInt aRounds)
    {
    TTestResult r;
    const TInt kWords = 1024;                            // 4 KB
    TUint32 t0 = User::FastCounter();
    TInt bad = 0;
    for (TInt p = 0; p < aRounds; ++p)
        {
        TUint32 x = 0x9e3779b9u + (TUint32)p;
        TInt i;
        TUint32 s = x;
        for (i = 0; i < kWords; ++i)
            {
            x = x * KLcgA + KLcgC;
            aBuf[i] = x;
            }
        x = s;
        for (i = 0; i < kWords; ++i)
            {
            x = x * KLcgA + KLcgC;
            if (aBuf[i] != x)
                ++bad;
            }
        }
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    TUint32 bytes = (TUint32)kWords * 4u * 2u * (TUint32)aRounds;
    r.iValue = us ? (TUint32)(((TUint64)bytes * 100u) / us) : 0;
    r.iErrors = bad;
    return r;
    }

// --- control ----------------------------------------------------------------

enum TRowId
    {
    ERowSlider = 0,
    ERowApply,
    ERowConfirm,
    ERowRestore,
    ERowTestAlu,
    ERowTestMem,
    ERowTestIntegrity,
    ERowTestCache,
    ERowTestCombined,
    ERowCount
    };

const TInt KLogLines = 6;

class COcView : public CCoeControl
    {
public:
    static COcView* NewL(const TRect& aRect);
    ~COcView();

    void Draw(const TRect& aRect) const;
    TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
    void ShutdownL();

private:
    COcView();
    void ConstructL(const TRect& aRect);
    void Activate(TRowId aRow);
    void RunTest(TRowId aRow);
    void RunCombined();
    void AddLog(const TDesC& aLine);
    void AddLogF(TRefByValue<const TDesC> aFmt, ...);
    void Repaint();
    TUint32* Buffer();
    void ReportResult(const TDesC& aName, const TTestResult& aRes,
                      const TDesC& aUnit);

private:
    COcEngine* iEngine;
    TInt   iRow;
    TInt   iMult;            // slider value
    TBool  iArmed;           // "APPLY SETTING" pressed, awaiting confirmation
    TInt   iArmedMult;
    TBuf<64> iLog[KLogLines];
    TInt   iLogCount;
    TUint32* iBuf;
    TInt   iBufWords;
    TBuf<80> iStatus;
    };

COcView::COcView()
  : iEngine(NULL), iRow(ERowSlider), iMult(0), iArmed(EFalse), iArmedMult(0),
    iLogCount(0), iBuf(NULL), iBufWords(0)
    {
    }

COcView* COcView::NewL(const TRect& aRect)
    {
    COcView* self = new (ELeave) COcView();
    CleanupStack::PushL(self);
    self->ConstructL(aRect);
    CleanupStack::Pop(self);
    return self;
    }

void COcView::ConstructL(const TRect& aRect)
    {
    CreateWindowL();
    SetRect(aRect);

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        gFcFreq = (TUint32)fc;

    iEngine = COcEngine::NewL();
    TInt r = iEngine->Prepare();
    if (r == KErrNone)
        {
        iMult = iEngine->StockMult();
        iArmedMult = iMult;
        AddLogF(_L("ready: stock mult %d"), iMult);
        AddLogF(_L("SRAM chunk @%08x"), iEngine->SramAddress());
        }
    else
        {
        iStatus.Format(_L("NOT READY (%d) at: %S"), r,
                       &COcEngine::StepName(iEngine->FailedAt()));
        AddLog(iStatus);
        AddLog(_L("patcher.ldd missing or driver refused"));
        iMult = 86;
        }
    ActivateL();
    }

COcView::~COcView()
    {
    User::Free(iBuf);
    delete iEngine;
    }

// Both changes outlive our process, so they must be undone explicitly and in
// order: the frequency first, then the patched body of the kernel function.
// Leaving the second in place plants a mine: any other dvfs_interface client
// calling DoControl(1) with a non-zero a1 dereferences an arbitrary address
void COcView::ShutdownL()
    {
    if (!iEngine)
        return;
    if (iEngine->IsOverclocked())
        {
        TUint32 st = 0;
        iEngine->RestoreStock(st);
        }
    iEngine->RestoreKernelCode();
    }

TUint32* COcView::Buffer()
    {
    if (!iBuf)
        {
        iBufWords = 512 * 1024;                 // 2 MB - clearly past the cache
        iBuf = (TUint32*)User::Alloc(iBufWords * sizeof(TUint32));
        if (!iBuf)
            {
            iBufWords = 64 * 1024;              // 256 KB - fallback
            iBuf = (TUint32*)User::Alloc(iBufWords * sizeof(TUint32));
            }
        if (iBuf)
            for (TInt i = 0; i < iBufWords; ++i)
                iBuf[i] = (TUint32)i + 1u;
        }
    return iBuf;
    }

void COcView::AddLog(const TDesC& aLine)
    {
    if (iLogCount == KLogLines)
        {
        for (TInt i = 0; i < KLogLines - 1; ++i)
            iLog[i] = iLog[i + 1];
        iLogCount = KLogLines - 1;
        }
    iLog[iLogCount].Copy(aLine.Left(iLog[iLogCount].MaxLength()));
    ++iLogCount;
    }

void COcView::AddLogF(TRefByValue<const TDesC> aFmt, ...)
    {
    VA_LIST list;
    VA_START(list, aFmt);
    TBuf<160> buf;
    buf.AppendFormatList(aFmt, list);
    VA_END(list);
    AddLog(buf);
    }

// The tests block the event loop, so we redraw explicitly and push the window
// queue - otherwise the screen freezes for the whole run.
void COcView::Repaint()
    {
    DrawNow();
    CCoeEnv::Static()->WsSession().Flush();
    }

void COcView::ReportResult(const TDesC& aName, const TTestResult& aRes,
                           const TDesC& aUnit)
    {
    AddLogF(_L("%S %u.%02u %S err=%d"), &aName, aRes.iValue / 100,
            aRes.iValue % 100, &aUnit, aRes.iErrors);
    }

_LIT(KUnitMitr, "Mitr/s");
_LIT(KUnitMbs, "MB/s");
_LIT(KNameAlu, "ALU");
_LIT(KNameMem, "MEM");
_LIT(KNameInt, "SDRAM");
_LIT(KNameCache, "CACHE");

void COcView::RunTest(TRowId aRow)
    {
    // The tests block the event loop for seconds. Without this line the screen
    // simply freezes, and it looks as if the button did nothing.
    iStatus.Copy(_L("running..."));
    Repaint();
    TUint32* buf = Buffer();
    if (!buf && aRow != ERowTestAlu)
        {
        AddLog(_L("no memory for buffer"));
        return;
        }
    switch (aRow)
        {
        case ERowTestAlu:
            {
            TTestResult r = TestAlu(1500000);
            ReportResult(KNameAlu, r, KUnitMitr);
            break;
            }
        case ERowTestMem:
            {
            TTestResult r = TestMemBandwidth(buf, iBufWords, 6);
            ReportResult(KNameMem, r, KUnitMbs);
            break;
            }
        case ERowTestIntegrity:
            {
            TTestResult r = TestMemIntegrity(buf, iBufWords);
            ReportResult(KNameInt, r, KUnitMbs);
            break;
            }
        case ERowTestCache:
            {
            TTestResult r = TestCache(buf, 400);
            ReportResult(KNameCache, r, KUnitMbs);
            break;
            }
        default:
            break;
        }
    iStatus.Zero();
    }

// Combined run: every subsystem in turn, several rounds, with a summary for
// each. This is what answers the question "where is the weak link": under
// overclocking, the subsystem that cannot cope is the first to report err>0.
void COcView::RunCombined()
    {
    TUint32* buf = Buffer();
    if (!buf)
        {
        AddLog(_L("no memory for buffer"));
        return;
        }
    const TInt kRounds = 3;
    TInt eAlu = 0, eMem = 0, eInt = 0, eCache = 0;
    TUint32 vAlu = 0, vMem = 0, vInt = 0, vCache = 0;

    for (TInt round = 0; round < kRounds; ++round)
        {
        iStatus.Format(_L("combined: round %d of %d"), round + 1, kRounds);
        Repaint();

        TTestResult a = TestAlu(800000);
        eAlu += a.iErrors;  if (a.iValue > vAlu) vAlu = a.iValue;

        TTestResult c = TestCache(buf, 200);
        eCache += c.iErrors; if (c.iValue > vCache) vCache = c.iValue;

        TTestResult m = TestMemBandwidth(buf, iBufWords, 4);
        eMem += m.iErrors;  if (m.iValue > vMem) vMem = m.iValue;

        TTestResult n = TestMemIntegrity(buf, iBufWords);
        eInt += n.iErrors;  if (n.iValue > vInt) vInt = n.iValue;
        }

    iStatus.Zero();
    AddLogF(_L("ALU %u.%02u Mitr/s e=%d"), vAlu / 100, vAlu % 100, eAlu);
    AddLogF(_L("CACHE %u.%02u MB/s e=%d"), vCache / 100, vCache % 100, eCache);
    AddLogF(_L("MEM %u.%02u MB/s e=%d"), vMem / 100, vMem % 100, eMem);
    AddLogF(_L("SDRAM %u.%02u MB/s e=%d"), vInt / 100, vInt % 100, eInt);

    if (eAlu == 0 && eCache == 0 && eMem == 0 && eInt == 0)
        AddLog(_L("VERDICT: all subsystems clean"));
    else if (eInt > 0 || eMem > 0)
        AddLog(_L("VERDICT: WEAK LINK = SDRAM/bus"));
    else if (eCache > 0)
        AddLog(_L("VERDICT: WEAK LINK = cache"));
    else
        AddLog(_L("VERDICT: WEAK LINK = CPU core"));
    }

void COcView::Activate(TRowId aRow)
    {
    if (!iEngine || !iEngine->IsReady())
        {
        if (aRow >= ERowTestAlu)
            {                                   // the tests work without the patcher too
            RunTest(aRow);
            return;
            }
        AddLog(_L("engine not ready - overclock disabled"));
        return;
        }

    switch (aRow)
        {
        case ERowApply:
            // Only arms it. It does NOT touch the frequency: this is the
            // "separate apply option" that still needs its own confirmation.
            iArmedMult = iMult;
            iArmed = ETrue;
            {
            TOcClocks c;
            iEngine->ClocksFor(iArmedMult, c);
            AddLogF(_L("armed: mult %d -> CPU %u MHz"), iArmedMult,
                    c.MpuKhz() / 1000);
            AddLog(_L("press CONFIRM to actually apply"));
            }
            break;

        case ERowConfirm:
            {
            if (!iArmed)
                {
                AddLog(_L("nothing armed - use APPLY first"));
                break;
                }
            TUint32 st = 0;
            iStatus.Format(_L("applying mult %d (stepwise)..."), iArmedMult);
            Repaint();
            // Let the window draw and fall quiet for a moment: the display
            // controller reads the frame buffer from SDRAM continuously, and
            // SDRAM is what we take away. Less traffic here, fewer artefacts.
            User::After(150000);
            TInt r = iEngine->ApplyMultRamp(iArmedMult, st);
            iStatus.Zero();
            if (r != KErrNone)
                {
                AddLogF(_L("apply failed: %d"), r);
                }
            else
                {
                TInt live = 0;
                iEngine->LiveMult(live);
                AddLogF(_L("applied: mult %d (live %d) st=%u"), iArmedMult,
                        live, st);
                // A relock timeout means the DPLL was left in an undefined
                // state. This must not wait until the next key press - we go
                // back to the stock multiplier ourselves.
                if (st != 0)
                    {
                    AddLog(_L("relock FAILED - blob rolled back"));
                    TUint32 st2 = 0;
                    iEngine->RestoreStock(st2);
                    iEngine->LiveMult(live);
                    AddLogF(_L("auto-restored, live mult %d"), live);
                    }
                }
            iArmed = EFalse;
            break;
            }

        case ERowRestore:
            {
            TUint32 st = 0;
            TInt r = iEngine->RestoreStock(st);
            TInt live = 0;
            iEngine->LiveMult(live);
            AddLogF(_L("restored: r=%d live mult %d"), r, live);
            iArmed = EFalse;
            break;
            }

        default:
            RunTest(aRow);
            break;
        }
    }

TKeyResponse COcView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
    {
    if (aType != EEventKey)
        return EKeyWasNotConsumed;

    switch (aKeyEvent.iCode)
        {
        case EKeyUpArrow:
            iRow = (iRow + ERowCount - 1) % ERowCount;
            break;
        case EKeyDownArrow:
            iRow = (iRow + 1) % ERowCount;
            break;
        case EKeyLeftArrow:
            if (iRow == ERowSlider && iMult > KOcMultMin)
                { --iMult; iArmed = EFalse; }
            break;
        case EKeyRightArrow:
            if (iRow == ERowSlider && iMult < KOcMultMax)
                { ++iMult; iArmed = EFalse; }
            break;
        // EKeyDevice3 (a joystick press on S60) equals EKeyOK here - no
        // separate branch is needed; the compiler flags it as a duplicate.
        case EKeyOK:
        case EKeyEnter:
            Activate((TRowId)iRow);
            break;
        case EKeyEscape:
        case EKeyDevice1:
            // CCoeAppUi has no Exit(); the exit goes through the shell command.
            CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
            return EKeyWasConsumed;
        default:
            // Digits 1..9 - a quick slider jump in multiples of the stock step.
            if (iRow == ERowSlider && aKeyEvent.iCode >= '0' && aKeyEvent.iCode <= '9')
                {
                TInt stock = iEngine ? iEngine->StockMult() : 86;
                TInt pct = (aKeyEvent.iCode - '0') * 5;         // 0,5,10..45%
                TInt m = stock + (stock * pct) / 100;
                iMult = (m > KOcMultMax) ? KOcMultMax : m;
                iArmed = EFalse;
                }
            else
                return EKeyWasNotConsumed;
            break;
        }
    DrawNow();
    return EKeyWasConsumed;
    }

void COcView::Draw(const TRect& /*aRect*/) const
    {
    CWindowGc& gc = SystemGc();
    TRect r = Rect();
    gc.SetPenStyle(CGraphicsContext::ESolidPen);
    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    gc.SetBrushColor(KRgbWhite);
    gc.Clear(r);

    // The SMALL font is deliberate. With NormalFont the full content (title,
    // slider, 8 button rows, status and log) came to about 360 px against the
    // ~268 available - the log lines were drawn past the bottom edge, and it
    // looked as if the tests did nothing. We now also clip whatever does not fit.
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    gc.UseFont(font);
    const TInt lh = font->HeightInPixels() + 1;
    const TInt x = r.iTl.iX + 2;
    const TInt yMax = r.iBr.iY - lh;
    TInt y = r.iTl.iY + lh;
    TBuf<96> line;

    gc.SetPenColor(KRgbBlack);
    TBool ready = (iEngine && iEngine->IsReady());
    TInt stock = ready ? iEngine->StockMult() : 86;

    if (ready)
        {
        const TOcClocks& s = iEngine->StockClocks();
        line.Format(_L("stock m%d  CPU %u  BUS %u MHz"), s.iMult,
                    s.MpuKhz() / 1000, s.L3Khz() / 1000);
        }
    else
        {
        line.Copy(_L("ENGINE NOT READY - tests only"));
        }
    gc.DrawText(line, TPoint(x, y));
    y += lh;

    // --- slider ---
    TBool sel = (iRow == ERowSlider);
    TOcClocks t;
    if (ready)
        iEngine->ClocksFor(iMult, t);
    TInt pct = stock ? ((iMult - stock) * 1000) / stock : 0;

    if (ready)
        line.Format(_L("%cMULT %d  %d.%d%%  -> CPU %u BUS %u"), sel ? '>' : ' ',
                    iMult, pct / 10, (pct < 0 ? -pct : pct) % 10,
                    t.MpuKhz() / 1000, t.L3Khz() / 1000);
    else
        line.Format(_L("%cMULT %d"), sel ? '>' : ' ', iMult);
    gc.DrawText(line, TPoint(x, y));
    y += lh;

    // The bar: minimum left, maximum right, grey mark = the stock multiplier.
    TInt barW = r.Width() - 6;
    TInt barY = y - font->AscentInPixels() + 2;
    TInt span = KOcMultMax - KOcMultMin;
    TInt knob = x + ((iMult - KOcMultMin) * (barW - 4)) / span;
    TInt mark = x + ((stock - KOcMultMin) * (barW - 4)) / span;
    gc.SetBrushStyle(CGraphicsContext::ENullBrush);
    gc.DrawRect(TRect(x, barY, x + barW, barY + 7));
    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    gc.SetBrushColor(KRgbGray);
    gc.DrawRect(TRect(mark, barY, mark + 2, barY + 7));
    gc.SetBrushColor(sel ? KRgbBlack : KRgbDarkGray);
    gc.DrawRect(TRect(knob, barY - 2, knob + 5, barY + 9));
    gc.SetBrushStyle(CGraphicsContext::ENullBrush);
    y += lh;

    // --- button rows ---
    const TInt kRows[] = { ERowApply, ERowConfirm, ERowRestore, ERowTestAlu,
                           ERowTestMem, ERowTestIntegrity, ERowTestCache,
                           ERowTestCombined };
    for (TInt i = 0; i < 8 && y < yMax; ++i)
        {
        TInt row = kRows[i];
        TBuf<40> label;
        switch (row)
            {
            case ERowApply:     label.Copy(_L("APPLY SETTING")); break;
            case ERowConfirm:
                label.Copy(iArmed ? _L("CONFIRM OVERCLOCK <<")
                                  : _L("CONFIRM (arm first)"));
                break;
            case ERowRestore:   label.Copy(_L("RESTORE STOCK")); break;
            case ERowTestAlu:   label.Copy(_L("TEST cpu/alu")); break;
            case ERowTestMem:   label.Copy(_L("TEST mem bandwidth")); break;
            case ERowTestIntegrity: label.Copy(_L("TEST sdram integrity")); break;
            case ERowTestCache: label.Copy(_L("TEST cache")); break;
            default:            label.Copy(_L("TEST combined (all)")); break;
            }
        line.Format(_L("%c%S"), (iRow == row) ? '>' : ' ', &label);
        gc.DrawText(line, TPoint(x, y));
        y += lh;
        }

    if (iStatus.Length() && y < yMax)
        {
        gc.DrawText(iStatus, TPoint(x, y));
        y += lh;
        }
    for (TInt i = 0; i < iLogCount && y < yMax; ++i)
        {
        gc.DrawText(iLog[i], TPoint(x, y));
        y += lh;
        }

    gc.DiscardFont();
    }

// --- application framework --------------------------------------------------

class COcAppUi : public CAknAppUi
    {
public:
    void ConstructL();
    ~COcAppUi();
    void HandleCommandL(TInt aCommand);
    void HandleResourceChangeL(TInt aType);
private:
    COcView* iView;
    };

void COcAppUi::ConstructL()
    {
    // ENoScreenFurniture removes both the status pane and the soft key pane, so
    // the application fills the whole screen in both orientations. With it, the
    // rect must come from ApplicationRect(), not ClientRect(): the latter returns
    // the area UNDER the furniture, which is now gone.
    BaseConstructL(CAknAppUi::ENoAppResourceFile | CAknAppUi::EAknEnableSkin |
                   CAknAppUi::ENoScreenFurniture);
    iView = COcView::NewL(ApplicationRect());
    AddToStackL(iView);
    }

// Screen rotation changes the geometry, and without this the application would
// keep a portrait rectangle in landscape mode - which is exactly why the
// results were readable in only one orientation.
void COcAppUi::HandleResourceChangeL(TInt aType)
    {
    CAknAppUi::HandleResourceChangeL(aType);
    if (aType == KEikDynamicLayoutVariantSwitch && iView)
        {
        iView->SetRect(ApplicationRect());
        iView->DrawNow();
        }
    }

COcAppUi::~COcAppUi()
    {
    if (iView)
        {
        // Order matters: undo the overclock first, then destroy the control.
        TRAP_IGNORE(iView->ShutdownL());
        RemoveFromStack(iView);
        delete iView;
        }
    }

void COcAppUi::HandleCommandL(TInt aCommand)
    {
    if (aCommand == EEikCmdExit || aCommand == EAknSoftkeyExit)
        {
        if (iView)
            iView->ShutdownL();
        Exit();
        }
    }

class COcDocument : public CAknDocument
    {
public:
    COcDocument(CEikApplication& aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi* CreateAppUiL() { return new (ELeave) COcAppUi; }
    };

class COcApplication : public CAknApplication
    {
private:
    CApaDocument* CreateDocumentL() { return new (ELeave) COcDocument(*this); }
    TUid AppDllUid() const { return KOcGuiAppUid; }
    };

LOCAL_C CApaApplication* NewApplication()
    {
    return new COcApplication;
    }

GLDEF_C TInt E32Main()
    {
    return EikStart::RunApplication(NewApplication);
    }
