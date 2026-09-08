// SymbianOverclock: розгін і стрес-тести прямо на апараті.
//
// Керування (джойстик):
//   вгору/вниз    - вибір рядка
//   вліво/вправо  - на рядку MULT рухає повзунок (крок 1; з утриманням - швидше)
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

#include "ocengine.h"

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

// --- контрол ----------------------------------------------------------------

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
    TInt   iMult;            // значення повзунка
    TBool  iArmed;           // "APPLY SETTING" натиснуто, чекаємо підтвердження
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

void COcView::RunTest(TRowId aRow)
    {
    // Тести блокують цикл подій на секунди. Без цього рядка екран просто
    // завмирає, і виглядає це так, ніби кнопка не спрацювала.
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

// Комбінований прогін: усі підсистеми поспіль, кілька кіл, із підсумком по
// кожній. Саме він відповідає на питання "де слабка ланка": під розгоном
// першою почне давати err>0 та підсистема, яка не витримує.
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
            {                                   // тести працюють і без патчера
            RunTest(aRow);
            return;
            }
        AddLog(_L("engine not ready - overclock disabled"));
        return;
        }

    switch (aRow)
        {
        case ERowApply:
            // Лише озброює. Частоту НЕ чіпає: це та сама "окрема опція
            // застосування", після якої ще потрібне окреме підтвердження.
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
            // Дати вікну намалюватись і на мить затихнути: контролер дисплея
            // безперервно читає кадровий буфер із SDRAM, а саме її ми на час
            // переходу і забираємо. Менше трафіку в цю мить - менше артефактів.
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
                // Таймаут у relock означає, що DPLL лишився в невизначеному
                // стані. Тягнути з цим до наступного натискання не можна -
                // повертаємось на штатний множник самі.
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
        // EKeyDevice3 (натискання джойстика на S60) тут дорівнює EKeyOK -
        // окремої гілки не треба, компілятор ловить це як дубль.
        case EKeyOK:
        case EKeyEnter:
            Activate((TRowId)iRow);
            break;
        case EKeyEscape:
        case EKeyDevice1:
            // CCoeAppUi не має Exit(); вихід іде штатною командою оболонки.
            CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
            return EKeyWasConsumed;
        default:
            // Цифри 1..9 - швидкий стрибок повзунка на кратні кроки від штатного.
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

    // ДРІБНИЙ шрифт навмисно. З NormalFont повний вміст (заголовок, повзунок,
    // 8 рядків-кнопок, статус і лог) виходив приблизно на 360 px при доступних
    // ~268 - рядки логу малювались за нижньою межею екрана, і виглядало це так,
    // ніби тести нічого не роблять. Тепер ще й явно обрізаємо все, що не влазить.
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

    // --- повзунок ---
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

    // Смуга: ліворуч мінімум, праворуч максимум, сіра позначка - штатний множник.
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

    // --- рядки-кнопки ---
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
