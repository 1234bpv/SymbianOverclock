// SymbianOverclock: overclock and stress tests directly on the device.
//
// Controls (joystick):
//   up/down       - row selection
//   left/right    - on the MULT row moves the slider (step 1; holding accelerates),
//                   on MPUDIV and BUSDIV cycles through allowed dividers
//   OK on BUSDIV  - clears the manual value, returns divider to auto-selection
//   OK/select     - activates the selected row
//   right softkey or Esc - exit (before exit the stock frequency
//                   IS RESTORED automatically)
//
// Protection against accidental overclocking is intentional and two-tiered: the slider only
// PREPARES the value, "APPLY SETTING" arms it, and only a separate button
// "CONFIRM OVERCLOCK" performs relock. After reboot nothing is
// applied automatically - the application creates no startup entries.
//
// Log strings are intentionally ASCII: GCCE 3.4.3 and 8-bit descriptors are not worth the risk
// of Cyrillic in literals.

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
#include <sysutil.h>
#include <f32file.h>

#include "ocengine.h"
#include "ocstr.h"
#include "octests.h"
#include "ocboot.h"

// Build version. On the device it is otherwise impossible to tell which one is installed.
_LIT(KAppVersion, "SymbianOverclock 2.4.0");

const TUid KOcGuiAppUid = { 0xE0001011 };

// The boot-time applier. Started from here only to soak a freshly saved
// configuration; at boot it is started by the system from the startup list.
_LIT(KOcBootExe, "ocboot.exe");
_LIT(KSoakArg, "soak");


// Next/previous bus divider value.
//
// The set is intentionally NARROWER than what the firmware validator allows: we use
// KAutoL3Allowed (1, 2, 4, 6, 8, 12, 16). The reason is measured, not out of caution:
// `dssdiv=7` passes power.dll validation and yet does NOT make it into the register, nay
// more - an invalid value in one field rejects the ENTIRE write to
// CM_CLKSEL1_CORE (findings §26.0). Allowing such values into a row adjusted by
// finger makes no sense: the user would see "applied", but nothing would change
// in hardware. The wider set remains in MANUAL, where verification by reading is available.
static TInt L3DivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 1 && v <= 31; v += aDir)
        {
        if (KAutoL3Allowed & (1u << v))
            return v;
        }
    return aValue;
    }

// --- control ----------------------------------------------------------------

// List rows. Dividers are separate rows, so everything together no longer fits
// on screen - the list scrolls (see EnsureVisible/Draw).
//
// Memory bus divider (L3 field) has a SEPARATE row at the top, even though it lives
// in the same CM_CLKSEL1_CORE as the remaining fields in the list below. The reason is not
// convenience: this is the only divider used to measure where the limit actually lies - in the core or in the
// bus. It is measured (findings §26.13) that BestMem responds to bus change almost
// completely (l3=8 yields -45%), and to core frequency change - only by two fifths. To
// use this, the bus must be adjusted separately from the multiplier, without entering
// MANUAL and without touching the display at the same time.
enum TRowId
    {
    ERowSlider = 0,      // DPLL multiplier
    ERowMpu,             // MPU divider (separate register CM_CLKSEL_MPU)
    ERowL3,              // memory bus divider - L3 field in CM_CLKSEL1_CORE
    ERowDsp,             // DSP domain divider (separate register CM_CLKSEL_DSP)
    ERowMode,            // AUTO / MANUAL for CLKSEL1_CORE dividers
    ERowDivFirst,        // 7 divider rows of CM_CLKSEL1_CORE
    ERowDivLast = ERowDivFirst + ECoreFieldCount - 1,
    ERowApply,
    ERowConfirm,
    ERowRestore,
    ERowKeep,            // whether to keep overclock after exiting the application
    ERowAutostart,       // state of the boot-time applier
    ERowSaveBoot,        // save the live configuration for the boot-time applier
    ERowLang,            // interface language
    ERowTestAlu,
    ERowTestMem,
    ERowTestIntegrity,
    ERowTestCache,
    ERowTestCombined,
    ERowHelp,            // built-in instructions
    ERowTestProfile,     // manual selection of untested profile
    ERowCount
    };

const TInt KLogLines = 4;

class COcView : public CCoeControl
    {
public:
    static COcView* NewL(const TRect& aRect);
    ~COcView();

    void Draw(const TRect& aRect) const;
    void DrawHelp(CWindowGc& aGc, const TRect& aRect) const;
    TInt HelpTotalLines() const;
    TInt HelpVisibleLines() const;
    TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
    void ShutdownL();
    void SetLanguage(TInt aIndex);

private:
    COcView();
    void ConstructL(const TRect& aRect);
    void Activate(TRowId aRow);
    void ActivateTestProfile();
    void EnsureVisible(TInt aVisibleRows);
    TInt VisibleRows() const;
    TInt RowLines(TInt aRow) const;
    TInt LogLines() const;
    // CM_CLKSEL1_CORE value that will go to the device: either auto-selection or manual.
    TUint32 EffectiveCore() const;
    TUint32 BusKhz() const;
    void WarnBus(TInt aDiv);
    void RowLabel(TInt aRow, TDes& aOut) const;
    void SyncManualFromAuto();
    void RunTest(TRowId aRow);
    void RunCombined();
    // Saving a configuration for boot. Three conditions have to hold at once:
    // the overclock must be live in the registers, it must pass three rounds of
    // tests here and now, and it must then survive a soak before boot will use
    // it. See ocboot.h for why one proof is not enough.
    void SeedFromLive();
    void SaveForBoot();
    void ReportBootOutcome();
    // Watching the soak that follows a save. The soak runs in ocboot.exe, in a
    // process of its own, so the record changes behind our back and the row
    // would otherwise keep saying CHECKING until the application was next
    // opened - which is what it did, and what made a working save look broken.
    // The countdown is the same number the record holds, so what is on screen
    // is what ocboot is actually waiting for.
    static TInt SoakTick(TAny* aSelf);
    void StartSoakWatch(TInt aSeconds);
    // DSP divider auto-revert. Measured (findings §24.5): a non-stock divider,
    // left in the system for long, crashes the device into a reboot even without any
    // load on the DSP, whereas a dwell with guaranteed reversion succeeds
    // cleanly. Therefore the divider does not "stay until removed" here, but lives for exactly
    // the allocated window and reverts on its own, even if the application is simply left
    // open.
    static TInt DspRevertTick(TAny* aSelf);
    void CancelDspRevert();
    void RestoreDspNow(const TDesC& aWhy);
    void AddLog(const TDesC& aLine);
    void AddLogF(TRefByValue<const TDesC> aFmt, ...);
    void Repaint();
    TUint32* Buffer();
    void ReportResult(const TDesC& aName, const TTestResult& aRes,
                      const TDesC& aUnit);

private:
    COcEngine* iEngine;
    TInt   iRow;
    TInt   iTop;             // first visible row (list scrolls)
    TInt   iMult;            // slider value
    TBool  iManual;          // dividers set by user, not auto-selection
    TUint32 iCoreReg;        // edited CM_CLKSEL1_CORE in manual mode
    TInt   iMpuDiv;          // edited MPU divider (1,2,4,6,8)
    TInt   iL3Div;           // bus divider; 0 = leave on auto-selection
    TInt   iLiveL3;          // bus divider CURRENTLY in hardware (0 = not read)
    TBool  iKeep;            // overclock remains after exit
    TInt   iDspDiv;          // edited DSP divider (path separate from iArmed)
    CPeriodic* iDspTimer;    // watchdog for DSP divider auto-revert
    TInt   iDspLeftSec;      // seconds remaining until auto-revert
    CPeriodic* iSoakTimer;   // watches the soak running in ocboot.exe
    TInt   iSoakLeftSec;     // seconds left of the soak, 0 = past it
    TInt   iSoakPollSec;     // seconds left of waiting for the verdict
    TBool  iArmed;           // "APPLY SETTING" pressed, awaiting confirmation
    TInt   iArmedMult;
    TUint32 iArmedCore;
    TUint32 iArmedMpu;
    TBuf<64> iLog[KLogLines];
    TInt   iLogCount;
    TUint32* iBuf;
    TInt   iBufWords;
    TBuf<80> iStatus;
    TBool  iKnownDevice;     // profile matched automatically
    TInt   iTestProfile;     // selected test profile; -1 = none
    TOcBootRecord iBoot;     // autostart record, as read at startup
    TBool  iHelp;            // ETrue = showing help page
    TInt   iHelpTop;         // line of help to draw from
    };

COcView::COcView()
  : iEngine(NULL), iRow(ERowSlider), iTop(0), iMult(0), iManual(EFalse),
    iCoreReg(0), iMpuDiv(2), iL3Div(0), iLiveL3(0), iKeep(ETrue), iDspDiv(3), iDspTimer(NULL), iDspLeftSec(0),
    iSoakTimer(NULL), iSoakLeftSec(0), iSoakPollSec(0),
    iArmed(EFalse), iArmedMult(0), iArmedCore(0),
    iArmedMpu(0),
    iLogCount(0), iBuf(NULL), iBufWords(0), iKnownDevice(EFalse), iTestProfile(0), iHelp(EFalse), iHelpTop(0)
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

