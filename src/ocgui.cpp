// SymbianOverclock: розгін і стрес-тести прямо на апараті.
//
// Керування (джойстик):
//   вгору/вниз    - вибір рядка
//   вліво/вправо  - на рядку MULT рухає повзунок (крок 1; з утриманням - швидше),
//                   на MPUDIV і BUSDIV перебирає допустимі дільники
//   OK на BUSDIV  - знімає ручне значення, повертає дільник на автопідбір
//   OK/select     - активує вибраний рядок
//   права софт-клавіша або Esc - вихід (перед виходом штатна частота
//                   ПОВЕРТАЄТЬСЯ автоматично)
//
// Захист від випадкового розгону навмисний і дворівневий: повзунок лише
// ГОТУЄ значення, "APPLY SETTING" його озброює, і тільки окрема кнопка
// "CONFIRM OVERCLOCK" виконує relock. Після перезавантаження нічого не
// застосовується само - застосунок не ставить жодного startup-запису.
//
// Рядки логу навмисно ASCII: GCCE 3.4.3 і 8-бітні дескриптори не варті ризику
// з кирилицею в літералах.

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

#include "ocengine.h"
#include "ocstr.h"

// Версія збірки. На апараті інакше неможливо сказати, яка саме стоїть.
_LIT(KAppVersion, "SymbianOverclock 1.9.0");

const TUid KOcGuiAppUid = { 0xE0001011 };

// --- прилади: кожен водночас і стрес-тест, і бенчмарк -----------------------
//
// Головна вимога до всіх: САМОПЕРЕВІРКА. На розгоні цікава не швидкість, а
// момент, коли підсистема починає тихо помилятися, і побачити це можна лише
// тестом із відомою відповіддю. Тому:
//   * ALU крутить LCG вперед, а потім рівно стільки ж кроків назад через
//     модульно обернений множник - будь-яка одинична помилка ламає повернення
//     до початкового значення;
//   * пам'ять і кеш пишуть псевдовипадковий візерунок і звіряють його назад.
// Ані перший, ані другі не потребують еталона, знятого на іншій частоті.

const TUint32 KLcgA = 1664525u;
const TUint32 KLcgC = 1013904223u;

struct TTestResult
    {
    TUint32 iValue;        // пропускна здатність, у сотих одиниці
    TInt    iErrors;       // -1 = тест не виконувався
    };

// Модульно обернений KLcgA за модулем 2^32 (ітерація Ньютона; для непарного
// множника завжди існує). Рахуємо, а не зашиваємо: константа, вписана руками,
// перетворила б тест правильності на тест моєї арифметики.
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

// ALU: вперед і назад. Повертає Mitr/s у сотих і кількість помилок (0 або 1).
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

// Смуга пам'яті: обхід буфера, більшого за кеш, кроком у рядок кешу.
// Впирається в L3/SDRAM, тобто показує шину, а не ядро.
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
    r.iValue = us ? (TUint32)(((TUint64)lines * 64u * 100u) / us) : 0;  // МБ/с*100
    r.iErrors = (acc == 0) ? 1 : 0;     // нуль тут неможливий: візерунок ненульовий
    return r;
    }

// Цілісність SDRAM: пишемо візерунок, читаємо назад, рахуємо розбіжні слова.
// Саме цей тест ловить головний ризик розгону шини - тихе псування даних.
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
    TUint32 bytes = (TUint32)aWords * 4u * 2u;          // запис + читання
    r.iValue = us ? (TUint32)(((TUint64)bytes * 100u) / us) : 0;
    r.iErrors = bad;
    return r;
    }

// Кеш: той самий візерунок, але робочий набір явно менший за L1 (16 КБ на
// ARM1136). Відділяє помилки ядра й кеша від помилок зовнішньої шини.
static TTestResult TestCache(TUint32* aBuf, TInt aRounds)
    {
    TTestResult r;
    const TInt kWords = 1024;                            // 4 КБ
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

// Наступне/попереднє значення дільника шини.
//
// Набір навмисно ВУЖЧИЙ за той, що дозволяє валідатор прошивки: беремо
// KAutoL3Allowed (1, 2, 4, 6, 8, 12, 16). Причина виміряна, а не з обережності:
// `dssdiv=7` проходить валідацію power.dll і при цьому НЕ лягає в регістр, ба
// більше - недійсне значення в одному полі відкидає ВЕСЬ запис у
// CM_CLKSEL1_CORE (findings §26.0). У рядок, який крутять пальцем, пускати такі
// значення нема сенсу: користувач побачив би "застосовано", а в залізі не
// змінилося б нічого. Ширший набір лишається в MANUAL, де є звірка читанням.
static TInt L3DivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 1 && v <= 31; v += aDir)
        {
        if (KAutoL3Allowed & (1u << v))
            return v;
        }
    return aValue;
    }