// Defined below, alongside the rest of the settings file routines.
static void LoadLanguage();

void COcView::ConstructL(const TRect& aRect)
    {
    // Language is loaded once and before the first visible line: otherwise part
    // of the log would remain in English, and part would not.
    OcStrInit();
    LoadLanguage();
    ReportBootOutcome();
    CreateWindowL();
    SetRect(aRect);

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        OcTestsInit((TUint32)fc);

    iEngine = COcEngine::NewL();
    // Overclock remains after exit: user applied it consciously, and
    // clearing it on their behalf simply because they closed the window is unjustified.
    // Kernel patch is always removed in doing so - unlike frequency,
    // it is a landmine for later. Reboot restores stock clocking in any case.
    iEngine->KeepOnExit(iKeep);

    // Device profile is matched automatically: there is no command line here, and a foreign
    // profile would mean patching foreign addresses into the kernel. Unknown device is an honest
    // refusal BEFORE any touch to the kernel, not a guess.
    const TOcProfile* prof = ProfileForThisDevice();
    if (!prof)
        {
        TInt uid = 0;
        HAL::Get(HALData::EMachineUid, uid);
        iStatus.Format(OcStr(EStrUnknownDevice), (TUint32)uid);
        AddLog(iStatus);
        AddLogF(OcStr(EStrNoProfileUid),
                (TUint32)uid);
        // Unknown device is not a dead end, but the beginning of porting, and exactly
        // these two lines are needed to build a profile for it. Therefore they
        // are visible right on screen, not hidden in a developer build.
        {
        TBuf<KSysUtilVersionTextLength> sw;
        if (SysUtil::GetSWVersion(sw) == KErrNone)
            {
            TInt nl = sw.Locate(TChar(10));
            TPtrC first(sw.Left(nl == KErrNotFound ? sw.Length() : nl));
            AddLogF(_L("firmware: %S"), &first);
            }
        AddLog(OcStr(EStrUnknownSend));
        AddLog(OcStr(EStrUnknownSeeDocs));
        }
        iMult = 86;
        ActivateL();       // window must remain alive so the refusal is visible
        return;
        }
    iKnownDevice = ETrue;
    iEngine->SetProfile(*prof);
    {
    TBuf<32> pid;
    pid.Copy(TPtrC8((const TUint8*)prof->iId));
    // Two lines: name with version and profile ID together do not
    // fit within the N95 screen width (device remark, 2026-09-12).
    AddLog(KAppVersion);
    AddLogF(_L("profile: %S"), &pid);
    }

    TInt r = iEngine->Prepare();
    if (r == KErrNone)
        {
        SeedFromLive();
        AddLogF(_L("ready: stock m%d core %08x"), iEngine->StockMult(),
                iEngine->StockCore());   // the reference, not the editor
        if (iEngine->BootedNonStock())
            AddLogF(_L("NON-STOCK at start: m%d core %08x mpu %08x"),
                    iEngine->BootMult(), iEngine->BootCore(), iEngine->BootMpu());
        AddLogF(_L("SRAM chunk @%08x"), iEngine->SramAddress());
        }
    else
        {
        iStatus.Format(_L("NOT READY (%d) at: %S"), r,
                       &COcEngine::StepName(iEngine->FailedAt()));
        AddLog(iStatus);
        // Firmware mismatch is not a breakdown, but an intentional refusal, and confusing
        // it with "driver failed to load" is unacceptable: it is cured by something completely different.
        if (iEngine->FailedAt() == EOcStepFirmware)
            {
            TPtrC fw(iEngine->Firmware());
            if (fw.Length())
                AddLogF(_L("device firmware: %S"), &fw);
            AddLog(OcStr(EStrNoProfileBuild));
            }
        else
            AddLog(OcStr(EStrPatcherMissing));
        iMult = 86;
        }
    ActivateL();
    }

COcView::~COcView()
    {
    delete iDspTimer;
    delete iSoakTimer;
    User::Free(iBuf);
    delete iEngine;
    }

// Ceiling for how long a non-stock DSP divider is allowed to run at all.
//
// This is NOT "proven safe time": there is no safe time here. It is measured
// (findings §24.6) that domain at CORE/2 remains alive only as long as nothing is executing
// on C55x, and crashes the device as soon as DSP is brought up - whether by our child
// process, or by stock audio ten minutes later. Therefore watchdog does not "wait out
// danger", but merely limits the window into which such an event can strike.
const TInt KDspHoldSec = 120;

// Stops the watchdog without destroying it.
//
// The timer must NOT be deleted here. This runs from DspRevertTick, which is
// the timer's own callback: deleting the CPeriodic from inside its callback
// leaves CActive::RunL working on freed memory once the callback returns.
// The object is cheap to keep and is destroyed once, in the destructor; a
// cancelled CPeriodic can simply be started again.
void COcView::CancelDspRevert()
    {
    if (iDspTimer)
        iDspTimer->Cancel();
    iDspLeftSec = 0;
    }

void COcView::RestoreDspNow(const TDesC& aWhy)
    {
    CancelDspRevert();
    if (!iEngine || !iEngine->IsDspChanged())
        return;
    TInt rc = 0;
    iEngine->RestoreDspDiv(rc);
    iDspDiv = iEngine->DspDiv();
    AddLogF(OcStr(EStrDspReverted), iDspDiv, &aWhy);
    Repaint();
    }

TInt COcView::DspRevertTick(TAny* aSelf)
    {
    COcView* self = (COcView*)aSelf;
    if (--self->iDspLeftSec > 0)
        {
        self->Repaint();            // shows how much is remaining
        return 0;
        }
    self->RestoreDspNow(OcStr(EStrReasonTimeout));
    return 0;
    }

// Two changes outlive our process, and are treated differently.
//
// Replaced kernel function body is removed ALWAYS. Leaving it means
// setting a landmine: any other dvfs_interface client calling
// DoControl(1) with non-zero a1 will dereference an arbitrary address in kernel mode.
//
// Frequency, bus divider, and DSP divider - on the contrary, remain if the user
// so desires (KEEP ON EXIT row, enabled by default). Closing the window is no
// reason to cancel what the user applied consciously; stock clocking
// is restored by reboot. At the same time this removes the only genuinely dangerous operation from
// the exit path: reverting a lowered bus back to stock killed the device four times
// out of four, whereas exit now simply does not perform it.
// Name of the file containing selected language. Application private directory on C:,
// i.e. a regular data file - it has nothing to do with firmware or overclocking.
_LIT(KLangFile, "C:\\private\\e0001011\\lang.txt");

// Reads saved language. Error at any step means "nothing saved",
// in which case language chosen by OcStrInit based on device language remains.
static void LoadLanguage()
    {
    RFs fs;
    if (fs.Connect() != KErrNone)
        return;
    RFile f;
    if (f.Open(fs, KLangFile, EFileRead) == KErrNone)
        {
        TBuf8<16> buf;
        if (f.Read(buf) == KErrNone && buf.Length() > 0)
            {
            TLex8 lex(buf);
            TInt v = -1;
            if (lex.Val(v) == KErrNone && v >= 0 && v < OcStrLanguageCount())
                OcStrSetLanguage(v);
            }
        f.Close();
        }
    fs.Close();
    }

// Saves selected language. Silently ignores errors intentionally: unsaved selection
// is an inconvenience, not a reason to halt application operation.
static void SaveLanguage(TInt aIndex)
    {
    RFs fs;
    if (fs.Connect() != KErrNone)
        return;
    fs.MkDirAll(KLangFile);
    RFile f;
    if (f.Replace(fs, KLangFile, EFileWrite) == KErrNone)
        {
        TBuf8<16> buf;
        buf.Num(aIndex);
        f.Write(buf);
        f.Close();
        }
    fs.Close();
    }

// Switches interface language. Index wraps around circularly within OcStr, so
// any number can be passed here.
void COcView::SetLanguage(TInt aIndex)
    {
    OcStrSetLanguage(aIndex);
    SaveLanguage(OcStrLanguageIndex());
    // Log remains as recorded: strings in it are already finished
    // text, not identifiers, and are not translated retroactively.
    AddLog(OcStrActiveLanguageName());
    }

void COcView::ShutdownL()
    {
    if (!iEngine)
        return;
    if (!iKeep && iEngine->IsOverclocked())
        {
        TUint32 st = 0;
        iEngine->RestoreStock(st);
        }
    CancelDspRevert();
    if (!iKeep && iEngine->IsDspChanged())
        {
        TInt rc = 0;
        iEngine->RestoreDspDiv(rc);
        }
    iEngine->RestoreKernelCode();
    }

TUint32* COcView::Buffer()
    {
    if (!iBuf)
        {
        iBufWords = 512 * 1024;                 // 2 MB - well beyond cache
        iBuf = (TUint32*)User::Alloc(iBufWords * sizeof(TUint32));
        if (!iBuf)
            {
            iBufWords = 64 * 1024;              // 256 KB - fallback option
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

// Tests block the event loop, so repaint explicitly and flush the window
// server queue - otherwise screen freezes for the entire run.
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

// Sets the editable configuration to what the registers hold right now.
//
// The multiplier was always taken from the live register; the dividers were
// taken from the profile, and on a phone that autostart had already
// overclocked the two disagreed. The multiplier row said 105 while the bus and
// L4 rows said stock, because the dividers shown were either the profile
// constant or what auto-compensation would propose - never the register. That
// looked exactly like an autostart that had applied the multiplier and dropped
// the dividers, and it cost an evening to find out that the hardware had them
// all along (findings 51).
//
// A phone that booted non-stock also switches to manual: the layout in the
// registers was chosen by someone, and auto-compensation would quietly offer a
// different one under rows that are supposed to describe the phone as it is.
void COcView::SeedFromLive()
    {
    iMult = iEngine->BootMult();
    iArmedMult = iMult;
    iCoreReg = iEngine->BootCore();
    iArmedCore = iCoreReg;
    iMpuDiv = (TInt)(iEngine->BootMpu() & KMpuDivMask);
    iArmedMpu = iEngine->BootMpu();
    iDspDiv = iEngine->StockDspDiv();
    iLiveL3 = (TInt)(iEngine->BootCore() & KCoreL3Mask);
    if (iEngine->BootedNonStock())
        {
        iManual = ETrue;
        iL3Div = iLiveL3;
        }
    }

// In manual mode edited register is used, in auto - one chosen by engine.
// Beyond that: as soon as the user touches the BUSDIV row, its value overrides
// the L3 field in both modes. For this reason the row is placed at the top - so the bus can
// be specified without entering MANUAL and without touching the display divider, which in auto
// remains compensated.
TUint32 COcView::EffectiveCore() const
    {
    if (!iEngine || !iEngine->IsReady())
        return iCoreReg;
    TUint32 core = iManual ? iCoreReg : iEngine->CompensatedCore(iMult);
    if (iL3Div > 0)
        core = CoreFieldSet(core, ECoreL3, iL3Div);
    return core;
    }

// Bus frequency with selected multiplier and CURRENT L3 divider, in kHz.
TUint32 COcView::BusKhz() const
    {
    if (!iEngine || !iEngine->IsReady())
        return 0;
    TOcClocks c;
    iEngine->ClocksFor(iMult, c);
    const TInt v = CoreFieldValue(EffectiveCore(), ECoreL3);
    return v ? c.CoreKhz() / (TUint32)v : 0;
    }

// Warning about direction of bus change. Both are measured, not hypothetical.
void COcView::WarnBus(TInt aDiv)
    {
    if (!iEngine || !iEngine->IsReady())
        return;
    const TInt stock = iEngine->StockClocks().iL3Div;
    if (aDiv > stock)
        {
        // Most costly known fact of this row: bus itself at /8 holds
        // cleanly (two runs, zero corrupted words in SDRAM), but REVERTING
        // from it crashed the device three times out of three - both via full relock and lightweight
        // divider rewrite (findings §26.12-26.14).
        AddLog(OcStr(EStrWarnBusBelow));
        AddLog(OcStr(EStrWarnBusReturn));
        }
    else if (aDiv < stock)
        {
        AddLog(OcStr(EStrWarnBusAbove));
        }
    }

// When switching to manual mode, start not from scratch, but from what
// automatic selection picked for the current multiplier: giving user an immediate working baseline.
void COcView::SyncManualFromAuto()
    {
    if (iEngine && iEngine->IsReady())
        iCoreReg = iEngine->CompensatedCore(iMult);
    }

static TInt WrapText(CWindowGc* aGc, const CFont* aFont, const TDesC& aText,
                     TInt aX, TInt& aY, TInt aLh, TInt aYMax, TInt aWidth,
                     TInt& aSkip);

// How many screen lines list label occupies after wrapping.
TInt COcView::RowLines(TInt aRow) const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt w = Rect().Width() - 4;
    TBuf<96> label;
    RowLabel(aRow, label);
    TBuf<98> line;
    line.Format(_L(" %S"), &label);
    TInt y = 0;
    TInt skip = 0;
    return WrapText(NULL, font, line, 0, y, 1, 0, w, skip);
    }

// How many screen lines log will occupy after wrapping. Log line has up to
// 64 characters, meaning on a narrow screen it routinely becomes two, and because of this
// test results ended up beyond the bottom edge: space for the log was
// allocated by entry count rather than line count.
TInt COcView::LogLines() const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt lh = font->HeightInPixels() + 1;
    const TInt w = Rect().Width() - 4;
    TInt y = 0;
    TInt skip = 0;
    TInt n = 0;
    for (TInt i = 0; i < iLogCount; ++i)
        n += WrapText(NULL, font, iLog[i], 0, y, 1, 0, w, skip);
    // Allocate at most half the screen to the log: the rest is needed by the list.
    const TInt cap = (Rect().Height() / lh) / 2;
    return n > cap ? cap : n;
    }

// How many LIST ROWS fit, counting from iTop. After labels
// started wrapping, row height is no longer constant, so dividing screen height
// by row height would yield too large a number, and cursor would go beyond the edge.
TInt COcView::VisibleRows() const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt lh = font->HeightInPixels() + 1;
    // 3 header lines + status line + log
    TInt avail = Rect().Height() / lh - 4 - LogLines();
    if (avail < 3)
        avail = 3;
    TInt used = 0;
    TInt rows = 0;
    for (TInt r = iTop; r < ERowCount; ++r)
        {
        const TInt n = RowLines(r);
        if (rows > 0 && used + n > avail)
            break;
        used += n;
        ++rows;
        }
    return rows > 0 ? rows : 1;
    }

// Argument remains for call compatibility: how many rows are visible now
// depends on iTop itself, so query it anew at each step.
void COcView::EnsureVisible(TInt /*aVisibleRows*/)
    {
    if (iRow < iTop)
        iTop = iRow;
    while (iTop < iRow && iRow >= iTop + VisibleRows())
        ++iTop;
    if (iTop < 0)
        iTop = 0;
    }