// --- контрол ----------------------------------------------------------------

// Рядки списку. Дільники йдуть окремими рядками, тож усе разом на екран уже не
// влазить - список прокручується (див. EnsureVisible/Draw).
//
// Дільник шини памʼяті (поле L3) має ОКРЕМИЙ рядок нагорі, хоч і живе в тому
// самому CM_CLKSEL1_CORE, що й решта полів у списку нижче. Причина не в
// зручності: це єдиний дільник, яким міряють, де насправді межа - в ядрі чи в
// шині. Виміряно (findings §26.13), що BestMem реагує на зміну шини майже
// повністю (l3=8 дає -45%), а на зміну частоти ядра - лише на дві п'ятих. Щоб
// цим користуватись, шину треба крутити окремо від множника, не заходячи в
// MANUAL і не чіпаючи водночас дисплей.
enum TRowId
    {
    ERowSlider = 0,      // множник DPLL
    ERowMpu,             // дільник MPU (окремий регістр CM_CLKSEL_MPU)
    ERowL3,              // дільник шини памʼяті - поле L3 у CM_CLKSEL1_CORE
    ERowBusGuard,        // запобіжник підйому шини; типово увімкнений
    ERowDsp,             // дільник домену DSP (окремий регістр CM_CLKSEL_DSP)
    ERowMode,            // AUTO / MANUAL для дільників CLKSEL1_CORE
    ERowDivFirst,        // 7 рядків дільників CM_CLKSEL1_CORE
    ERowDivLast = ERowDivFirst + ECoreFieldCount - 1,
    ERowApply,
    ERowConfirm,
    ERowRestore,
    ERowTestAlu,
    ERowTestMem,
    ERowTestIntegrity,
    ERowTestCache,
    ERowTestCombined,
    ERowHelp,            // вбудована інструкція
    ERowTestProfile,     // ручний вибір неперевіреного профілю
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
    TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
    void ShutdownL();

private:
    COcView();
    void ConstructL(const TRect& aRect);
    void Activate(TRowId aRow);
    void EnsureVisible(TInt aVisibleRows);
    TInt VisibleRows() const;
    // Значення CM_CLKSEL1_CORE, яке піде на апарат: або автопідбір, або ручне.
    TUint32 EffectiveCore() const;
    TUint32 BusKhz() const;
    void WarnBus(TInt aDiv);
    void RowLabel(TInt aRow, TDes& aOut) const;
    void SyncManualFromAuto();
    void RunTest(TRowId aRow);
    void RunCombined();
    // Автоповернення дільника DSP. Виміряно (findings §24.5): нештатний дільник,
    // лишений у системі надовго, валить апарат у ребут навіть без жодного
    // навантаження на DSP, а витримка з гарантованим поверненням проходить
    // чисто. Тому дільник тут не «стоїть, поки не приберуть», а живе рівно
    // відведене вікно і повертається сам, навіть якщо застосунок просто лишили
    // відкритим.
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
    TInt   iTop;             // перший видимий рядок (список прокручується)
    TInt   iMult;            // значення повзунка
    TBool  iManual;          // дільники задає користувач, а не автопідбір
    TUint32 iCoreReg;        // редагований CM_CLKSEL1_CORE у ручному режимі
    TInt   iMpuDiv;          // редагований дільник MPU (1,2,4,6,8)
    TInt   iL3Div;           // дільник шини; 0 = лишити на автопідборі
    TInt   iLiveL3;          // дільник шини, який СТОЇТЬ у залізі (0 = не читали)
    TBool  iBusGuard;        // ETrue = підйом шини заборонено (типово)
    TInt   iDspDiv;          // редагований дільник DSP (окремий від iArmed шлях)
    CPeriodic* iDspTimer;    // сторож автоповернення дільника DSP
    TInt   iDspLeftSec;      // скільки секунд лишилось до автоповернення
    TBool  iArmed;           // "APPLY SETTING" натиснуто, чекаємо підтвердження
    TInt   iArmedMult;
    TUint32 iArmedCore;
    TUint32 iArmedMpu;
    TBuf<64> iLog[KLogLines];
    TInt   iLogCount;
    TUint32* iBuf;
    TInt   iBufWords;
    TBuf<80> iStatus;
    TBool  iKnownDevice;     // профіль добрано автоматично
    TInt   iTestProfile;     // вибраний тестовий профіль; -1 = жоден
    TBool  iHelp;            // ETrue = показуємо сторінку довідки
    TInt   iHelpTop;         // з якого рядка довідки малюємо
    };