// One list row in finished form. For dividers we show not only
// the value, but also resulting frequency and deviation from stock - it is this
// deviation that guides tuning.
void COcView::RowLabel(TInt aRow, TDes& aOut) const
    {
    aOut.Zero();
    const TBool ready = (iEngine && iEngine->IsReady());
    const TInt stock = ready ? iEngine->StockMult() : 86;

    if (aRow == ERowSlider)
        {
        TInt pct = stock ? ((iMult - stock) * 1000) / stock : 0;
        TOcClocks c;
        if (ready)
            {
            iEngine->ClocksFor(iMult, c);
            c.iMpuDiv = iMpuDiv;
            }
        aOut.Format(_L("MULT %d  %d.%d%%  CPU %u"), iMult, pct / 10,
                    (pct < 0 ? -pct : pct) % 10, ready ? c.MpuKhz() / 1000 : 0);
        return;
        }
    if (aRow == ERowMpu)
        {
        // CPU = CORE / mpu_div. Divider 1 gives the same core frequency at twice
        // lower CORE - this is the test for whether the limit is in ARM or in CORE.
        TOcClocks c;
        if (ready)
            {
            iEngine->ClocksFor(iMult, c);
            c.iMpuDiv = iMpuDiv;
            aOut.Format(_L(" MPUDIV /%d  CPU %u  CORE %u"), iMpuDiv,
                        c.MpuKhz() / 1000, c.CoreKhz() / 1000);
            }
        else
            aOut.Format(_L(" MPUDIV /%d"), iMpuDiv);
        return;
        }
    if (aRow == ERowL3)
        {
        // We show bus frequency and deviation from stock rather than the divider itself:
        // tuning is guided by it. The auto mark means value is taken by
        // auto-selection and will move along with the multiplier.
        const TInt v = ready ? CoreFieldValue(EffectiveCore(), ECoreL3)
                             : (iL3Div ? iL3Div : 4);
        if (!ready)
            {
            aOut.Format(_L(" BUSDIV /%d"), v);
            return;
            }
        // Bus clamping is shown BEFORE application, not only in log after
        // it. Divider smaller than live one means bus increase, and engine
        // will not permit it (findings §38.2). Row must show the
        // value that will actually go into hardware, otherwise UI lies about
        // its own operation, looking exactly like "divider does not work".
        const TUint32 got = BusKhz();
        const TUint32 want = iEngine->StockClocks().L3Khz();
        const TInt d = want ? (TInt)(((TInt64)got - want) * 1000 / want) : 0;
        if (iL3Div)
            aOut.Format(OcStr(EStrRowBusManual), v, got / 1000,
                        d / 10, (d < 0 ? -d : d) % 10);
        else
            aOut.Format(OcStr(EStrRowBusAuto), v, got / 1000,
                        d / 10, (d < 0 ? -d : d) % 10);
        return;
        }
    if (aRow == ERowTestProfile)
        {
        // Test profile is only needed where device is unrecognized. On
        // a known device offering untested addresses makes no sense and
        // is harmful: a proven profile is always better.
        if (iKnownDevice)
            {
            aOut.Copy(OcStr(EStrTestProfileKnown));
            return;
            }
        const TOcProfile* tp = TestProfile(iTestProfile);
        if (!tp)
            {
            aOut.Copy(OcStr(EStrTestProfileNone));
            return;
            }
        TBuf<48> nm;
        nm.Copy(TPtrC8((const TUint8*)tp->iId));
        aOut.Format(OcStr(EStrTestProfileRow), &nm);
        return;
        }
    if (aRow == ERowAutostart)
        {
        if (iBoot.iState == EOcBootCandidate && iSoakLeftSec > 0)
            aOut.Format(OcStr(EStrRowAutostartSoakLeft),
                        iSoakLeftSec / 60, iSoakLeftSec % 60);
        else if (iBoot.iState == EOcBootCandidate)
            aOut.Copy(OcStr(EStrRowAutostartSoak));
        else if (iBoot.iState == EOcBootArmed || iBoot.iState == EOcBootApplied)
            {
            aOut.Format(OcStr(EStrRowAutostartOn), (TInt)iBoot.iMult);
            // The dividers belong here as much as the multiplier does. Without
            // them the row says what the record holds only in part, and a record
            // saved before the dividers were set is indistinguishable from one
            // saved after - which is exactly the question the E90 raised. The
            // field names are register fields and stay as they are in any
            // language, like the other technical suffixes in this list.
            TBuf<24> div;
            div.Format(_L("  l3/%d l4/%d"),
                       CoreFieldValue(iBoot.iCore, ECoreL3),
                       CoreFieldValue(iBoot.iCore, ECoreL4));
            if (aOut.MaxLength() - aOut.Length() >= div.Length())
                aOut.Append(div);
            }
        else
            aOut.Copy(OcStr(EStrRowAutostartOff));
        return;
        }
    if (aRow == ERowSaveBoot)
        {
        aOut.Copy(OcStr(EStrRowSaveBoot));
        return;
        }
    if (aRow == ERowKeep)
        {
        aOut.Copy(OcStr(iKeep ? EStrKeepRowOn : EStrKeepRowOff));
        return;
        }
    if (aRow == ERowLang)
        {
        TPtrC nm(OcStrActiveLanguageName());
        aOut.Format(OcStr(EStrRowLanguage), &nm);
        return;
        }
    if (aRow == ERowDsp)
        {
        // DSP is clocked from CORE, so its frequency depends on both the multiplier
        // and this divider. We show the result for the SELECTED multiplier,
        // as in remaining rows, plus what is in hardware right now.
        if (!ready)
            {
            aOut.Format(_L(" DSPDIV /%d"), iDspDiv);
            return;
            }
        TOcClocks c;
        iEngine->ClocksFor(iMult, c);
        TUint32 got = iDspDiv ? c.CoreKhz() / (TUint32)iDspDiv : 0;
        if (iDspLeftSec > 0)
            aOut.Format(OcStr(EStrRowDspRevert), iDspDiv,
                        got / 1000, iDspLeftSec);
        else
            aOut.Format(OcStr(EStrRowDspLive), iDspDiv,
                        got / 1000, iEngine->DspDiv());
        return;
        }
    if (aRow == ERowMode)
        {
        {
        TPtrC md(OcStr(iManual ? EStrModeManual : EStrModeAuto));
        aOut.Format(_L("DIVIDERS: %S"), &md);
        }
        return;
        }
    if (aRow >= ERowDivFirst && aRow <= ERowDivLast)
        {
        const TInt id = aRow - ERowDivFirst;
        const TCoreField& f = CoreField(id);
        const TUint32 core = EffectiveCore();
        const TInt v = CoreFieldValue(core, id);
        TPtrC8 n8((const TUint8*)f.iName);
        TBuf<8> nm; nm.Copy(n8);

        if (f.iBase < 0 || !ready)
            {                                  // not a divider: raw value only
            aOut.Format(_L(" %S = %d"), &nm, v);
            return;
            }
        // Frequency of this domain at SELECTED multiplier and this divider.
        TOcClocks c;
        iEngine->ClocksFor(iMult, c);
        TUint32 base = c.CoreKhz();
        if (f.iBase == 1)                      // L4 divides from L3, not from CORE
            {
            TInt l3 = CoreFieldValue(core, ECoreL3);
            base = l3 ? c.CoreKhz() / (TUint32)l3 : 0;
            }
        TUint32 got = v ? base / (TUint32)v : 0;

        // Stock frequency of this same domain - reference point for tuning.
        const TOcClocks& st = iEngine->StockClocks();
        TUint32 sbase = st.CoreKhz();
        TInt sv = CoreFieldValue(iEngine->StockCore(), id);
        if (f.iBase == 1)
            {
            TInt sl3 = CoreFieldValue(iEngine->StockCore(), ECoreL3);
            sbase = sl3 ? st.CoreKhz() / (TUint32)sl3 : 0;
            }
        TUint32 want = sv ? sbase / (TUint32)sv : 0;
        TInt d = want ? (TInt)(((TInt64)got - want) * 1000 / want) : 0;
        aOut.Format(_L(" %S /%d = %u  %d.%d%%"), &nm, v, got / 1000,
                    d / 10, (d < 0 ? -d : d) % 10);
        return;
        }
    switch (aRow)
        {
        case ERowApply:   aOut.Copy(OcStr(EStrRowApply)); break;
        case ERowConfirm:
            aOut.Copy(OcStr(iArmed ? EStrRowConfirmArmed
                                   : EStrRowConfirmIdle));
            break;
        case ERowRestore: aOut.Copy(OcStr(EStrRowRestore)); break;
        case ERowTestAlu: aOut.Copy(OcStr(EStrRowTestAlu)); break;
        case ERowTestMem: aOut.Copy(OcStr(EStrRowTestMem)); break;
        case ERowTestIntegrity: aOut.Copy(OcStr(EStrRowTestInt)); break;
        case ERowTestCache: aOut.Copy(OcStr(EStrRowTestCache)); break;
        case ERowHelp: aOut.Copy(OcStr(EStrRowHelp)); break;
        default: aOut.Copy(OcStr(EStrRowTestAll)); break;
        }
    }

void COcView::RunTest(TRowId aRow)
    {
    // Tests block the event loop for seconds. Without this line the screen simply
    // freezes, making it look as though the button failed to act.
    iStatus.Copy(OcStr(EStrRunning));
    Repaint();
    TUint32* buf = Buffer();
    if (!buf && aRow != ERowTestAlu)
        {
        AddLog(OcStr(EStrNoMemory));
        return;
        }
    switch (aRow)
        {
        case ERowTestAlu:
            {
            // Three different numbers, because they answer different questions:
            // frequency - "how many cycles per second", cycles per operation -
            // "how much work costs", Mitr/s - "did the result match".
            const TUint32 mhz = MeasureCoreMhz();
            AddLogF(_L("CLOCK %u MHz"), mhz);
            if (mhz)
                {
                const TUint32 c = MeasureLcgCycles100(mhz);
                AddLogF(_L("MUL+ADD %u.%02u cycles/op"), c / 100, c % 100);
                }
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
        case ERowTestCombined:
            // Without this branch the row did nothing at all: the switch fell
            // through to default and RunCombined was never reached.
            RunCombined();
            break;

        default:
            break;
        }
    iStatus.Zero();
    }

// Combined run: all subsystems sequentially, several rounds, with summary for
// each. It specifically answers the question "where is the weak link": under overclocking
// the subsystem that cannot keep up will be the first to yield err>0.
void COcView::RunCombined()
    {
    TUint32* buf = Buffer();
    if (!buf)
        {
        AddLog(OcStr(EStrNoMemory));
        return;
        }
    const TInt kRounds = 3;
    TInt eAlu = 0, eMem = 0, eInt = 0, eCache = 0;
    TUint32 vAlu = 0, vMem = 0, vInt = 0, vCache = 0;

    for (TInt round = 0; round < kRounds; ++round)
        {
        iStatus.Format(OcStr(EStrCombinedRound), round + 1, kRounds);
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
    const TUint32 mhz = MeasureCoreMhz();
    AddLogF(_L("CLOCK %u MHz"), mhz);
    AddLogF(_L("ALU %u.%02u Mitr/s e=%d"), vAlu / 100, vAlu % 100, eAlu);
    AddLogF(_L("CACHE %u.%02u MB/s e=%d"), vCache / 100, vCache % 100, eCache);
    AddLogF(_L("MEM %u.%02u MB/s e=%d"), vMem / 100, vMem % 100, eMem);
    AddLogF(_L("SDRAM %u.%02u MB/s e=%d"), vInt / 100, vInt % 100, eInt);

    if (eAlu == 0 && eCache == 0 && eMem == 0 && eInt == 0)
        AddLog(OcStr(EStrVerdictClean));
    else if (eInt > 0 || eMem > 0)
        AddLog(OcStr(EStrVerdictWeakBus));
    else if (eCache > 0)
        AddLog(OcStr(EStrVerdictWeakCache));
    else
        AddLog(OcStr(EStrVerdictWeakCpu));
    }

// Trial connection of untested profile. Lives in a separate method because
// it is called prior to engine readiness check - on an unknown device it is
// not ready precisely because there is no profile yet.
// What the boot-time applier did last time, shown once. The flag is cleared
// here: a failure that has been read is no longer news. This also decides
// whether to open on the help page - see below.
void COcView::ReportBootOutcome()
    {
    if (OcBootLoad(iBoot) != KErrNone)
        {
        OcBootInit(iBoot);
        TInt uid = 0;
        HAL::Get(HALData::EMachineUid, uid);
        iBoot.iMachineUid = (TUint32)uid;
        }
    if (iBoot.iFlags & (KOcBootFlagFailed | KOcBootFlagSoakBad))
        {
        // Which of the two refusals it was matters to whoever is reading: one
        // says the phone did not survive, the other says only that it was
        // restarted too soon after a check, which is not the same news at all.
        TInt id = EStrBootSoakBad;
        if (iBoot.iFlags & KOcBootFlagFailed)
            id = (iBoot.iLastResult == (TUint32)EOcBootResRefusedRecent)
                 ? EStrBootRefusedRecent : EStrBootFailed;
        AddLog(OcStr(id));
        AddLogF(OcStr(EStrBootFailedCfg), (TInt)iBoot.iMult,
                (TInt)(iBoot.iCore & KCoreL3Mask));
        iBoot.iFlags &= ~(KOcBootFlagFailed | KOcBootFlagSoakBad);
        OcBootSave(iBoot);
        }
    // The paths that give up before writing a register leave the record armed
    // and set no flag, so without this they say nothing anywhere and autostart
    // looks simply ignored. That is what a phone whose unlock patches are not
    // applied at boot will do every time.
    else if (iBoot.iLastResult == (TUint32)EOcBootResNotReady ||
             iBoot.iLastResult == (TUint32)EOcBootResApplyFailed ||
             iBoot.iLastResult == (TUint32)EOcBootResNoProfile)
        {
        AddLog(OcStr(EStrBootNotApplied));
        iBoot.iLastResult = (TUint32)EOcBootResNone;
        OcBootSave(iBoot);
        }
    // Help on first run, and again after an update. Keyed on the version rather
    // than on a plain "already shown" flag: uninstalling does not reliably remove
    // the private directory, so a flag could survive a reinstall and hide the help
    // from someone seeing the application for the first time.
    const TUint32 h = OcBootHash(KAppVersion);
    if (iBoot.iHelpShown != h)
        {
        iHelp = ETrue;
        iHelpTop = 0;
        iBoot.iHelpShown = h;
        OcBootSave(iBoot);
        }
    // Opened while a soak started earlier is still running: there is no way to
    // know how much of the hold is left, so only the verdict is waited for.
    if (iBoot.iState == EOcBootCandidate)
        StartSoakWatch(0);
    }

// How long the verdict is waited for once the countdown has run out, and how
// much is added to the hold to cover the round of tests that follows it. Both
// are generous on purpose: overrunning the estimate costs a row that says
// CHECKING slightly too long, while cutting it short would report a soak as
// lost while ocboot is still working.
const TInt KSoakPollSec = 300;
const TInt KSoakMarginSec = 30;

// One second of the soak. The countdown is cosmetic; the verdict is not, and
// it comes from the record rather than from anything this process knows, since
// the soak runs in ocboot.exe and may outlive this window.
TInt COcView::SoakTick(TAny* aSelf)
    {
    COcView* self = (COcView*)aSelf;
    if (self->iSoakLeftSec > 0)
        {
        --self->iSoakLeftSec;
        self->Repaint();
        return 0;
        }
    TOcBootRecord rec;
    if (OcBootLoad(rec) == KErrNone && rec.iState != EOcBootCandidate)
        {
        self->iSoakTimer->Cancel();
        self->iSoakPollSec = 0;
        self->iBoot = rec;
        if (rec.iState == EOcBootArmed)
            self->AddLog(OcStr(EStrBootSoakArmed));
        else
            self->AddLog(OcStr((rec.iFlags & KOcBootFlagSoakBad)
                               ? EStrBootSoakBad : EStrBootOff));
        // Reported here, so it is not reported again at the next start.
        if (self->iBoot.iFlags & (KOcBootFlagFailed | KOcBootFlagSoakBad))
            {
            self->iBoot.iFlags &= ~(KOcBootFlagFailed | KOcBootFlagSoakBad);
            OcBootSave(self->iBoot);
            }
        self->Repaint();
        return 0;
        }
    // Nothing was written and the budget is spent: ocboot is gone without a
    // verdict, which the record already shows by staying a candidate - and a
    // candidate is exactly what boot ignores, so there is nothing to repair.
    if (--self->iSoakPollSec <= 0)
        self->iSoakTimer->Cancel();
    return 0;
    }

// Starts, or restarts, the watch. aSeconds is what is left of the hold; zero
// means the application was opened in the middle of a soak started earlier and
// only the verdict is waited for.
void COcView::StartSoakWatch(TInt aSeconds)
    {
    TInt err = KErrNone;
    if (!iSoakTimer)
        TRAP(err, iSoakTimer = CPeriodic::NewL(CActive::EPriorityStandard));
    if (err != KErrNone || !iSoakTimer)
        return;                 // no countdown; the soak itself is unaffected
    iSoakTimer->Cancel();
    iSoakLeftSec = aSeconds;
    iSoakPollSec = KSoakPollSec;
    iSoakTimer->Start(1000000, 1000000, TCallBack(COcView::SoakTick, this));
    }

// Saves the running configuration for the boot-time applier.
//
// Three conditions, and all three have to hold. The overclock must be live -
// and live means what the registers say, read back, not what the screen shows.
// It must then pass three rounds of the full test set here and now, because
// this configuration is going into daily use rather than into one session. And
// even then it is only a candidate: ocboot soaks it in the background and
// promotes it only if the phone is still alive afterwards.
void COcView::SaveForBoot()
    {
    if (!iEngine || !iEngine->IsReady())
        {
        AddLog(OcStr(EStrEngineDisabled));
        return;
        }
    TInt liveMult = 0;
    TUint32 liveCore = 0;
    if (iEngine->LiveMult(liveMult) != KErrNone ||
        iEngine->LiveCore(liveCore) != KErrNone)
        {
        AddLog(OcStr(EStrBootNoRead));
        return;
        }
    if (!iEngine->IsOverclocked() || liveMult <= iEngine->StockMult())
        {
        AddLog(OcStr(EStrBootNeedLive));
        return;
        }
    TUint32* buf = Buffer();
    if (!buf)
        {
        AddLog(OcStr(EStrNoMemory));
        return;
        }

    const TInt kRounds = 3;
    TInt errors = 0;
    for (TInt round = 0; round < kRounds; ++round)
        {
        iStatus.Format(OcStr(EStrBootTesting), round + 1, kRounds);
        Repaint();
        TTestResult r = TestAlu(800000);
        if (r.iErrors > 0) errors += r.iErrors;
        r = TestCache(buf, 200);
        if (r.iErrors > 0) errors += r.iErrors;
        r = TestMemBandwidth(buf, iBufWords, 4);
        if (r.iErrors > 0) errors += r.iErrors;
        r = TestMemIntegrity(buf, iBufWords);
        if (r.iErrors > 0) errors += r.iErrors;
        }
    iStatus.Zero();
    if (errors != 0)
        {
        AddLog(OcStr(EStrBootTestFailed));
        Repaint();
        return;
        }

    iBoot.iMult = (TUint32)liveMult;
    iBoot.iCore = liveCore;
    // The engine exposes no read-back for CM_CLKSEL_MPU, so this value is
    // composed exactly the way the apply path composes it. The multiplier and
    // CLKSEL1_CORE, which can be read, are read.
    iBoot.iMpu = (iEngine->StockMpu() & ~KMpuDivMask) |
                 ((TUint32)iMpuDiv & KMpuDivMask);
    iBoot.iDspDiv = (TUint32)iEngine->DspDiv();
    iBoot.iState = EOcBootCandidate;
    iBoot.iFlags = 0;
    if (!iBoot.iDelaySec)
        iBoot.iDelaySec = KOcBootDefaultDelaySec;
    if (!iBoot.iHoldSec)
        iBoot.iHoldSec = KOcBootDefaultHoldSec;
    if (OcBootSave(iBoot) != KErrNone)
        {
        AddLog(OcStr(EStrBootSaveFailed));
        return;
        }

    // What went into the record, in the same shape as the "ready: stock" line
    // above, so that the two can be compared by eye. The record is what boot
    // applies, and until this line existed nothing on the phone said whether the
    // dividers had been part of it.
    AddLogF(_L("boot cfg: m%d core %08x mpu %08x"),
            (TInt)iBoot.iMult, iBoot.iCore, iBoot.iMpu);

    // The soak measures the configuration that is live, so the configuration has
    // to stay live: with KEEP off, closing this window would put the phone back
    // to stock and the soak would then certify a stock clock as an overclock.
    if (!iKeep)
        {
        iKeep = ETrue;
        iEngine->KeepOnExit(ETrue);
        AddLog(OcStr(EStrBootKeepForced));
        }

    // The soak runs in its own process so that the phone can be used normally
    // while it waits, and so that closing this window does not cancel it.
    {
    RProcess p;
    if (p.Create(KOcBootExe, KSoakArg) == KErrNone)
        {
        p.Resume();
        p.Close();
        }
    }
    AddLogF(OcStr(EStrBootSaved), (TInt)(iBoot.iHoldSec / 60));
    AddLog(OcStr(EStrBootSoakNoReboot));
    AddLog(OcStr(EStrBootSoakWatchNote));
    AddLog(OcStr(EStrBootSoakWarn));
    AddLogF(OcStr(EStrBootApplyWarn), (TInt)iBoot.iDelaySec);
    StartSoakWatch((TInt)iBoot.iHoldSec + KSoakMarginSec);
    }

void COcView::ActivateTestProfile()
    {
    if (iKnownDevice || !iEngine)
        return;
    const TOcProfile* tp = TestProfile(iTestProfile);
    if (!tp)
        return;
    TBuf<48> nm;
    nm.Copy(TPtrC8((const TUint8*)tp->iId));
    AddLog(OcStr(EStrTestProfileWarn1));
    AddLog(OcStr(EStrTestProfileWarn2));
    AddLogF(OcStr(EStrTestProfileTrying), &nm);
    Repaint();
    iEngine->SetProfile(*tp);
    const TInt pr = iEngine->Prepare();
    if (pr == KErrNone)
        {
        // Status still says "unknown device" - after successful connection this
        // is no longer true, and it cannot be left on top of the working screen.
        iStatus.Zero();
        AddLog(OcStr(EStrTestProfileOk));
        if (iEngine->FoundGlobal())
            AddLogF(OcStr(EStrTestProfileFoundGlobal), iEngine->FoundGlobal());
        SeedFromLive();
        }
    else
        {
        iStatus.Format(_L("NOT READY (%d) at: %S"), pr,
                       &COcEngine::StepName(iEngine->FailedAt()));
        AddLog(iStatus);
        }
    Repaint();
    }

void COcView::Activate(TRowId aRow)
    {
    if (aRow == ERowHelp)
        {
        iHelp = ETrue;
        iHelpTop = 0;
        Repaint();
        return;
        }
    // Handle test profile before IsReady() check: engine is not ready
    // specifically on an unknown device, and this row exists precisely for such a device.
    // Leaving it after the check would make it never execute.
    if (aRow == ERowTestProfile && !iKnownDevice)
        {
        ActivateTestProfile();
        return;
        }
    // Switching autostart off has to work even with no engine: this row is the
    // way out of a configuration that misbehaves, and making it depend on a
    // working patcher would be backwards.
    if (aRow == ERowAutostart)
        {
        if (iBoot.iState == EOcBootOff)
            {
            AddLog(OcStr(EStrBootUseSave));
            }
        else
            {
            iBoot.iState = EOcBootOff;
            OcBootSave(iBoot);
            AddLog(OcStr(EStrBootOff));
            }
        return;
        }
    if (!iEngine || !iEngine->IsReady())
        {
        if (aRow >= ERowTestAlu && aRow != ERowHelp && aRow != ERowTestProfile)
            {
            RunTest(aRow);          // tests work without patcher as well
            return;
            }
        AddLog(OcStr(EStrEngineDisabled));
        return;
        }

    switch (aRow)
        {
        case ERowMode:
            if (!iManual)
                SyncManualFromAuto();
            iManual = !iManual;
            iArmed = EFalse;
            {
            TPtrC md(OcStr(iManual ? EStrModeManual : EStrModeAuto));
            AddLogF(_L("dividers: %S"), &md);
            }
            break;

        case ERowKeep:
            // Frequency outlives the process on its own - this is not our service, but
            // a hardware property. The only question is whether destructor clears it,
            // and the answer to that is now up to the user. Reboot restores stock
            // clocking in any case, so there is nothing irreversible here.
            iKeep = !iKeep;
            iEngine->KeepOnExit(iKeep);
            AddLog(OcStr(iKeep ? EStrKeepRowOn : EStrKeepRowOff));
            if (iKeep)
                AddLog(OcStr(EStrKeepNote));
            break;

        case ERowSaveBoot:
            SaveForBoot();
            break;

        case ERowLang:
            // OK cycles language just like the arrow keys: in a list of twenty-eight
            // entries searching for the right one is convenient either way.
            SetLanguage(OcStrLanguageIndex() + 1);
            break;

        case ERowL3:
            // OK applies nothing - only clears manual value and returns
            // field to auto-selection. Application, as with remaining CLKSEL1_CORE fields,
            // proceeds only via APPLY + CONFIRM: this divider changes in the
            // same write as multiplier, and has no separate fast path like DSP does.
            // for it.
            if (iL3Div)
                {
                iL3Div = 0;
                iArmed = EFalse;
                AddLog(OcStr(EStrBusBackToAuto));
                }
            else
                {
                AddLog(OcStr(EStrHintArrows));
                }
            break;

        case ERowDsp:
            {
            // Applied immediately, without arm/confirm. Reason: change is isolated
            // within the DSP domain - neither CORE, nor SDRAM, nor display are affected, so
            // there is no window in which the device could hang "on the fly", as with
            // DPLL relock. Reboot restores stock divider in any case.
            if (iDspDiv == iEngine->DspDiv())
                {
                AddLog(OcStr(EStrDspAlreadySet));
                break;
                }
            TInt rc = KErrGeneral;
            TInt r = iEngine->ApplyDspDiv(iDspDiv, rc);
            if (r == KErrNotSupported)
                {
                // Profile without setter address. Explain why, because otherwise it
                // looks like a broken menu item.
                AddLog(OcStr(EStrDspNoSetter));
                AddLog(OcStr(EStrDspNoSetterWhy));
                break;
                }
            if (r != KErrNone)
                {
                AddLogF(_L("dsp: not applied, r=%d"), r);
                break;
                }
            if (rc == KErrArgument)
                {
                AddLog(OcStr(EStrDspRejected));
                break;
                }
            if (rc != KErrNone)
                {
                AddLogF(_L("dsp: latch failed (rc=%d)"), rc);
                break;
                }
            // Read back: do not trust our own write - project rule.
            TInt live = 0;
            iEngine->LiveDspDiv(live);
            AddLogF(_L("dsp: /%d applied, live /%d"), iDspDiv, live);
            if (live != iDspDiv)
                {
                AddLog(OcStr(EStrDspMismatch));
                break;
                }
            // Return to stock - without watchdog: it is safe and permanent.
            if (!iEngine->IsDspChanged())
                {
                CancelDspRevert();
                break;
                }
            // Non-stock divider is released only under watchdog. Leaving it
            // forever is not allowed: measured two reboots out of two, one being
            // at complete idle, without any load on the DSP (findings §24.5).
            CancelDspRevert();
            TInt terr = KErrNone;
            if (!iDspTimer)
                TRAP(terr, iDspTimer = CPeriodic::NewL(CActive::EPriorityStandard));
            if (terr != KErrNone || !iDspTimer)
                {
                // No watchdog - no right to hold non-stock divider.
                AddLog(OcStr(EStrDspNoTimer));
                TInt rc2 = 0;
                iEngine->RestoreDspDiv(rc2);
                iDspDiv = iEngine->DspDiv();
                break;
                }
            iDspLeftSec = KDspHoldSec;
            iDspTimer->Start(1000000, 1000000,
                             TCallBack(COcView::DspRevertTick, this));
            AddLogF(OcStr(EStrDspHeld), iDspDiv,
                    KDspHoldSec);
            if (iDspDiv < iEngine->StockDspDiv())
                {
                // Not "could be", but measured: 330 MHz crashes the device as soon as
                // anything starts on C55x - audio, camera, whatever.
                AddLog(OcStr(EStrWarnDspAbove));
                AddLog(OcStr(EStrWarnDspUseMult));
                }
            break;
            }

        case ERowApply:
            // Only arms. Does NOT touch frequency: this is the same "separate application
            // option", followed by requiring separate confirmation.
            iArmedMult = iMult;
            iArmedCore = EffectiveCore();
            iArmedMpu = (iEngine->StockMpu() & ~KMpuDivMask) |
                        ((TUint32)iMpuDiv & KMpuDivMask);
            iArmed = ETrue;
            {
            TOcClocks c;
            iEngine->ClocksFor(iArmedMult, c);
            c.iMpuDiv = iMpuDiv;
            const TInt l3 = CoreFieldValue(iArmedCore, ECoreL3);
            AddLogF(_L("armed m%d CPU %u BUS %u"), iArmedMult, c.MpuKhz() / 1000,
                    l3 ? c.CoreKhz() / (TUint32)l3 / 1000 : 0);
            if (iArmedMult > KOcMultTested)
                AddLog(OcStr(EStrAboveTested));
            AddLog(OcStr(EStrPressConfirm));
            }
            break;

        case ERowConfirm:
            {
            if (!iArmed)
                {
                AddLog(OcStr(EStrNothingArmed));
                break;
                }
            TUint32 st = 0;
            iStatus.Format(OcStr(EStrApplying), iArmedMult);
            Repaint();
            // Allow window to draw and settle for a moment before the transition.
            User::After(150000);
            TInt r = iEngine->ApplyRampWithCore(iArmedMult, iArmedCore,
                                                iArmedMpu, st);
            iStatus.Zero();
            {
            // Whether divider write TOOK EFFECT. Hardware silently rejects entire write to
            // CM_CLKSEL1_CORE if any single field is invalid; previously
            // the application reported intent rather than result (findings 38).
            // Compare against what engine ACTUALLY submitted to hardware: it clamps
            // l3 field to the live value, because raising the bus is fatal (findings 38.2).
            const TUint32 wroteCore = iEngine->LastCoreWritten();
            // Kept as an assertion rather than as a clamp report: the engine
            // no longer alters the register it is given, so a difference here
            // would mean a write went astray.
            if (wroteCore != iArmedCore)
                AddLogF(OcStr(EStrBusClamped),
                        (TInt)(wroteCore & KCoreL3Mask));
            TUint32 liveCore = 0;
            if (iEngine->LiveCore(liveCore) == KErrNone)
                {
                iLiveL3 = (TInt)(liveCore & KCoreL3Mask);
                TPtrC verdict(OcStr((liveCore == wroteCore) ? EStrApplied
                                                           : EStrRejected));
                AddLogF(_L("core %08x (want %08x) l3=%d dss1=%d %S"),
                        liveCore, wroteCore,
                        (TInt)(liveCore & KCoreL3Mask),
                        (TInt)((liveCore & KCoreDss1Mask) >> KCoreDss1Shift),
                        &verdict);
                }
            }
            if (r == KErrOcCoreRejected)
                {
                // The engine has already walked the multiplier back: the
                // hardware refused the dividers, and the new multiplier on the
                // old dividers would overclock the display instead.
                TInt live = 0;
                iEngine->LiveMult(live);
                AddLogF(_L("dividers refused, back to m%d"), live);
                }
            else if (r != KErrNone)
                {
                AddLogF(_L("apply failed: %d st=%u"), r, st);
                }
            else
                {
                TInt live = 0;
                iEngine->LiveMult(live);
                AddLogF(_L("applied m%d (live %d) st=%u"), iArmedMult, live, st);
                }
            if (st != 0)
                {
                // Blob already rolled back to previous value on its own within
                // the critical section, so here we only report it.
                AddLog(OcStr(EStrRelockFailed));
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
            AddLogF(_L("restored: r=%d live m%d"), r, live);
            // We do not raise the bus back: four deaths out of four across two devices
            // (findings 38.2). Lowered bus is safe, reboot restores stock.
            if (iEngine->RestoreNeedsReboot())
                AddLog(OcStr(EStrBusLeftLowered));
            {
            TUint32 lc = 0;
            if (iEngine->LiveCore(lc) == KErrNone)
                iLiveL3 = (TInt)(lc & KCoreL3Mask);
            }
            // DSP divider - in the same "return everything to stock": otherwise one would have to
            // remember that RESTORE STOCK does not apply to everything.
            RestoreDspNow(OcStr(EStrReasonRestore));
            iMult = iEngine->StockMult();
            iCoreReg = iEngine->StockCore();
            iMpuDiv = iEngine->StockClocks().iMpuDiv;
            iL3Div = 0;                       // along with everything else - to auto-selection
            iDspDiv = iEngine->StockDspDiv();
            iArmed = EFalse;
            break;
            }

        default:
            if (aRow >= ERowDivFirst && aRow <= ERowDivLast)
                {
                // On divider row OK applies nothing - value
                // cycles left/right. Reduces chance of accidental trigger.
                AddLog(OcStr(EStrHintArrowsDiv));
                }
            else
                {
                RunTest(aRow);
                }
            break;
        }
    }

TKeyResponse COcView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
    {
    if (aType != EEventKey)
        return EKeyWasNotConsumed;

    if (iHelp)
        {
        const TInt maxTop = HelpTotalLines() - HelpVisibleLines();
        if (aKeyEvent.iCode == EKeyDownArrow)
            {
            if (iHelpTop < maxTop)
                ++iHelpTop;
            }
        else if (aKeyEvent.iCode == EKeyUpArrow)
            {
            if (iHelpTop > 0)
                --iHelpTop;
            }
        else
            {
            iHelp = EFalse;
            }
        Repaint();
        return EKeyWasConsumed;
        }

    const TInt vis = VisibleRows();
    switch (aKeyEvent.iCode)
        {
        case EKeyUpArrow:
            iRow = (iRow + ERowCount - 1) % ERowCount;
            break;
        case EKeyDownArrow:
            iRow = (iRow + 1) % ERowCount;
            break;

        case EKeyLeftArrow:
        case EKeyRightArrow:
            {
            const TInt dir = (aKeyEvent.iCode == EKeyRightArrow) ? 1 : -1;
            if (iRow == ERowSlider)
                {
                TInt m = iMult + dir;
                if (m >= KOcMultMin && m <= KOcMultMax)
                    {
                    iMult = m;
                    iArmed = EFalse;
                    // In auto mode dividers recalculate on their own; in manual
                    // they remain as is - that is the whole point of manual.
                    }
                }
            else if (iRow == ERowMpu)
                {
                TInt nv = MpuDivStep(iMpuDiv, dir);
                if (nv != iMpuDiv)
                    {
                    iMpuDiv = nv;
                    iArmed = EFalse;
                    }
                }
            else if (iRow == ERowL3)
                {
                // Adjust from WHAT is currently shown, not from saved value:
                // while divider is on auto-selection, shown value came from
                // CompensatedCore, and step must proceed from it.
                const TInt cur = CoreFieldValue(EffectiveCore(), ECoreL3);
                const TInt nv = L3DivStep(cur, dir);
                if (nv != cur)
                    {
                    iL3Div = nv;
                    // Mirror into manual register so L3 row in list below
                    // shows the same: two controls for one field that
                    // diverge would be an outright trap.
                    iCoreReg = CoreFieldSet(iCoreReg, ECoreL3, nv);
                    iArmed = EFalse;
                    WarnBus(nv);
                    }
                }
            else if (iRow == ERowTestProfile)
                {
                const TInt cnt = TestProfileCount();
                if (!iKnownDevice && cnt > 0)
                    {
                    iTestProfile = (iTestProfile + dir + cnt) % cnt;
                    AddLog(OcStr(EStrTestProfilePick));
                    }
                }
            else if (iRow == ERowLang)
                {
                SetLanguage(OcStrLanguageIndex() + dir);
                }
            else if (iRow == ERowDsp)
                {
                // Cycles freely, nothing goes into hardware: application only
                // on OK on this same row. Leave iArmed untouched - DSP path is separate
                // from arm/confirm for core frequency.
                iDspDiv = DspDivStep(iDspDiv, dir);
                }
            else if (iRow == ERowMode)
                {
                if (!iManual)
                    SyncManualFromAuto();
                iManual = !iManual;
                iArmed = EFalse;
                }
            else if (iRow >= ERowDivFirst && iRow <= ERowDivLast)
                {
                if (!iManual)
                    {
                    AddLog(OcStr(EStrNeedManual));
                    }
                else
                    {
                    const TInt id = iRow - ERowDivFirst;
                    TInt v = CoreFieldValue(iCoreReg, id);
                    TInt nv = CoreFieldStep(id, v, dir);
                    if (nv != v)
                        {
                        iCoreReg = CoreFieldSet(iCoreReg, id, nv);
                        // Same field as BUSDIV row at the top: keep them
                        // at the same value, otherwise top row would silently
                        // override what was just entered here.
                        if (id == ECoreL3)
                            {
                            iL3Div = nv;
                            WarnBus(nv);
                            }
                        iArmed = EFalse;
                        }
                    }
                }
            break;
            }

        case EKeyOK:
        case EKeyEnter:
            Activate((TRowId)iRow);
            break;

        case EKeyEscape:
        case EKeyDevice1:
            CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
            return EKeyWasConsumed;

        default:
            // Digits 1..9 on MULT row - fast jump to multiples of 5% from stock.
            if (iRow == ERowSlider && aKeyEvent.iCode >= '0' && aKeyEvent.iCode <= '9')
                {
                // StockMult() is zero until the engine is ready, and zero here
                // used to propagate straight into iMult, below the allowed
                // minimum. Fall back to the value both supported phones share.
                TInt stock = iEngine ? iEngine->StockMult() : 0;
                if (stock <= 0)
                    stock = 86;
                TInt pct = (aKeyEvent.iCode - '0') * 5;
                TInt m = stock + (stock * pct) / 100;
                if (m < KOcMultMin)
                    m = KOcMultMin;
                iMult = (m > KOcMultMax) ? KOcMultMax : m;
                iArmed = EFalse;
                }
            else
                return EKeyWasNotConsumed;
            break;
        }
    EnsureVisible(vis);
    DrawNow();
    return EKeyWasConsumed;
    }

// Draws string clipped to window width.
//
// Without this a long line simply went beyond screen edge, and end of text became
// invisible - this was exactly how the name with version vanished on N95 (device remark,
// 2026-09-12). We clip by characters rather than pixels: half a glyph
// would look like a rendering artifact.
static void DrawClipped(CWindowGc& aGc, const CFont* aFont, const TDesC& aText,
                        const TPoint& aPt, TInt aWidth)
    {
    if (aFont->TextWidthInPixels(aText) <= aWidth)
        {
        aGc.DrawText(aText, aPt);
        return;
        }
    TInt n = aFont->TextCount(aText, aWidth);
    if (n < 0)
        n = 0;
    aGc.DrawText(aText.Left(n), aPt);
    }

// Prints text, wrapping it across window width.
//
// Manual wraps in source do not work: screen width is known only on device,
// and what fit across 240 pixels of N95 gets clipped on narrower device and leaves
// empty space on wider one. Therefore text in src/strings.json resides in paragraphs, and
// this function breaks it - at spaces, and only when a word does not fit entirely,
// in the middle of a word.
//
// A single function does two jobs: with aGc == NULL it draws nothing, only
// counts lines. This is needed for help scrolling, whose boundaries without wrapping
// are unknown. aSkip - how many formatted lines to skip (scroll top).
// Returns total number of lines text occupies.
static TInt WrapText(CWindowGc* aGc, const CFont* aFont, const TDesC& aText,
                     TInt aX, TInt& aY, TInt aLh, TInt aYMax, TInt aWidth,
                     TInt& aSkip)
    {
    const TInt len = aText.Length();
    TInt lines = 0;
    TInt pos = 0;
    for (;;)
        {
        TPtrC seg;
        if (pos >= len)
            {
            if (len != 0)
                break;
            seg.Set(KNullDesC);          // empty paragraph - empty line
            }
        else
            {
            TPtrC rest = aText.Mid(pos);
            TInt n = aFont->TextCount(rest, aWidth);
            if (n <= 0)
                n = 1;
            if (n < rest.Length())
                {
                const TInt sp = rest.Left(n + 1).LocateReverse(' ');
                if (sp > 0)
                    n = sp;
                }
            seg.Set(rest.Left(n));
            pos += n;
            while (pos < len && aText[pos] == ' ')
                ++pos;
            }
        ++lines;
        if (aSkip > 0)
            --aSkip;
        else if (aGc && aY <= aYMax)
            {
            aGc->DrawText(seg, TPoint(aX, aY));
            aY += aLh;
            }
        if (len == 0)
            break;
        }
    return lines;
    }

// Full-screen help. In the same small font as the rest: strings in
// src/strings.json are already broken for this width, so there is no wrapping here.
void COcView::DrawHelp(CWindowGc& aGc, const TRect& aRect) const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    aGc.UseFont(font);
    aGc.SetPenColor(KRgbBlack);
    const TInt lh = font->HeightInPixels() + 1;
    const TInt x = aRect.iTl.iX + 2;
    const TInt w = aRect.Width() - 4;
    const TInt yMax = aRect.iBr.iY - 2;
    TInt y = aRect.iTl.iY + lh;
    TInt none = 0;
    WrapText(&aGc, font, OcStr(EStrHelpTitle), x, y, lh, yMax, w, none);
    y += lh;
    TInt skip = iHelpTop;
    const TInt total = OcHelpLineCount();
    for (TInt i = 0; i < total && y <= yMax; ++i)
        WrapText(&aGc, font, OcHelpLine(i), x, y, lh, yMax, w, skip);
    aGc.DiscardFont();
    }

// How many lines help occupies AFTER wrapping. Depends on window width,
// so calculated each time rather than remembered.
TInt COcView::HelpTotalLines() const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt w = Rect().Width() - 4;
    TInt y = 0;
    TInt skip = 0;
    TInt n = 0;
    const TInt total = OcHelpLineCount();
    for (TInt i = 0; i < total; ++i)
        n += WrapText(NULL, font, OcHelpLine(i), 0, y, 1, 0, w, skip);
    return n;
    }

TInt COcView::HelpVisibleLines() const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt lh = font->HeightInPixels() + 1;
    const TInt n = (Rect().Height() - lh * 2) / lh;
    return n > 1 ? n : 1;
    }