COcView::COcView()
  : iEngine(NULL), iRow(ERowSlider), iTop(0), iMult(0), iManual(EFalse),
    iCoreReg(0), iMpuDiv(2), iL3Div(0), iLiveL3(0), iBusGuard(ETrue), iDspDiv(3), iDspTimer(NULL), iDspLeftSec(0),
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

void COcView::ConstructL(const TRect& aRect)
    {
    // Мову беремо один раз і до першого видимого рядка: інакше частина
    // журналу лишилась би англійською, а частина - ні.
    OcStrInit();
    CreateWindowL();
    SetRect(aRect);

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        gFcFreq = (TUint32)fc;

    iEngine = COcEngine::NewL();

    // Профіль апарата добираємо самі: командного рядка тут немає, а чужий
    // профіль означав би патч чужих адрес у ядро. Невідомий апарат - чесна
    // відмова ДО будь-якого дотику до ядра, а не здогад.
    const TOcProfile* prof = ProfileForThisDevice();
    if (!prof)
        {
        TInt uid = 0;
        HAL::Get(HALData::EMachineUid, uid);
        iStatus.Format(OcStr(EStrUnknownDevice), (TUint32)uid);
        AddLog(iStatus);
        AddLogF(OcStr(EStrNoProfileUid),
                (TUint32)uid);
        // Невідомий апарат - це не глухий кут, а початок портування, і рівно
        // ці два рядки потрібні, щоб зробити для нього профіль. Тому вони
        // видимі просто на екрані, а не сховані в збірці для розробника.
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
        ActivateL();       // вікно мусить лишитись живим, щоб відмову було видно
        return;
        }
    iKnownDevice = ETrue;
    iEngine->SetProfile(*prof);
    {
    TBuf<32> pid;
    pid.Copy(TPtrC8((const TUint8*)prof->iId));
    AddLogF(_L("profile: %S  %S"), &pid, &KAppVersion);
    }

    TInt r = iEngine->Prepare();
    if (r == KErrNone)
        {
        // Повзунок ставимо на ФАКТИЧНИЙ множник апарата, а опорна точка
        // (StockCore/StockMpu) - стала. Якщо застали апарат розігнаним, це видно
        // в лозі окремим рядком, а не мовчки стає новою нормою.
        iMult = iEngine->BootMult();
        iArmedMult = iMult;
        iCoreReg = iEngine->StockCore();
        iArmedCore = iCoreReg;
        iMpuDiv = (TInt)(iEngine->BootMpu() & KMpuDivMask);
        iArmedMpu = iEngine->StockMpu();
        iDspDiv = iEngine->StockDspDiv();
        iLiveL3 = (TInt)(iEngine->BootCore() & KCoreL3Mask);
        AddLogF(_L("ready: stock m%d core %08x"), iEngine->StockMult(), iCoreReg);
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
        // Розбіжність прошивки - не поломка, а свідома відмова, і сплутати
        // її з "драйвер не став" не можна: лікується вона зовсім іншим.
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
    User::Free(iBuf);
    delete iEngine;
    }

// Стеля, на яку взагалі дозволено відпускати нештатний дільник DSP.
//
// Це НЕ «перевірено безпечний час»: безпечного часу тут немає. Виміряно
// (findings §24.6), що домен на CORE/2 живий рівно доти, доки на C55x нічого не
// виконується, і валить апарат, щойно DSP піднімають - хоч нашим дочірнім
// процесом, хоч штатним аудіо через десять хвилин. Тому сторож не «пересиджує
// небезпеку», а лише обмежує вікно, в яке така подія може влучити.
const TInt KDspHoldSec = 120;

void COcView::CancelDspRevert()
    {
    if (iDspTimer)
        {
        iDspTimer->Cancel();
        delete iDspTimer;
        iDspTimer = NULL;
        }
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
        self->Repaint();            // видно, скільки лишилось
        return 0;
        }
    self->RestoreDspNow(OcStr(EStrReasonTimeout));
    return 0;
    }