void COcView::Draw(const TRect& /*aRect*/) const
    {
    CWindowGc& gc = SystemGc();
    TRect r = Rect();
    gc.SetPenStyle(CGraphicsContext::ESolidPen);
    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    gc.SetBrushColor(KRgbWhite);
    gc.Clear(r);

    if (iHelp)
        {
        DrawHelp(gc, r);
        return;
        }

    // Small font and explicit clipping: with NormalFont content did not fit, and log
    // lines were drawn beyond screen edge (looking like "tests do not work").
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    gc.UseFont(font);
    const TInt lh = font->HeightInPixels() + 1;
    const TInt x = r.iTl.iX + 2;
    const TInt w = r.Width() - 4;
    TInt noSkip = 0;                        // WrapText skips nothing
    // Log is anchored to the bottom edge: first compute how much it will take,
    // and allocate only the remainder to header with list. Otherwise long list pushes
    // test results off screen right when they just appeared.
    const TInt logH = LogLines();
    const TInt logY = r.iBr.iY - 2 - (logH > 0 ? (logH - 1) * lh : 0);
    const TInt yMax = (logH > 0) ? logY - lh : r.iBr.iY - lh;
    TInt y = r.iTl.iY + lh;
    TBuf<96> line;
    gc.SetPenColor(KRgbBlack);

    const TBool ready = (iEngine && iEngine->IsReady());

    // --- header: stock state and what will result after application ---
    if (ready)
        {
        const TOcClocks& st = iEngine->StockClocks();
        line.Format(_L("stock m%d CPU %u BUS %u DSS %u"), st.iMult,
                    st.MpuKhz() / 1000, st.L3Khz() / 1000, st.Dss1Khz() / 1000);
        WrapText(&gc, font, line, x, y, lh, yMax, w, noSkip);

        TOcClocks c;
        iEngine->ClocksFor(iMult, c);
        c.iMpuDiv = iMpuDiv;
        const TUint32 core = EffectiveCore();
        TInt l3 = CoreFieldValue(core, ECoreL3);
        TInt ds = CoreFieldValue(core, ECoreDss1);
        line.Format(_L("new   m%d CPU %u BUS %u DSS %u"), iMult,
                    c.MpuKhz() / 1000,
                    l3 ? c.CoreKhz() / (TUint32)l3 / 1000 : 0,
                    ds ? c.CoreKhz() / (TUint32)ds / 1000 : 0);
        WrapText(&gc, font, line, x, y, lh, yMax, w, noSkip);
        line.Format(_L("CLKSEL1_CORE %08x"), core);
        WrapText(&gc, font, line, x, y, lh, yMax, w, noSkip);
        }
    else
        {
        DrawClipped(gc, font, OcStr(EStrNotReady), TPoint(x, y), w);
        y += lh;
        if (iStatus.Length())
            {
            WrapText(&gc, font, iStatus, x, y, lh, yMax, w, noSkip);
            }
        y += lh;
        }

    // --- scrollable list ---
    const TInt vis = VisibleRows();
    TInt last = iTop + vis;
    if (last > ERowCount)
        last = ERowCount;
    for (TInt row = iTop; row < last && y < yMax; ++row)
        {
        TBuf<96> label;
        RowLabel(row, label);
        line.Format(_L("%c%S"), (iRow == row) ? '>' : ' ', &label);
        WrapText(&gc, font, line, x, y, lh, yMax, w, noSkip);
        }
    // Marker indicating list is longer than screen.
    if (last < ERowCount && y < yMax)
        {
        line.Format(OcStr(EStrMoreRows), ERowCount - last);
        WrapText(&gc, font, line, x, y, lh, yMax, w, noSkip);
        }

    if (ready && iStatus.Length() && y < yMax)
        {
        WrapText(&gc, font, iStatus, x, y, lh, yMax, w, noSkip);
        }
    // Top of log shows NEWEST entry. Previously new line went to the bottom, i.e.
    // farthest from eye and first beyond screen edge, becoming visible only
    // when pushed by an even newer one.
    if (logH > 0)
        {
        TInt ly = logY;
        const TInt lyMax = r.iBr.iY - 2;
        for (TInt i = iLogCount - 1; i >= 0 && ly <= lyMax; --i)
            WrapText(&gc, font, iLog[i], x, ly, lh, lyMax, w, noSkip);
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
    // ENoScreenFurniture removes both status pane and softkey pane, so
    // application occupies entire screen in both orientations. Along with it rect must
    // be taken from ApplicationRect() rather than ClientRect(): the latter returns the area
    // UNDER furniture, which is now absent.
    BaseConstructL(CAknAppUi::ENoAppResourceFile | CAknAppUi::EAknEnableSkin |
                   CAknAppUi::ENoScreenFurniture);
    iView = COcView::NewL(ApplicationRect());
    // AddToStackL can leave when memory is short. Without the cleanup stack the
    // view - and with it the engine and its open kernel channels - would simply
    // be lost.
    CleanupStack::PushL(iView);
    AddToStackL(iView);
    CleanupStack::Pop(iView);
    }

// Screen rotation changes geometry, and without this application would remain with
// a vertical rectangle in landscape mode - exactly why results were readable
// normally only in one orientation.
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
        // Order matters: first remove overclock, then destroy control.
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