// Обидві зміни переживають наш процес, тож знімати їх треба явно й у порядку:
// спершу частота, потім підмінене тіло функції ядра. Лишити друге - означає
// поставити міну: будь-який інший клієнт dvfs_interface, що викличе DoControl(1)
// з ненульовим a1, розіменує довільну адресу в режимі ядра.
void COcView::ShutdownL()
    {
    if (!iEngine)
        return;
    if (iEngine->IsOverclocked())
        {
        TUint32 st = 0;
        iEngine->RestoreStock(st);
        }
    CancelDspRevert();
    if (iEngine->IsDspChanged())
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
        iBufWords = 512 * 1024;                 // 2 МБ - явно повз кеш
        iBuf = (TUint32*)User::Alloc(iBufWords * sizeof(TUint32));
        if (!iBuf)
            {
            iBufWords = 64 * 1024;              // 256 КБ - запасний варіант
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

// Тести блокують цикл подій, тож перемальовуємо явно й проштовхуємо чергу
// вікон - інакше екран завмирає на весь прогін.
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

// У ручному режимі йде відредагований регістр, в авто - підібраний двигуном.
// Понад це: щойно користувач торкнувся рядка BUSDIV, його значення перекриває
// поле L3 в обох режимах. Заради цього рядок і винесено нагору - щоб шину можна
// було задати, не заходячи в MANUAL і не чіпаючи дільник дисплея, який в авто
// лишається компенсованим.
TUint32 COcView::EffectiveCore() const
    {
    if (!iEngine || !iEngine->IsReady())
        return iCoreReg;
    TUint32 core = iManual ? iCoreReg : iEngine->CompensatedCore(iMult);
    if (iL3Div > 0)
        core = CoreFieldSet(core, ECoreL3, iL3Div);
    return core;
    }

// Частота шини при вибраному множнику й ПОТОЧНОМУ дільнику L3, у кГц.
TUint32 COcView::BusKhz() const
    {
    if (!iEngine || !iEngine->IsReady())
        return 0;
    TOcClocks c;
    iEngine->ClocksFor(iMult, c);
    const TInt v = CoreFieldValue(EffectiveCore(), ECoreL3);
    return v ? c.CoreKhz() / (TUint32)v : 0;
    }

// Попередження про напрямок зміни шини. Обидва - виміряні, не гіпотетичні.
void COcView::WarnBus(TInt aDiv)
    {
    if (!iEngine || !iEngine->IsReady())
        return;
    const TInt stock = iEngine->StockClocks().iL3Div;
    if (aDiv > stock)
        {
        // Найдорожчий відомий факт цього рядка: сама шина на /8 тримається
        // чисто (два прогони, нуль зіпсованих слів у SDRAM), а ось ПОВЕРНЕННЯ
        // з неї валило апарат три рази з трьох - і повним relock, і легким
        // переписом дільників (findings §26.12-26.14).
        AddLog(OcStr(EStrWarnBusBelow));
        AddLog(OcStr(EStrWarnBusReturn));
        }
    else if (aDiv < stock)
        {
        AddLog(OcStr(EStrWarnBusAbove));
        }
    }

// Перемикаючись у ручний режим, стартуємо не з нуля, а з того, що підібрала
// автоматика для поточного множника: так у користувача одразу робоча база.
void COcView::SyncManualFromAuto()
    {
    if (iEngine && iEngine->IsReady())
        iCoreReg = iEngine->CompensatedCore(iMult);
    }

TInt COcView::VisibleRows() const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    const TInt lh = font->HeightInPixels() + 1;
    // 3 рядки шапки + рядок статусу + лог
    TInt avail = Rect().Height() / lh - 4 - KLogLines;
    if (avail < 4)
        avail = 4;
    return avail;
    }

void COcView::EnsureVisible(TInt aVisibleRows)
    {
    if (iRow < iTop)
        iTop = iRow;
    if (iRow >= iTop + aVisibleRows)
        iTop = iRow - aVisibleRows + 1;
    if (iTop < 0)
        iTop = 0;
    }

// Один рядок списку в готовому вигляді. Для дільників показуємо не лише
// значення, а й отриману частоту і відхилення від штатної - саме за цим
// відхиленням і налаштовують.
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
        // CPU = CORE / mpu_div. Дільник 1 дає ту саму частоту ядра при вдвічі
        // нижчій CORE - це і є перевірка, чи межа в ARM, чи в CORE.
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
        // Показуємо не сам дільник, а частоту шини й відхилення від штатної:
        // налаштовують саме за ним. Позначка auto означає, що значення бере
        // автопідбір і воно поїде разом із множником.
        const TInt v = ready ? CoreFieldValue(EffectiveCore(), ECoreL3)
                             : (iL3Div ? iL3Div : 4);
        if (!ready)
            {
            aOut.Format(_L(" BUSDIV /%d"), v);
            return;
            }
        // Затиск шини показуємо ДО застосування, а не лише в лозі після
        // нього. Менший дільник за живий - це підйом шини, і двигун його
        // не пропустить (findings §38.2). Рядок мусить називати те
        // значення, яке справді піде в залізо, інакше інтерфейс бреше про
        // власну роботу, і виглядає це рівно як "дільник не працює".
        if (iBusGuard && iLiveL3 && v < iLiveL3)
            {
            TOcClocks cc;
            iEngine->ClocksFor(iMult, cc);
            aOut.Format(OcStr(EStrRowBusClamped), v, iLiveL3,
                        cc.CoreKhz() / (TUint32)iLiveL3 / 1000);
            return;
            }
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
        // Тестовий профіль потрібен лише там, де апарат не впізнано. На
        // відомому апараті пропонувати неперевірені адреси нема сенсу й
        // шкідливо: доведений профіль завжди кращий.
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
    if (aRow == ERowBusGuard)
        {
        // Рядок каже не лише стан, а й ціну: чотири смерті з чотирьох -
        // це єдина цифра, яка тут щось важить.
        if (iBusGuard)
            aOut.Copy(OcStr(EStrGuardRowOn));
        else
            aOut.Copy(OcStr(EStrGuardRowOff));
        return;
        }
    if (aRow == ERowDsp)
        {
        // DSP тактується від CORE, тож його частота залежить і від множника,
        // і від цього дільника. Показуємо результат для ВИБРАНОГО множника,
        // як і в решті рядків, плюс те, що стоїть у залізі просто зараз.
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
            {                                  // не дільник: лише сире значення
            aOut.Format(_L(" %S = %d"), &nm, v);
            return;
            }
        // Частота цього домену при ВИБРАНОМУ множнику й цьому дільнику.
        TOcClocks c;
        iEngine->ClocksFor(iMult, c);
        TUint32 base = c.CoreKhz();
        if (f.iBase == 1)                      // L4 ділиться від L3, не від CORE
            {
            TInt l3 = CoreFieldValue(core, ECoreL3);
            base = l3 ? c.CoreKhz() / (TUint32)l3 : 0;
            }
        TUint32 got = v ? base / (TUint32)v : 0;

        // Штатна частота цього ж домену - точка відліку для налаштування.
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
    // Тести блокують цикл подій на секунди. Без цього рядка екран просто
    // завмирає, і виглядає це так, ніби кнопка не спрацювала.
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

// Комбінований прогін: усі підсистеми поспіль, кілька кіл, із підсумком по
// кожній. Саме він відповідає на питання "де слабка ланка": під розгоном
// першою почне давати err>0 та підсистема, яка не витримує.
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

void COcView::Activate(TRowId aRow)
    {
    if (aRow == ERowHelp)
        {
        iHelp = ETrue;
        iHelpTop = 0;
        Repaint();
        return;
        }
    if (!iEngine || !iEngine->IsReady())
        {
        if (aRow >= ERowTestAlu && aRow != ERowHelp && aRow != ERowTestProfile)
            {
            RunTest(aRow);          // тести працюють і без патчера
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

        case ERowTestProfile:
            {
            if (iKnownDevice)
                break;
            const TOcProfile* tp = TestProfile(iTestProfile);
            if (!tp)
                break;
            TBuf<48> nm;
            nm.Copy(TPtrC8((const TUint8*)tp->iId));
            AddLog(OcStr(EStrTestProfileWarn1));
            AddLog(OcStr(EStrTestProfileWarn2));
            AddLogF(OcStr(EStrTestProfileTrying), &nm);
            Repaint();
            iEngine->SetProfile(*tp);
            TInt pr = iEngine->Prepare();
            if (pr == KErrNone)
                {
                AddLog(OcStr(EStrTestProfileOk));
                if (iEngine->FoundGlobal())
                    AddLogF(OcStr(EStrTestProfileFoundGlobal), iEngine->FoundGlobal());
                iMult = iEngine->BootMult();
                iArmedMult = iMult;
                iCoreReg = iEngine->StockCore();
                iArmedCore = iCoreReg;
                iMpuDiv = (TInt)(iEngine->BootMpu() & KMpuDivMask);
                iArmedMpu = iEngine->StockMpu();
                iDspDiv = iEngine->StockDspDiv();
                iLiveL3 = (TInt)(iEngine->BootCore() & KCoreL3Mask);
                }
            else
                {
                iStatus.Format(_L("NOT READY (%d) at: %S"), pr,
                               &COcEngine::StepName(iEngine->FailedAt()));
                AddLog(iStatus);
                }
            break;
            }

        case ERowBusGuard:
            iBusGuard = !iBusGuard;
            iEngine->SetBusGuard(iBusGuard);
            iArmed = EFalse;
            if (iBusGuard)
                AddLog(OcStr(EStrGuardOn));
            else
                {
                AddLog(OcStr(EStrGuardOff));
                AddLog(OcStr(EStrGuardNote));
                }
            break;

        case ERowL3:
            // OK нічого не застосовує - лише знімає ручне значення й повертає
            // поле на автопідбір. Застосування, як і в решті полів CLKSEL1_CORE,
            // іде тільки через APPLY + CONFIRM: цей дільник міняється в тому
            // самому записі, що й множник, і окремого швидкого шляху, як у DSP,
            // для нього немає.
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
            // Застосовується одразу, без arm/confirm. Причина: зміна ізольована
            // в домені DSP - ні CORE, ні SDRAM, ні дисплей не зачіпаються, тож
            // немає вікна, в якому апарат міг би зависнути «на льоту», як при
            // relock DPLL. Ребут повертає штатний дільник у будь-якому разі.
            if (iDspDiv == iEngine->DspDiv())
                {
                AddLog(OcStr(EStrDspAlreadySet));
                break;
                }
            TInt rc = KErrGeneral;
            TInt r = iEngine->ApplyDspDiv(iDspDiv, rc);
            if (r == KErrNotSupported)
                {
                // Профіль без адреси сеттера. Кажемо чому, бо інакше це
                // виглядає як поламаний пункт меню.
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
            // Читаємо назад: своєму запису не віримо - правило проєкту.
            TInt live = 0;
            iEngine->LiveDspDiv(live);
            AddLogF(_L("dsp: /%d applied, live /%d"), iDspDiv, live);
            if (live != iDspDiv)
                {
                AddLog(OcStr(EStrDspMismatch));
                break;
                }
            // Повернення до штатного - без сторожа: воно безпечне й остаточне.
            if (!iEngine->IsDspChanged())
                {
                CancelDspRevert();
                break;
                }
            // Нештатний дільник відпускаємо лише під сторожа. Лишити його
            // назавжди не можна: виміряно два ребути з двох, причому один - у
            // повному простої, без жодного навантаження на DSP (findings §24.5).
            CancelDspRevert();
            TRAPD(terr, iDspTimer = CPeriodic::NewL(CActive::EPriorityStandard));
            if (terr != KErrNone || !iDspTimer)
                {
                // Немає сторожа - немає й права тримати нештатний дільник.
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
                // Не «може бути», а виміряно: 330 МГц валить апарат, щойно на
                // C55x щось запускається - аудіо, камера, будь-що.
                AddLog(OcStr(EStrWarnDspAbove));
                AddLog(OcStr(EStrWarnDspUseMult));
                }
            break;
            }

        case ERowApply:
            // Лише озброює. Частоту НЕ чіпає: це та сама "окрема опція
            // застосування", після якої потрібне окреме підтвердження.
            iArmedMult = iMult;
            iArmedCore = EffectiveCore();
            iArmedMpu = (iEngine->StockMpu() & ~KMpuDivMask) |
                        ((TUint32)iMpuDiv & KMpuDivMask);
            iArmed = ETrue;
            {
            TOcClocks c;
            iEngine->ClocksFor(iArmedMult, c);
            c.iMpuDiv = iMpuDiv;
            TInt l3 = CoreFieldValue(iArmedCore, ECoreL3);
            if (iBusGuard && iLiveL3 && l3 < iLiveL3)
                {
                AddLogF(OcStr(EStrBusStaysClamped), iLiveL3);
                l3 = iLiveL3;
                }
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
            // Дати вікну намалюватись і на мить затихнути перед переходом.
            User::After(150000);
            TInt r = iEngine->ApplyRampWithCore(iArmedMult, iArmedCore,
                                                iArmedMpu, st);
            iStatus.Zero();
            {
            // Чи ВЗЯВСЯ запис дільників. Апаратура відкидає весь запис у
            // CM_CLKSEL1_CORE, якщо хоч одне поле недійсне, і мовчки; доти
            // застосунок звітував про намір, а не про результат (findings 38).
            // Порівнюємо з тим, що двигун НАСПРАВДІ віддав у залізо: поле l3 він
            // затискає живим значенням, бо підйом шини смертельний (findings 38.2).
            const TUint32 wroteCore = iEngine->LastCoreWritten();
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
            if (r != KErrNone)
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
                // Блоб уже сам відкатився на попереднє значення всередині
                // критичної секції, тож тут лишається тільки повідомити.
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
            // Шину назад не піднімаємо: чотири смерті з чотирьох на двох апаратах
            // (findings 38.2). Знижена шина безпечна, ребут повертає штатну.
            if (iEngine->RestoreNeedsReboot())
                AddLog(OcStr(EStrBusLeftLowered));
            {
            TUint32 lc = 0;
            if (iEngine->LiveCore(lc) == KErrNone)
                iLiveL3 = (TInt)(lc & KCoreL3Mask);
            }
            // Дільник DSP - у тому ж «повернути все як було»: інакше довелося б
            // памʼятати, що RESTORE STOCK стосується не всього.
            RestoreDspNow(OcStr(EStrReasonRestore));
            iMult = iEngine->StockMult();
            iCoreReg = iEngine->StockCore();
            iMpuDiv = iEngine->StockClocks().iMpuDiv;
            iL3Div = 0;                       // разом з усім іншим - на автопідбір
            iDspDiv = iEngine->StockDspDiv();
            iArmed = EFalse;
            break;
            }

        default:
            if (aRow >= ERowDivFirst && aRow <= ERowDivLast)
                {
                // На рядку дільника OK нічого не застосовує - значення
                // крутиться вліво/вправо. Так менше шансів зачепити випадково.
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
        const TInt total = OcHelpLineCount();
        if (aKeyEvent.iCode == EKeyDownArrow)
            {
            if (iHelpTop < total - 1)
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
                    // В авто-режимі дільники перераховуються самі; у ручному
                    // лишаються як є - у цьому й сенс ручного.
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
                // Крутимо від ТОГО, що зараз показано, а не від збереженого:
                // поки дільник на автопідборі, показане значення прийшло від
                // CompensatedCore, і крок мусить іти саме від нього.
                const TInt cur = CoreFieldValue(EffectiveCore(), ECoreL3);
                const TInt nv = L3DivStep(cur, dir);
                if (nv != cur)
                    {
                    iL3Div = nv;
                    // Дзеркалимо в ручний регістр, щоб рядок L3 у списку нижче
                    // показував те саме: два контроли на одне поле, які
                    // розійшлись би, - це готова пастка.
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
            else if (iRow == ERowBusGuard)
                {
                iBusGuard = !iBusGuard;
                iEngine->SetBusGuard(iBusGuard);
                iArmed = EFalse;
                if (!iBusGuard)
                    AddLog(OcStr(EStrGuardOff));
                }
            else if (iRow == ERowDsp)
                {
                // Крутиться вільно, у залізо нічого не йде: застосування - лише
                // по OK на цьому ж рядку. iArmed не чіпаємо - шлях DSP окремий
                // від arm/confirm для частоти ядра.
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
                        // Те саме поле, що й рядок BUSDIV нагорі: тримаємо їх
                        // в одному значенні, інакше верхній рядок мовчки
                        // перекрив би щойно введене тут.
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
            // Цифри 1..9 на рядку MULT - швидкий стрибок на кратні 5% від штатного.
            if (iRow == ERowSlider && aKeyEvent.iCode >= '0' && aKeyEvent.iCode <= '9')
                {
                TInt stock = iEngine ? iEngine->StockMult() : 86;
                TInt pct = (aKeyEvent.iCode - '0') * 5;
                TInt m = stock + (stock * pct) / 100;
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

// Довідка на весь екран. Тим самим дрібним шрифтом, що й решта: рядки в
// src/strings.json уже нарізані під цю ширину, тож переносу тут немає.
void COcView::DrawHelp(CWindowGc& aGc, const TRect& aRect) const
    {
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    aGc.UseFont(font);
    aGc.SetPenColor(KRgbBlack);
    const TInt lh = font->HeightInPixels() + 1;
    const TInt x = aRect.iTl.iX + 2;
    TInt y = aRect.iTl.iY + lh;
    aGc.DrawText(OcStr(EStrHelpTitle), TPoint(x, y));
    y += lh * 2;
    const TInt total = OcHelpLineCount();
    for (TInt i = iHelpTop; i < total && y <= aRect.iBr.iY - 2; ++i)
        {
        aGc.DrawText(OcHelpLine(i), TPoint(x, y));
        y += lh;
        }
    aGc.DiscardFont();
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

    // Дрібний шрифт і явне обрізання: з NormalFont вміст не влазив, і рядки
    // логу малювались за краєм екрана (виглядало як "тести не працюють").
    const CFont* font = CEikonEnv::Static()->AnnotationFont();
    gc.UseFont(font);
    const TInt lh = font->HeightInPixels() + 1;
    const TInt x = r.iTl.iX + 2;
    const TInt yMax = r.iBr.iY - lh;
    TInt y = r.iTl.iY + lh;
    TBuf<96> line;
    gc.SetPenColor(KRgbBlack);

    const TBool ready = (iEngine && iEngine->IsReady());

    // --- шапка: штатний стан і те, що вийде після застосування ---
    if (ready)
        {
        const TOcClocks& st = iEngine->StockClocks();
        line.Format(_L("stock m%d CPU %u BUS %u DSS %u"), st.iMult,
                    st.MpuKhz() / 1000, st.L3Khz() / 1000, st.Dss1Khz() / 1000);
        gc.DrawText(line, TPoint(x, y));
        y += lh;

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
        gc.DrawText(line, TPoint(x, y));
        y += lh;
        line.Format(_L("CLKSEL1_CORE %08x"), core);
        gc.DrawText(line, TPoint(x, y));
        y += lh;
        }
    else
        {
        gc.DrawText(OcStr(EStrNotReady), TPoint(x, y));
        y += lh;
        if (iStatus.Length())
            {
            gc.DrawText(iStatus, TPoint(x, y));
            y += lh;
            }
        y += lh;
        }

    // --- прокрутний список ---
    const TInt vis = VisibleRows();
    TInt last = iTop + vis;
    if (last > ERowCount)
        last = ERowCount;
    for (TInt row = iTop; row < last && y < yMax; ++row)
        {
        TBuf<96> label;
        RowLabel(row, label);
        line.Format(_L("%c%S"), (iRow == row) ? '>' : ' ', &label);
        gc.DrawText(line, TPoint(x, y));
        y += lh;
        }
    // Позначка, що список довший за екран.
    if (last < ERowCount && y < yMax)
        {
        line.Format(OcStr(EStrMoreRows), ERowCount - last);
        gc.DrawText(line, TPoint(x, y));
        y += lh;
        }

    if (ready && iStatus.Length() && y < yMax)
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

// --- каркас застосунку ------------------------------------------------------

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
    // ENoScreenFurniture прибирає і статусну стрічку, і панель софт-клавіш, тож
    // застосунок займає весь екран в обох орієнтаціях. Разом із ним rect треба
    // брати з ApplicationRect(), а не ClientRect(): останній повертає область
    // ПІД furniture, якого тепер немає.
    BaseConstructL(CAknAppUi::ENoAppResourceFile | CAknAppUi::EAknEnableSkin |
                   CAknAppUi::ENoScreenFurniture);
    iView = COcView::NewL(ApplicationRect());
    AddToStackL(iView);
    }

// Поворот екрана міняє геометрію, і без цього застосунок лишався б із
// вертикальним прямокутником у ландшафтному режимі - саме тому результати
// читались нормально лише в одній орієнтації.
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
        // Порядок важливий: спершу знімаємо розгін, потім руйнуємо контрол.
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
