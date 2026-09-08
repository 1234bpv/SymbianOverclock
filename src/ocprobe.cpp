// Пробник DVFS для Symbian 9.2 / OMAP 2420 (еталон - Nokia N95 8GB).
//
// Нічого не змінює. Три задачі:
//   1. зняти те, що система сама повідомляє про частоту (HAL);
//   2. виміряти РЕАЛЬНУ швидкодію ядра і пам'яті - незалежний прилад, яким
//      потім доводитимемо, що зміна частоти справжня, а не намальована;
//   3. з'ясувати, чи видно kernel-side драйвер DVFS із користувацького боку.
//
// Пункт 2 тут ключовий: усі відомі "розгони" Symbian провалилися саме на
// тому, що міряли напис, а не роботу.
//
// Рядки логу навмисно ASCII: GCCE 3.4.3 і 8-бітні дескриптори не варті ризику
// з кирилицею в літералах.

#include <e32base.h>
#include <e32std.h>
#include <e32cmn.h>
#include <f32file.h>
#include <hal.h>

#include "ocengine.h"

_LIT(KLogFile, "E:\\ocprobe.log");
_LIT(KLddFile, "dvfs_interface.ldd");
// Зареєстроване ім'я взято з переліку TFindLogicalDevice на живому апараті.
// Рядок "DVFSInterfaceDriver" усередині .ldd - це назва фабрики, не каналу;
// спроба відкритись по ній дає KErrNotFound.
_LIT(KLddName, "dvfs_interface");
_LIT(KLddNameAlt, "DVFSInterfaceDriver");

// Тестовий драйвер силового API. Лежить у Z:\sys\bin, штатно не завантажений.
_LIT(KPwrFile, "d_powerapi.ldd");
_LIT(KPwrName, "TestPowerAPI");

// RomPatcher+ драйвер. У ROM його НЕМАЄ - демон заливає file0 у E:\sys\bin
// перед прогоном. Iм'я каналу - з рядка всередині .ldd, звірено з диспетчером
// DoControl (func 100..105, findings 14). func 105 = одиничний запис:
//   DoControl(105, &addr, &desc) -> memmove(dest=addr, src=desc.Ptr(), len=desc.Length())
_LIT(KPatcherFile, "patcher.ldd");
_LIT(KPatcherName, "RomPatcherPlus.Driver");

static RFs gFs;
static RFile gLog;
static TUint32 gFcFreq = 0;

static void Log(const TDesC8 &aLine)
    {
    gLog.Write(aLine);
    gLog.Write(_L8("\r\n"));
    gLog.Flush();
    }

static void LogF(TRefByValue<const TDesC8> aFmt, ...)
    {
    VA_LIST list;
    VA_START(list, aFmt);
    TBuf8<512> buf;
    buf.AppendFormatList(aFmt, list);
    VA_END(list);
    Log(buf);
    }

// --- прилад -----------------------------------------------------------------

// Цикл із залежністю по даних: кожна ітерація чекає результату попередньої,
// тож час масштабується з тактовою частотою ядра, а не з шириною конвеєра
// і не з пам'яттю (робочий набір - один регістр, влазить у будь-який кеш).
static TUint32 SpinAlu(TUint32 aIter)
    {
    TUint32 x = 0x12345678u;
    for (TUint32 i = 0; i < aIter; ++i)
        {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
        }
    return x;
    }

// Обхід буфера, більшого за кеш: впирається в L3/SDRAM. На OMAP2 частота ядра
// і частота шини змінюються разом, тож цей замір відрізнить одне від одного.
static TUint32 SpinMem(TUint32 *aBuf, TInt aWords, TInt aPasses)
    {
    TUint32 acc = 0;
    for (TInt p = 0; p < aPasses; ++p)
        for (TInt i = 0; i < aWords; i += 16)   // крок = рядок кешу (64 Б)
            acc += aBuf[i];
    return acc;
    }

// АБСОЛЮТНИЙ замір частоти. 256 незалежних додавань поспіль: ARM1136 -
// одноконвеєрний, кожне таке додавання коштує рівно такт, залежностей між ними
// немає, промахів I-cache теж (1 КБ коду при 16 КБ кешу). Тож такт ділиться на
// час напряму - на відміну від циклу з множенням, де тактів на ітерацію ми
// точно не знаємо і 206 від 332 МГц не відрізнимо.
const TUint32 KNopCyclesPerIter = 258;   // 256 add + subs + bne

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

// Мікросекунди між двома відліками швидкого лічильника.
static TUint32 ElapsedUs(TUint32 aStart, TUint32 aEnd)
    {
    TUint32 d = aEnd - aStart;      // беззнакове віднімання переживає обгортання
    if (gFcFreq == 0)
        return 0;
    return (TUint32)(((TUint64)d * 1000000u) / gFcFreq);
    }

// --- канал драйвера ---------------------------------------------------------

// Розкладка взята з дизасемблера dvfs_interface.ldd (HandleMsg @0x80148698):
//   iValue == 0x80000000 -> закриття каналу
//   iValue <  0          -> DoRequest, номер = ~iValue
//   iValue >= 0          -> DoControl
// DoRequest(0, status, a1, a2) веде в SetPerformance @0x80148460, де рішення
// ухвалюється ВИКЛЮЧНО за a2: cmp r2,#50, більше - резервувати, менше або
// дорівнює - звільнити. a1 не читається. Рівень зашитий константою (mov r0,#66),
// тож довільної частоти цей інтерфейс не приймає.
class RDvfs : public RBusLogicalChannel
    {
public:
    TInt Open(const TDesC &aName, const TVersion &aVer)
        {
        return DoCreate(aName, aVer, KNullUnit, NULL, NULL, EOwnerProcess);
        }
    // DoControl(1, ...) @0x80148410 робить GetClockFrequency(MPU) і ділить на
    // 3300000, тобто віддає відсоток від номіналу. Решта опкодів повертають 0.
    TInt PerfPercent() { return DoControl(1, (TAny *)0); }
    // DoControl захищений у RBusLogicalChannel, тож відкриваємо його явно.
    TInt Ctl(TInt aFunc) { return DoControl(aFunc, (TAny *)0); }
    // Після READ-патчу DoControl(1) повертає r0 з ldr [a1]. TInt тут лише
    // транспорт: викликач одразу приводить результат до TUint32.
    TInt ReadRaw(TUint32 aAddress) { return DoControl(1, (TAny *)aAddress); }
    // Після EXEC-патчу те саме DoControl(1) стає "bx r1", тобто викликом коду
    // за адресою a1 у режимі ядра. Окрема назва - щоб на місці виклику було
    // видно, який саме патч зараз накладено.
    TInt Call(TUint32 aAddress) { return DoControl(1, (TAny *)aAddress); }
    // Драйвер завершує запит через Kern::RequestComplete, тож чекаємо на статус.
    TInt SetPerformance(TInt aValue)
        {
        TRequestStatus s;
        DoRequest(0, s, (TAny *)0, (TAny *)aValue);
        User::WaitForRequest(s);
        return s.Int();
        }
    };

// Розкладка з дизасемблера d_powerapi.ldd (HandleMsg @0x80148FB8):
//   r0 = ~iValue           -> номер запиту, тобто піддія (0 = зайняти,
//                             1 = прочитати стан, 3 = перелічити)
//   r1 = msg[32] = a1      -> індекс у таблиці переходів, 0..12
//   результат повертається через Kern::RequestComplete у TRequestStatus
// Індекси: 1..6 -> ресурси 1..5, 6 -> 65, 7 -> 66 (продуктивність CPU),
//          8 -> 67, 9 -> 69, 10 -> 70.
// Статичний, а не на стеку: якщо запит не завершиться і ми підемо далі, драйвер
// колись напише за цією адресою. Стекова змінна на той час уже чужа пам'ять.
static TRequestStatus gPwrStatus;

class RPwr : public RBusLogicalChannel
    {
public:
    TInt Open(const TVersion &aVer)
        {
        return DoCreate(KPwrName, aVer, KNullUnit, NULL, NULL, EOwnerProcess);
        }
    // Повертає KErrTimedOut, якщо драйвер не відповів за aMs. Скасувати запит
    // нічим - Cancel драйвер не має, - тож після таймауту викликач мусить
    // припинити роботу з каналом, а не слати наступний запит.
    TInt Req(TInt aSubOp, TInt aIndex, TInt aMs)
        {
        gPwrStatus = KRequestPending;
        DoRequest(aSubOp, gPwrStatus, (TAny *)aIndex, (TAny *)0);

        RTimer timer;
        if (timer.CreateLocal() != KErrNone)
            {                                   // без таймера краще не ризикувати
            return KErrGeneral;
            }
        TRequestStatus ts;
        timer.After(ts, aMs * 1000);
        User::WaitForRequest(gPwrStatus, ts);

        if (gPwrStatus == KRequestPending)
            {
            timer.Close();
            return KErrTimedOut;
            }
        timer.Cancel();
        User::WaitForRequest(ts);
        timer.Close();
        return gPwrStatus.Int();
        }
    };

// Лише читання: піддія 1 по ОДНОМУ індексу за прогін.
//
// Перебір усіх індексів поспіль обійшовся зависанням: index=6 (ресурс 65) не
// завершує запит ніколи. Скасувати його нічим - Cancel драйвер не має, - тож
// єдиний надійний спосіб іти далі це новий процес на кожен індекс: висячий
// запит помирає разом із процесом, а не тягнеться в наступну спробу.
static void ProbePowerApi(TInt aIndex)
    {
    LogF(_L8("--- TestPowerAPI: index %d (read-only) ---"), aIndex);
    TInt r = User::LoadLogicalDevice(KPwrFile);
    LogF(_L8("LoadLogicalDevice(d_powerapi.ldd) -> %d   [0=ok, -11=already loaded]"), r);
    if (r != KErrNone && r != KErrAlreadyExists)
        {
        Log(_L8("  driver not loadable - skipped"));
        return;
        }

    RPwr ch;
    TInt o = ch.Open(TVersion(0,0,0));
    LogF(_L8("DoCreate(TestPowerAPI, v0.0.0) -> %d"), o);
    if (o != KErrNone)
        return;

    TInt v = ch.Req(1, aIndex, 3000);
    LogF(_L8("  get(index=%d) -> %d"), aIndex, v);
    if (v == KErrTimedOut)
        {
        Log(_L8("  request never completed - channel left open on purpose"));
        return;                         // канал НЕ закриваємо: запит ще висить
        }

    ch.Close();
    Log(_L8("channel closed"));
    }

// --- розгін через спільний двигун -------------------------------------------
//
// Уся делікатна частина - READ/EXEC-патчі, ланцюг до SRAM-копії і блоб relock -
// живе в ocengine.cpp і використовується спільно із застосунком ocgui. Двох
// копій цієї послідовності бути не може: розійшовшись, вони вішають апарат.
// Тут лишається тільки сценарій заміру.

static TUint32 MeasureMhz();          // визначено нижче, у секції приладу

// Одиночний замір шумить на кілька відсотків: заважають переривання й чужі
// потоки. Але завада може тільки СПОВІЛЬНИТИ прогін, ніколи не пришвидшити,
// тож найкращий із серії - найчистіший, а не "везіння".
static TUint32 BestMhz(TInt aRuns)
    {
    TUint32 best = 0;
    for (TInt i = 0; i < aRuns; ++i)
        {
        TUint32 v = MeasureMhz();
        if (v > best)
            best = v;
        }
    return best;
    }

static TUint32 BestAlu(TInt aRuns, TUint32 aIter)
    {
    TUint32 best = 0;
    for (TInt i = 0; i < aRuns; ++i)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinAlu(aIter);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 m100 = us ? (TUint32)(((TUint64)aIter * 100u) / us) : 0;
        if (sink == 0)
            m100 ^= 1;
        if (m100 > best)
            best = m100;
        }
    return best;
    }

static TUint32 BestMem(TInt aRuns)
    {
    const TInt kWords = 512 * 1024;               // 2 МБ - явно повз кеш
    TUint32 *buf = (TUint32 *)User::Alloc(kWords * sizeof(TUint32));
    if (!buf)
        return 0;
    TInt i;
    for (i = 0; i < kWords; ++i)
        buf[i] = (TUint32)i;
    TUint32 best = 0;
    for (TInt r = 0; r < aRuns; ++r)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinMem(buf, kWords, 8);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 lines = (TUint32)(kWords / 16) * 8u;
        TUint32 mbs = us ? (TUint32)(((TUint64)lines * 64u) / us) : 0;
        if (sink == 0)
            mbs ^= 1;
        if (mbs > best)
            best = mbs;
        }
    User::Free(buf);
    return best;
    }

static TInt MemoryIntegrity(TInt aWords)
    {
    TUint32 *buf = (TUint32 *)User::Alloc(aWords * sizeof(TUint32));
    if (!buf)
        return KErrNoMemory;
    TUint32 x = 0x2545f491u;
    TInt i;
    for (i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        buf[i] = x;
        }
    x = 0x2545f491u;
    TInt bad = 0;
    for (i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        if (buf[i] != x)
            ++bad;
        }
    User::Free(buf);
    return bad;
    }

static void LogClocks(const TDesC8 &aTag, const TOcClocks &aC)
    {
    LogF(_L8("  %S: m=%d DPLL=%u CORE=%u MPU=%u BUS=%u DSS1=%u kHz (l3div=%d dssdiv=%d)"),
         &aTag, aC.iMult, aC.DpllKhz(), aC.CoreKhz(), aC.MpuKhz(), aC.L3Khz(),
         aC.Dss1Khz(), aC.iL3Div, aC.iDss1Div);
    }

// Показує, ЩО саме буде записано в CM_CLKSEL1_CORE і які частоти з цього
// вийдуть. Головне тут - DSS1: його зсув і ламав відрисовку (findings §21).
static void LogCompensation(COcEngine *aE, TInt aMult)
    {
    TUint32 newCore = aE->CompensatedCore(aMult);
    TOcClocks c;
    aE->ClocksFor(aMult, c);
    // Перерахувати з НОВИМИ дільниками.
    TOcClocks comp = c;
    comp.iL3Div = (TInt)(newCore & KCoreL3Mask);
    comp.iDss1Div = (TInt)((newCore & KCoreDss1Mask) >> KCoreDss1Shift);
    LogF(_L8("  CLKSEL1_CORE %08x -> %08x"), aE->StockCore(), newCore);
    LogClocks(_L8("compensated"), comp);
    const TOcClocks &st = aE->StockClocks();
    TInt dp = st.Dss1Khz() ? (TInt)(((TInt64)comp.Dss1Khz() - st.Dss1Khz()) * 1000
                                    / st.Dss1Khz()) : 0;
    TInt lp = st.L3Khz() ? (TInt)(((TInt64)comp.L3Khz() - st.L3Khz()) * 1000
                                  / st.L3Khz()) : 0;
    LogF(_L8("  vs stock: DSS1 %d.%d%%  BUS %d.%d%%  (0 = display untouched)"),
         dp / 10, (dp < 0 ? -dp : dp) % 10, lp / 10, (lp < 0 ? -lp : lp) % 10);
    }

// Знімок усіх чотирьох регістрів розкладки за ОДНЕ накладання READ-патчу:
// інакше поля знімались би в різні моменти, а губернатор рухає їх у рантаймі.
struct TClockSnap
    {
    TUint32 iSel1;
    TUint32 iSel2;
    TUint32 iMpu;
    TUint32 iCore;
    TUint32 iRfr;      // SDRC_RFR_CTRL0
    };

// SDRC_RFR_CTRL0 знімається разом із рештою навмисно. Його поле ARCV рахує
// такти L3, тож при незмінному ARCV повільніша шина розтягує інтервал
// авторефрешу в реальному часі (§26.1a). Питання, чи рухає прошивка ARCV при
// переході OPP, лишалось відкритим саме тому, що ми читали цей регістр лише
// між прогонами, а не в момент, коли губернатор стоїть на нижчій OPP. Тепер
// він у тому ж знімку, що й CM_CLKSEL2_PLL, тобто відповідь буде видно прямо.
static TInt SnapClocks(COcEngine *aE, TClockSnap &aOut)
    {
    const TUint32 regs[5] =
        { KRegClkSel1Pll, KRegClkSel2Pll, KRegClkSelMpu, KRegClkSel1Core,
          KRegSdrcRfr0 };
    TUint32 v[5] = { 0, 0, 0, 0, 0 };
    TInt r = aE->ReadRegs(regs, v, 5);
    aOut.iSel1 = v[0];
    aOut.iSel2 = v[1];
    aOut.iMpu  = v[2];
    aOut.iCore = v[3];
    aOut.iRfr  = v[4];
    return r;
    }

// Дві частоти з цього знімка - MPU і L3 - і є те, заради чого все затіяно, тож
// друкуємо і сирі слова, і розшифровку. CORE тут рахується з МИТТЄВОГО
// CM_CLKSEL2_PLL, а він ходить між 1 і 2 під губернатором (findings §12, §18):
// відлік із sel2=1 означає лише, що знімок упіймав нижчу OPP, а не що розкладку
// зняли.
static void LogSnap(const TDesC8 &aTag, const TClockSnap &aS)
    {
    TOcClocks c;
    c.Decode(aS.iSel1, aS.iSel2, aS.iMpu, aS.iCore);
    LogF(_L8("  %S: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x RFR=%08x"),
         &aTag, aS.iSel1, aS.iSel2, aS.iMpu, aS.iCore, aS.iRfr);
    // Інтервал авторефрешу в наносекундах: ARCV тактів шини. Друкуємо саме
    // час, а не сире поле: питання стоїть про те, чи вкладаємось у вікно
    // утримання заряду, а не про вміст регістра.
    const TUint32 arcv = (aS.iRfr >> 8) & 0xffffu;
    const TUint32 busKhz = c.L3Khz();
    const TUint32 refrNs = busKhz ? (TUint32)(((TUint64)arcv * 1000000u) / busKhz) : 0;
    LogF(_L8("  %S: m=%d mpudiv=%d l3div=%d dssdiv=%d sel2=%d -> CORE=%u MPU=%u BUS=%u DSS1=%u kHz"),
         &aTag, c.iMult, c.iMpuDiv, c.iL3Div, c.iDss1Div, c.iCoreSel,
         c.CoreKhz(), c.MpuKhz(), c.L3Khz(), c.Dss1Khz());
    LogF(_L8("  %S: refresh arcv=%u -> %u ns (stock 7520 ns; longer = charge loss risk)"),
         &aTag, arcv, refrNs);
    }

// Розкладка тримається, поки не зсунулись ані множник, ані обидва дільники.
// CM_CLKSEL2_PLL свідомо НЕ порівнюємо: його рух - штатна робота губернатора, а
// не втрата нашої конфігурації.
static TBool SameLayout(const TClockSnap &aA, const TClockSnap &aB)
    {
    return ((aA.iSel1 >> 12) & 0x3ff) == ((aB.iSel1 >> 12) & 0x3ff) &&
           (aA.iMpu & KMpuDivMask) == (aB.iMpu & KMpuDivMask) &&
           (aA.iCore & (KCoreL3Mask | KCoreDss1Mask)) ==
           (aB.iCore & (KCoreL3Mask | KCoreDss1Mask));
    }

// Обраний профіль апарата. Вмикається в E32Main за наявності слова "e90" в
// команді. За замовчуванням RM-320. Двигун бере адреси з профілю, тож помилковий
// профіль на чужому апараті - це не просто хибні числа, а патч у чуже ядро; тому
// вибір явний і єдиний.
static TBool gUseE90Profile = EFalse;

static COcEngine *PrepareEngine()
    {
    COcEngine *e = NULL;
    TRAPD(err, e = COcEngine::NewL());
    if (err != KErrNone || !e)
        {
        LogF(_L8("engine alloc failed: %d"), err);
        return NULL;
        }
    if (gUseE90Profile)
        {
        e->SetProfile(KProfileE90);
        LogF(_L8("profile: %s (EReadMeasureOnly, no dvfs)"), e->Profile().iId);
        }
    TInt r = e->Prepare();
    if (r != KErrNone)
        {
        TBuf8<64> step;
        step.Copy(COcEngine::StepName(e->FailedAt()));
        LogF(_L8("engine NOT ready: err %d at step [%S]"), r, &step);
        delete e;
        return NULL;
        }
    LogF(_L8("engine ready: SRAM @%08x, stock mult %d"),
         e->SramAddress(), e->StockMult());
    LogClocks(_L8("stock"), e->StockClocks());
    // Опорна точка стала (KStockClkSel1Core), тож розбіжність із нею мусить
    // бути видно з логу відразу. Раніше цей випадок мовчки ставав новою нормою:
    // цикл "підняти - опустити" щоразу зсував dssdiv на крок і звітував при
    // цьому, що дисплей не зачеплено.
    if (e->BootedNonStock())
        LogF(_L8("  NOT STOCK AT START: m=%d CLKSEL1_CORE=%08x CLKSEL_MPU=%08x"
                 "  (stock: m=%d %08x %08x)"),
              e->BootMult(), e->BootCore(), e->BootMpu(),
              KStockMult, KStockClkSel1Core, KStockClkSelMpu);
    return e;
    }

// Безголова перевірка двигуна: та сама послідовність, що її виконує застосунок,
// але без інтерфейсу. Нічого не змінює.
static void ProbeEngine()
    {
    Log(_L8("--- engine self-check (read only) ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;
    const TUint32 regs[] =
        { KRegClkSel1Pll, KRegClkSel2Pll, KRegClkSelMpu, KRegClkEnPll,
          KRegIdlestPll, KRegClkSel1Core, KRegSdrcRfr0, KRegSdrcActimA };
    const char *names[] =
        { "CM_CLKSEL1_PLL ", "CM_CLKSEL2_PLL ", "CM_CLKSEL_MPU  ",
          "CM_CLKEN_PLL   ", "CM_IDLEST_PLL  ", "CM_CLKSEL1_CORE",
          "SDRC_RFR_CTRL0 ", "SDRC_ACTIM_A_0 " };
    for (TInt i = 0; i < 8; ++i)
        {
        TUint32 v = 0;
        e->ReadReg(regs[i], v);
        TPtrC8 nm((const TUint8 *)names[i]);
        LogF(_L8("  %S @%08x = %08x"), &nm, regs[i], v);
        }
    // Найважливіший рядок цього прогону: чи знято патч. Ненульовий відсоток
    // означає, що в ядрі знову штатний код і після виходу процесу міни не
    // лишається. Перезапису байтів для цього не досить - потрібен unshadow.
    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);
    delete e;
    Log(_L8("engine self-check done"));
    }

// Тестовий блоб ARM-коду для виконання в режимі ядра.
// Складається з двох інструкцій і константи в літерному пулі:
//   0x00: ldr r0, [pc, #0]   ; вантажить константу за адресою PC+8 = 0x08 (0xe59f0000)
//   0x04: bx  lr             ; повернення (0xe12fff1e)
//   0x08: .word 0x5a5a0001   ; константа (0x5a5a0001)
// На ARM1136 (ARMv6) інструкції movw/movt відсутні, тому константа береться з пулу.
static void ProbeBlobTest()
    {
    Log(_L8("--- custom kernel blob test ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    static const TUint8 kTestBlob[] =
        {
        0x00, 0x00, 0x9f, 0xe5,   // ldr r0, [pc, #0] ; вантажить @PC+8
        0x1e, 0xff, 0x2f, 0xe1,   // bx  lr
        0x01, 0x00, 0x5a, 0x5a    // .word 0x5a5a0001
        };
    TPtrC8 blob(kTestBlob, sizeof(kTestBlob));

    TInt result = 0;
    TInt r = e->ExecCustomBlob(blob, result);
    LogF(_L8("  ExecCustomBlob -> r=%d, result=0x%08x (%d)"), r, result, result);

    const TUint32 expected = 0x5a5a0001u;
    if (r == KErrNone && (TUint32)result == expected)
        Log(_L8("  MATCH: custom blob executed in kernel mode and returned expected 0x5a5a0001"));
    else
        LogF(_L8("  MISMATCH: expected 0x%08x, got 0x%08x (r=%d)"), expected, result, r);

    // Зняття патчу та перевірка відновлення коду ядра
    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);

    delete e;
    Log(_L8("custom blob test done"));
    }

// Лічильники продуктивності ARM1136 (CP15 c15 c12). Питання, на яке відповідає
// цей прогін: чи взагалі рахують ці регістри на цьому апараті, чи прошивка
// тримає PMU вимкненим і запис у PMCR нічого не робить.
//
// Два блоби. Перший пише конфігурацію в PMCR: біт 0 - увімкнути, 1 - скинути
// PMN0/PMN1, 2 - скинути CCNT, 3 - ділити CCNT на 64 (без дільника лічильник
// циклів переповнюється за десяток секунд), [19:12] - подія PMN0, [27:20] -
// подія PMN1. Другий читає всі три лічильники; повернути в r0 можна лише одне
// слово, тому решта лягає в літерний пул самого блоба, тобто в SRAM, звідки її
// потім видно READ-патчем.
//
// Події ARM1136: 0x07 - виконані інструкції, 0x0b - промах кешу даних.
static void ProbePmuTest()
    {
    Log(_L8("--- ARM1136 performance counters ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    // ldr r0,[pc,#8] / mcr p15,0,r0,c15,c12,0 / mov r0,#0 / bx lr / .word cfg
    TUint8 setup[] =
        {
        0x08, 0x00, 0x9f, 0xe5,
        0x1c, 0x0f, 0x0f, 0xee,
        0x00, 0x00, 0xa0, 0xe3,
        0x1e, 0xff, 0x2f, 0xe1,
        0x00, 0x00, 0x00, 0x00
        };
    // Обидва лічильники на ту саму подію 0x07 (виконані інструкції): якщо
    // підписи в блобі правильні, обидва числа мають збігтися. Розбіжність
    // означала б, що поля evtCount0 і evtCount1 підписані навпаки.
    const TUint32 cfg = (0x07u << 20) | (0x07u << 12) | 0x0fu;
    setup[16] = (TUint8)(cfg & 0xff);
    setup[17] = (TUint8)((cfg >> 8) & 0xff);
    setup[18] = (TUint8)((cfg >> 16) & 0xff);
    setup[19] = (TUint8)((cfg >> 24) & 0xff);

    // mrc CCNT/PMN0/PMN1 / add r0,pc,#8 / stmia r0,{r1,r2,r3} / mov r0,r1 / bx lr
    static const TUint8 kRead[] =
        {
        0x3c, 0x1f, 0x1f, 0xee,
        0x5c, 0x2f, 0x1f, 0xee,
        0x7c, 0x3f, 0x1f, 0xee,
        0x08, 0x00, 0x8f, 0xe2,
        0x0e, 0x00, 0x80, 0xe8,
        0x01, 0x00, 0xa0, 0xe1,
        0x1e, 0xff, 0x2f, 0xe1,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
        };

    TInt res = 0;
    TPtrC8 sblob(setup, sizeof(setup));
    TInt r = e->ExecCustomBlob(sblob, res);
    LogF(_L8("  PMCR write cfg=0x%08x -> r=%d"), cfg, r);

    TPtrC8 rblob(kRead, sizeof(kRead));
    r = e->ExecCustomBlob(rblob, res);
    LogF(_L8("  first read -> r=%d ccnt=%u"), r, (TUint32)res);
    const TUint32 first = (TUint32)res;

    // Щось порахувати, щоб між двома знімками була видима робота.
    volatile TUint32 acc = 0;
    for (TInt i = 0; i < 2000000; i++)
        acc += (TUint32)i;
    User::After(100000);

    r = e->ExecCustomBlob(rblob, res);
    LogF(_L8("  second read -> r=%d ccnt=%u delta=%u"),
         r, (TUint32)res, (TUint32)res - first);

    // Решта знімка лежить у пулі блоба, тобто в SRAM за зсувом 28 байтів.
    const TUint32 base = e->SramAddress() + 0x800u + 28u;
    const TUint32 addr[3] = { base, base + 4, base + 8 };
    TUint32 out[3] = { 0, 0, 0 };
    const TInt rr = e->ReadRegs(addr, out, 3);
    LogF(_L8("  sram snapshot r=%d ccnt=%u instr=%u dcmiss=%u"),
         rr, out[0], out[1], out[2]);
    if (out[0] == 0 && out[1] == 0 && out[2] == 0)
        Log(_L8("  ALL ZERO: the counters are not running"));
    else
        Log(_L8("  COUNTERS RUNNING"));

    e->EndPatchSession();
    LogF(_L8("  restore check -> %d"), e->VerifyRestored());
    delete e;
    Log(_L8("pmu test done"));
    }


// Домен DSP: ТІЛЬКИ читання, жодного запису в PRCM. Завдання - docs/TASK_DSP_CLOCK.md.
//
// Питання, на яке відповідає цей прогін: у якому полі CM_CLKSEL_DSP стоїть
// дільник C55x і чи справді він 6 (тобто DSP на 110 МГц замість штатних 220).
//
// Чому саме сире значення. Розкладка полів виведена з двох сеттерів у
// ehwaomap.pdd, і жоден із них не чіпає низького поля, тож із самого реверсу
// сеттерів не видно, яке поле тактує ядро C55x. Зате видно з power.dll: ord60
// @0x800B1644 маскує рівно 0xF і ділить на це CORE - саме це число драйвер
// ehwaomap.pdd кладе в символ _BRIDGEINIT_DSP_FREQ, і саме його бімодальність
// (110000/220000) спостерігав сусідній проєкт. Тобто рахуємо тут те саме, що
// й прошивка, і звіряємось із її ж числом.
static void ProbeDsp(TInt aSamples)
    {
    Log(_L8("--- DSP clock domain probe (READ ONLY, no PRCM writes) ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    const TUint32 regs[] =
        { KRegFclkenDsp, KRegIclkenDsp, KRegIdlestDsp, KRegClkSelDsp };
    const char *names[] =
        { "CM_FCLKEN_DSP", "CM_ICLKEN_DSP", "CM_IDLEST_DSP", "CM_CLKSEL_DSP" };
    TUint32 v[4] = { 0, 0, 0, 0 };
    // Усі чотири - за одне накладання патчу: і вікно одне, і знімок узгоджений
    // (стан домену й дільник прочитані практично одночасно).
    TInt r = e->ReadRegs(regs, v, 4);
    if (r != KErrNone)
        {
        LogF(_L8("ReadRegs failed: %d"), r);
        delete e;
        return;
        }
    for (TInt i = 0; i < 4; ++i)
        {
        TPtrC8 nm((const TUint8 *)names[i]);
        LogF(_L8("  %S @%08x = %08x"), &nm, regs[i], v[i]);
        }

    const TUint32 sel = v[3];
    const TInt fwDiv = (TInt)(sel & KDspFwDivMask);          // [3:0] - як бачить ord60
    const TInt dsDiv = (TInt)(sel & KDspDivMask);            // [4:0] - як у даташиті
    const TInt ifDiv = (TInt)((sel & KDspIfMask) >> 5);
    const TInt bit7  = (TInt)((sel & KDspBit7Mask) >> 7);
    const TInt iva   = (TInt)((sel & KDspIvaMask) >> 8);
    const TInt bit13 = (TInt)((sel & KDspBit13Mask) >> 13);
    LogF(_L8("  fields: DSP[3:0]=%d [4:0]=%d  IF[6:5]=%d  bit7=%d  IVA[12:8]=%d  bit13=%d"),
         fwDiv, dsDiv, ifDiv, bit7, iva, bit13);

    const TUint32 coreKhz = e->StockClocks().CoreKhz();
    if (fwDiv)
        LogF(_L8("  firmware math: CORE %u kHz / [3:0]=%d -> %u kHz (this is _BRIDGEINIT_DSP_FREQ)"),
             coreKhz, fwDiv, coreKhz / (TUint32)fwDiv);
    else
        Log(_L8("  firmware math: [3:0] is zero -> ord60 returns 0 (domain divider not set)"));

    // Кілька знімків: спостережене число бімодальне, тож одне читання може
    // застати домен у будь-якому з двох станів і збити з висновку. Кожен
    // знімок - окреме коротке вікно патча; тримати патч накладеним усі кілька
    // секунд було б гірше, бо загальний час, коли в ядрі лежить міна для
    // чужого DoControl(1), вийшов би на порядки більший.
    if (aSamples > 1)
        {
        LogF(_L8("  sampling CM_CLKSEL_DSP %dx, 250 ms apart:"), aSamples);
        TUint32 seen[8];
        TInt seenCount = 0;
        for (TInt s = 0; s < aSamples; ++s)
            {
            TUint32 pair[2] = { 0, 0 };
            const TUint32 addr[2] = { KRegClkSelDsp, KRegIdlestDsp };
            if (e->ReadRegs(addr, pair, 2) != KErrNone)
                break;
            LogF(_L8("    [%2d] CLKSEL=%08x ([3:0]=%d)  IDLEST=%08x"),
                 s, pair[0], (TInt)(pair[0] & KDspFwDivMask), pair[1]);
            TBool known = EFalse;
            for (TInt k = 0; k < seenCount; ++k)
                if (seen[k] == pair[0])
                    known = ETrue;
            if (!known && seenCount < 8)
                seen[seenCount++] = pair[0];
            User::After(250000);
            }
        LogF(_L8("  distinct CM_CLKSEL_DSP values seen: %d"), seenCount);
        for (TInt k = 0; k < seenCount; ++k)
            LogF(_L8("    %08x -> [3:0]=%d"), seen[k], (TInt)(seen[k] & KDspFwDivMask));
        }

    // Висновок формулюємо тут, а не потім по логу: число без тлумачення надто
    // легко прочитати навпаки.
    if (fwDiv == 6)
        Log(_L8("  VERDICT: divider is 6 -> DSP runs at half rate. Step 2 (6->3) is justified."));
    else if (fwDiv == 3)
        Log(_L8("  VERDICT: divider is 3 -> DSP already at nominal. Do NOT write; slowness is elsewhere."));
    else
        LogF(_L8("  VERDICT: unexpected divider %d - do NOT write, report raw value first."), fwDiv);
    if (dsDiv != fwDiv)
        Log(_L8("  WARNING: bit [4] is set, so ord60 (mask 0xF) misreports the DSP frequency."));

    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);
    delete e;
    Log(_L8("DSP probe done"));
    }

// Зміна дільника DSP. Викликається ТІЛЬКИ явним аргументом.
//
// Ризик локалізований у домені DSP: CORE, SDRAM, дисплей і USB не зачіпаються,
// латч PRCM_CLKCFG_CTRL не потрібен. У найгіршому разі до перезавантаження
// ламається аудіо - DSP/BIOS у вже завантаженому базовому образі налаштував
// свої таймери на стару частоту.
//
// Дільник НЕ лишаємо: прогін закінчується поверненням штатного. Щоб лишити,
// є окрема команда "patcher dsphold".
// Режим витримки. Три варіанти існують не для зручності, а щоб РОЗРІЗНИТИ
// причини ребуту при нештатному дільнику (findings 24.5):
//   ESoakPoll  - прокидаємось раз на 5 с. Система не встигає в глибокий
//                простій. Саме так проходила єдина чиста витримка на 120 с;
//   ESoakQuiet - один довгий User::After, жодних прокидань. Дільник тримає
//                НАШ процес, але системі ніщо не заважає піти в retention.
//                Якщо падає тут, а при ESoakPoll ні - справа в простої, а не
//                в тому, кому належить дільник;
//   ESoakRun   - усередині вікна запускаємо дочірній процес (бенчмарк DSP).
//                Дільник увесь час тримаємо ми й гарантовано повертаємо, тож
//                це єдиний спосіб зміряти DSP під навантаженням, не лишаючи
//                нештатний дільник у системі.
enum TSoakMode
    {
    ESoakPoll = 0,
    ESoakQuiet,
    ESoakRun
    };

// Запускає процес і чекає на нього, але НЕ довше за стелю. Без стелі зависла
// дитина тримала б і наш процес, а з ним - і нештатний дільник, тобто рівно
// той стан, якого ми уникаємо.
static TInt RunChild(const TDesC& aExe, const TDesC& aArgs, TInt aCapSec)
    {
    RProcess p;
    TInt r = p.Create(aExe, aArgs);
    if (r != KErrNone)
        {
        LogF(_L8("  child: create failed %d"), r);
        return r;
        }
    TRequestStatus done;
    p.Logon(done);
    RTimer timer;
    r = timer.CreateLocal();
    if (r != KErrNone)
        {
        p.Kill(KErrNone);
        p.Close();
        return r;
        }
    TRequestStatus cap;
    timer.After(cap, aCapSec * 1000000);
    p.Resume();
    Log(_L8("  child: started"));

    User::WaitForRequest(done, cap);
    TInt exitCode = KErrNone;
    if (done != KRequestPending)
        {
        timer.Cancel();
        User::WaitForRequest(cap);
        exitCode = done.Int();
        LogF(_L8("  child: exited, status=%d"), exitCode);
        }
    else
        {
        // Стеля вийшла. Вбиваємо дитину й чекаємо на неї - інакше вона
        // переживе нас разом із дільником.
        Log(_L8("  child: TIMED OUT, killing"));
        p.Kill(KErrTimedOut);
        User::WaitForRequest(done);
        exitCode = KErrTimedOut;
        }
    timer.Close();
    p.Close();
    return exitCode;
    }

static void ProbeDspSet(TInt aDiv, TBool aKeep, TInt aSoakSec, TInt aMode)
    {
    LogF(_L8("--- DSP divider set: %d, keep=%d, soak=%d s, mode=%d ---"),
         aDiv, aKeep ? 1 : 0, aSoakSec, aMode);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TInt before = -1;
    if (e->LiveDspDiv(before) != KErrNone)
        {
        Log(_L8("cannot read CM_CLKSEL_DSP - aborting"));
        delete e;
        return;
        }
    LogF(_L8("  before: div=%d -> DSP %u kHz  (stock div=%d)"),
         before, e->DspKhz(before), e->StockDspDiv());

    TInt rc = KErrGeneral;
    TInt r = e->ApplyDspDiv(aDiv, rc);
    LogF(_L8("  ApplyDspDiv(%d) -> r=%d, firmware setter rc=%d"), aDiv, r, rc);
    if (r != KErrNone)
        {
        Log(_L8("  not applied (our own validation or patch path)"));
        delete e;
        return;
        }
    if (rc == KErrArgument)
        {
        // -6: значення відкинула сама прошивка. Це не збій нашого шляху, а
        // спрацювання її ж поручнів.
        Log(_L8("  firmware REJECTED the value; register untouched"));
        delete e;
        return;
        }
    if (rc != KErrNone)
        {
        LogF(_L8("  blob returned %d (1 = PRCM_CLKCFG_CTRL latch never cleared)"), rc);
        delete e;
        return;
        }

    // Читаємо назад, а не віримо тому, що записували, - правило проєкту.
    TInt after = -1;
    e->LiveDspDiv(after);
    LogF(_L8("  after:  div=%d -> DSP %u kHz"), after, e->DspKhz(after));
    if (after != aDiv)
        Log(_L8("  MISMATCH: register did not take the value"));
    else
        Log(_L8("  register confirmed by read-back"));

    // Витримка. Кожен рядок лягає у файл із Flush, тож якщо апарат помре
    // посеред неї, лог на E: переживе і покаже, на якій секунді це сталось -
    // саме тому витримка розбита на кроки, а не зроблена одним User::After.
    const TInt soak = (aSoakSec > 0) ? aSoakSec : 3;
    if (aMode == ESoakQuiet)
        {
        // Жодних прокидань: рівно один сон на всю витримку. Якщо ребут
        // спричиняє глибокий простій, він має статись саме тут.
        LogF(_L8("  soak %d s QUIET - single sleep, no wakeups"), soak);
        User::After(soak * 1000000);
        TInt still = -1;
        e->LiveDspDiv(still);
        LogF(_L8("  quiet soak survived: div=%d ntick=%u"), still,
             User::NTickCount());
        }
    else if (aMode == ESoakRun)
        {
        LogF(_L8("  soak %d s with CHILD process - divider held by us"), soak);
        _LIT(KChild, "E:\\sys\\bin\\dsptest.exe");
        _LIT(KChildArgs, "baseonlydcdopnobench");
        TInt before2 = -1;
        e->LiveDspDiv(before2);
        LogF(_L8("  div before child: %d"), before2);
        RunChild(KChild, KChildArgs, soak);
        TInt still = -1;
        TInt rr = e->LiveDspDiv(still);
        LogF(_L8("  div after child: %d (read r=%d) ntick=%u"), still, rr,
             User::NTickCount());
        }
    else
        {
        LogF(_L8("  soak %d s, sampling every 5 s (log is flushed per line)"),
             soak);
        for (TInt t = 0; t < soak; t += 5)
            {
            User::After(5000000);
            TInt still = -1;
            TInt rr = e->LiveDspDiv(still);
            LogF(_L8("    t=%3d s: div=%d (read r=%d) ntick=%u"),
                 t + 5, still, rr, User::NTickCount());
            }
        }
    Log(_L8("  soak finished, system alive"));

    if (aKeep)
        {
        e->KeepOnExit(ETrue);
        Log(_L8("  KEEPING the new divider (reboot restores stock)"));
        // Не порада, а виміряний факт: два ребути з двох, один з них у повному
        // простої, без жодного навантаження на DSP (findings 24.5). Витримка з
        // поверненням усередині процесу при цьому проходить чисто.
        if (aDiv != e->StockDspDiv())
            Log(_L8("  WARNING: leaving a non-stock DSP divider rebooted the "
                    "device 2/2 times - use 'dspset <d> <sec>' instead"));
        }
    delete e;
    Log(_L8("DSP divider run done"));
    }

// Бісекція причини перезавантажень. Один рівень за прогін; після кожного
// апарат треба лишити в спокої і подивитись, чи вистоїть.
static void ProbeLevel(TInt aLevel)
    {
    LogF(_L8("--- LEVEL TEST %d ---"), aLevel);
    COcEngine *e = NULL;
    TRAPD(err, e = COcEngine::NewL());
    if (err != KErrNone || !e)
        {
        LogF(_L8("engine alloc failed: %d"), err);
        return;
        }
    TInt verify = -1;
    TUint32 readVal = 0;
    TInt r = e->SelfTestLevel(aLevel, verify, readVal);
    TBuf8<64> step;
    step.Copy(COcEngine::StepName(e->FailedAt()));
    LogF(_L8("  result %d, last step [%S]"), r, &step);
    if (aLevel >= 2)
        {
        LogF(_L8("  read CM_CLKSEL1_PLL = %08x"), readVal);
        LogF(_L8("  restore check -> %d  (nonzero = original kernel code back)"),
             verify);
        }
    delete e;
    Log(_L8("level test done - now leave the phone idle and watch"));
    }

// Застосувати множник і ЗАЛИШИТИ його. Апарат далі просто працює на новій
// частоті; повертає все перезавантаження (у флеш не пишеться нічого) або
// повторний виклик із штатним значенням 86.
//
// Це заразом обхід проблеми переходу з §20: relock робиться безголово, коли
// дисплей не активний, а користувач потім працює на вже встановленій частоті -
// небезпечне лише саме вікно зміни, а не стаціонарний режим.
static void HoldOverclock(TInt aMult)
    {
    LogF(_L8("--- HOLD: set mult %d and keep it ---"), aMult);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TOcClocks want;
    e->ClocksFor(aMult, want);
    LogClocks(_L8("target"), want);
    LogCompensation(e, aMult);

    TUint32 mhzBefore = BestMhz(5);
    TUint32 aluBefore = BestAlu(3, 3000000);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz, %u.%02u Mitr/s, %u MB/s"),
         mhzBefore, aluBefore / 100, aluBefore % 100, memBefore);

    TUint32 status = 0;
    TInt r = e->ApplyMultRamp(aMult, status);
    LogF(_L8("  ApplyMultRamp -> %d, status=%u"), r, status);

    TInt live = 0;
    e->LiveMult(live);
    LogF(_L8("  live mult now %d"), live);
    if (r != KErrNone || live != aMult)
        {
        Log(_L8("  NOT held: ramp failed, engine will restore stock"));
        delete e;
        return;
        }

    // Перечитуємо ВСЮ розкладку, а не лише множник. Рядок "CLKSEL1_CORE X -> Y"
    // вище друкується ДО запису, тобто це намір, а не факт, - і саме цієї
    // різниці бракувало сусідньому проєкту, коли він з'ясовував, чи трималась
    // компенсація дільників. Тепер факт видно прямо тут.
    TClockSnap hs;
    SnapClocks(e, hs);
    LogSnap(_L8("held"), hs);
    if ((hs.iCore & (KCoreL3Mask | KCoreDss1Mask)) !=
        (e->CompensatedCore(aMult) & (KCoreL3Mask | KCoreDss1Mask)))
        Log(_L8("  WARNING: compensated dividers are NOT in the register right after the write"));

    TUint32 mhzAfter = BestMhz(5);
    TUint32 aluAfter = BestAlu(3, 3000000);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz, %u.%02u Mitr/s, %u MB/s"),
         mhzAfter, aluAfter / 100, aluAfter % 100, memAfter);

    TInt bad = MemoryIntegrity(256 * 1024);
    LogF(_L8("  SDRAM integrity 1 MB: %d bad words"), bad);

    if (mhzBefore && mhzAfter)
        {
        TInt p = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        LogF(_L8("  RESULT NOP: %u -> %u MHz = %d.%d%%"),
             mhzBefore, mhzAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    if (memBefore && memAfter)
        {
        TInt p = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT MEM: %u -> %u MB/s = %d.%d%%"),
             memBefore, memAfter, p / 10, (p < 0 ? -p : p) % 10);
        }

    // Частоту лишаємо, патч ядра - НІ: він небезпечний для чужих викликів
    // каналу (findings 20). Деструктор зніме патч і shadow-сторінку, але
    // частоту вже не торкне.
    e->KeepOnExit(ETrue);
    delete e;
    LogF(_L8("  HELD at mult %d. Kernel patch removed; frequency stays."), aMult);
    Log(_L8("  to go back: patcher hold 86, or just reboot"));
    }

// --- повна розкладка клоків -------------------------------------------------
//
// Завдання - docs/TASK_MPU_DIVIDER.md від сусіднього проєкту PS1Emulator.
//
// Питання, на яке це відповідає: скільки з бюджету кадру - робота самого
// ARM1136, а скільки - очікування пам'яті. Розділити їх можна лише
// конфігурацією, у якій ОДИН домен змінився, а другий ні. Штатний "patcher hold"
// такої не дає: він рухає множник DPLL, а з ним CORE, а з CORE одразу і ядро, і
// шину, і дисплей - усі на ті самі відсотки. Тому тут задаються всі чотири поля
// окремо: множник DPLL, дільник MPU (CM_CLKSEL_MPU) і дільники L3 та DSS1
// (обидва в CM_CLKSEL1_CORE).

_LIT8(KLayoutHeld, "HELD");
_LIT8(KLayoutDrift, "DRIFTED");

// Побачили зсув - переписати розкладку назад. Вимкнено за замовчуванням: блоб
// ReassertDividers на апараті ще не виконувався жодного разу, а вмикати
// неперевірений код у ядрі мовчки не можна. Ключ hold=1.
//
// Повертає ETrue, якщо переписали.
static TBool ReassertIfDrifted(COcEngine *aE, const TClockSnap &aWant,
                               const TClockSnap &aNow, TBool aHold)
    {
    if (!aHold || SameLayout(aWant, aNow))
        return EFalse;
    TInt rc = -1;
    TInt r = aE->ReassertDividers(aWant.iCore, aWant.iMpu, rc);
    LogF(_L8("      reassert %08x/%08x -> r=%d rc=%d (rc=1 = CLKCFG latch never cleared)"),
         aWant.iCore, aWant.iMpu, r, rc);
    return ETrue;
    }

// "ключ=значення" з командного рядка; ключ передається разом зі знаком рівності.
static TBool KeyVal(const TDesC &aCmd, const TDesC &aKey, TInt &aOut)
    {
    TInt at = aCmd.Find(aKey);
    if (at == KErrNotFound)
        return EFalse;
    TLex lex(aCmd.Mid(at + aKey.Length()));
    TInt v = 0;
    if (lex.Val(v) != KErrNone)
        return EFalse;
    aOut = v;
    return ETrue;
    }

enum TChildKind
    {
    EChildNone = 0,
    EChildPs1,          // E:\sys\bin\ps1stage1.exe - стенд замовника
    EChildDsp           // E:\sys\bin\dsptest.exe   - стенд DSP-проєкту
    };

// Запуск дитини з періодичним знімком розкладки, поки вона працює.
//
// Стан ДО і ПІСЛЯ сам собою нічого не доводить: питання стоїть саме про те, чи
// трималася розкладка ВЕСЬ час. Губернатор DVFS переписує CM_CLKSEL1_CORE у
// рантаймі - це замовник спостерігав прямо, - і побачити момент можна лише
// зсередини вікна. Стеля часу лишається обов'язковою: зависла дитина інакше
// тримала б і нас, а з нами й нештатну розкладку.
static TInt RunChildWatched(const TDesC &aExe, const TDesC &aArgs, TInt aCapSec,
                            COcEngine *aE, TInt aSampleSec, TInt &aDrift,
                            TBool aHold)
    {
    aDrift = 0;
    RProcess p;
    TInt r = p.Create(aExe, aArgs);
    if (r != KErrNone)
        {
        LogF(_L8("  child: create failed %d"), r);
        return r;
        }
    TRequestStatus done;
    p.Logon(done);
    // Logon, що відмовив, завершує запит одразу, і тоді перше ж очікування
    // виглядало б як "дитина миттєво вийшла": ми зняли б розкладку, а дитина
    // лишилась би працювати. Тому такий запуск скасовуємо, а не наглядаємо
    // наосліп.
    if (done != KRequestPending)
        {
        LogF(_L8("  child: Logon failed (%d) - not running it unwatched"), done.Int());
        User::WaitForRequest(done);
        p.Kill(KErrGeneral);
        p.Close();
        return KErrGeneral;
        }
    RTimer timer;
    r = timer.CreateLocal();
    if (r != KErrNone)
        {
        p.Kill(KErrNone);
        User::WaitForRequest(done);
        p.Close();
        return r;
        }

    TClockSnap want;
    SnapClocks(aE, want);
    p.Resume();
    LogF(_L8("  child: started, cap %d s, sampling every %d s"), aCapSec, aSampleSec);

    TInt elapsed = 0;
    TInt exitCode = KErrNone;
    TBool finished = EFalse;
    while (elapsed < aCapSec)
        {
        TInt step = aSampleSec;
        if (elapsed + step > aCapSec)
            step = aCapSec - elapsed;
        TRequestStatus tick;
        timer.After(tick, step * 1000000);
        User::WaitForRequest(done, tick);
        if (done != KRequestPending)
            {
            timer.Cancel();
            User::WaitForRequest(tick);
            exitCode = done.Int();
            finished = ETrue;
            LogF(_L8("  child: exited at t~%d s, status=%d"), elapsed, exitCode);
            break;
            }
        elapsed += step;
        TClockSnap now;
        TInt rr = SnapClocks(aE, now);
        const TBool same = SameLayout(want, now);
        if (!same)
            ++aDrift;
        const TDesC8 *verdict = same ? &KLayoutHeld : &KLayoutDrift;
        LogF(_L8("    t=%3d s: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x %S (read r=%d)"),
             elapsed, now.iSel1, now.iSel2, now.iMpu, now.iCore, verdict, rr);
        ReassertIfDrifted(aE, want, now, aHold);
        }
    if (!finished)
        {
        Log(_L8("  child: TIMED OUT, killing"));
        p.Kill(KErrTimedOut);
        User::WaitForRequest(done);
        exitCode = KErrTimedOut;
        }
    timer.Close();
    p.Close();
    return exitCode;
    }

// Витримка без дитини: те саме опитування, але під власним навантаженням.
// Саме цим робиться нульовий за вартістю крок із §5 завдання - перевірка, чи не
// переписує губернатор CM_CLKSEL_MPU так само, як переписує CM_CLKSEL1_CORE.
// Навантаження тут потрібне: губернатор рухає OPP лише тоді, коли є робота, і
// перевірка в порожньому простої нічого б не показала.
// Псевдовипадковий заповнювач і звірка з ним. Розділені навмисно: між
// заповненням і звіркою мусить пройти час, інакше це вимір запису, а не
// утримання.
static void RetentionFill(TUint32 *aBuf, TInt aWords)
    {
    TUint32 x = 0x2545f491u;
    for (TInt i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        aBuf[i] = x;
        }
    }

static TInt RetentionCheck(const TUint32 *aBuf, TInt aWords)
    {
    TUint32 x = 0x2545f491u;
    TInt bad = 0;
    for (TInt i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        if (aBuf[i] != x)
            ++bad;
        }
    return bad;
    }

static TInt WatchLayout(COcEngine *aE, TInt aSec, TInt aSampleSec, TBool aHold)
    {
    TClockSnap want;
    SnapClocks(aE, want);
    TInt drift = 0;
    const TInt kRetWords = 256 * 1024;          // 1 МБ, завідомо повз кеш
    TUint32 *ret = (TUint32 *)User::Alloc(kRetWords * sizeof(TUint32));
    if (ret)
        RetentionFill(ret, kRetWords);
    else
        Log(_L8("  retention buffer alloc failed - SDRAM check skipped"));
    // Крок відлічуємо від реального годинника, а не від суми затримок: ALU-цикл
    // при вдвічі нижчій частоті ядра йде вдвічі довше, і мітка "t=" інакше
    // брехала б саме в тій конфігурації, заради якої все робиться.
    const TUint start = User::NTickCount();
    for (TInt t = aSampleSec; t <= aSec; t += aSampleSec)
        {
        // Спершу ALU, потім сон: перше змушує губернатора піднімати OPP, друге -
        // опускати, тож переходи стаються в обидва боки. ~20 млн ітерацій - це
        // близько 1.2 с при штатних 330 МГц і 2.4 с при половинних.
        BestAlu(1, 20000000u);
        const TInt leftMs = t * 1000 - (TInt)(User::NTickCount() - start);
        if (leftMs > 0)
            User::After(leftMs * 1000);
        TClockSnap now;
        TInt rr = SnapClocks(aE, now);
        const TBool same = SameLayout(want, now);
        if (!same)
            ++drift;
        const TDesC8 *verdict = same ? &KLayoutHeld : &KLayoutDrift;
        LogF(_L8("    t=%3d s: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x RFR=%08x %S (r=%d)"),
             t, now.iSel1, now.iSel2, now.iMpu, now.iCore, now.iRfr, verdict, rr);
        // Утримання заряду в SDRAM - пряма перевірка гіпотези §26.1a. Буфер
        // заповнено ОДИН раз до вікна, тож між записом і звіркою проходить
        // цілий інтервал вибірки. Саме довжина цієї паузи й робить вимір
        // чутливим: MemoryIntegrity() звіряє одразу після запису, а там
        // проходять частки мілісекунди, і розтягнутий refresh нічого б не
        // зіпсував.
        if (ret)
            {
            TInt bad = RetentionCheck(ret, kRetWords);
            LogF(_L8("      SDRAM retention over %d s: %d bad words in 1 MB"),
                 aSampleSec, bad);
            RetentionFill(ret, kRetWords);
            }
        ReassertIfDrifted(aE, want, now, aHold);
        }
    if (ret)
        User::Free(ret);
    return drift;
    }

// Головна команда. Порядок кроків не довільний.
//
// ЧОМУ ЗАСТОСУВАННЯ У ДВА КРОКИ. Наївна рампа з кінцевими дільниками на кожній
// сходинці вбила б апарат: конфігурація "mult 43, mpu_div 1" на проміжному
// множнику 86 дала б ядру 660 МГц, а шині 330 - удвічі більше за штатну, тобто
// далеко за межу 426 МГц, встановлену цим же проєктом. Тому спершу йде звичайна
// рампа з АВТОМАТИЧНОЮ компенсацією (перевірений шлях: на кожній сходинці частоти
// лишаються біля штатних), і лише на кінцевому множнику робиться ОДИН явний
// запис потрібної розкладки. На кінцевому множнику вона коректна за побудовою.
static void RunClocks(TInt aMult, TInt aMpu, TInt aL3, TInt aDss, TInt aSec,
                      TInt aChild, const TDesC &aChildArgs, TInt aSampleSec,
                      TBool aHold)
    {
    LogF(_L8("--- CLOCKS: mult=%d mpu=%d l3=%d dss=%d sec=%d child=%d hold=%d ---"),
         aMult, aMpu, aL3, aDss, aSec, aChild, aHold ? 1 : 0);
    if (aHold)
        Log(_L8("  hold=1: divider reassert is ENABLED and has never run on hardware"));

    // Поручні беремо з валідації штатних сеттерів power.dll, а не зі своїх
    // уявлень: дільника MPU 3 на цьому SoC не існує, хоч поле його й вмістило б.
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        { LogF(_L8("  mult %d out of range %d..%d"), aMult, KOcMultMin, KOcMultMax); return; }
    if (aMpu < 0 || aMpu > 31 || !(KMpuAllowed & (1u << aMpu)))
        { LogF(_L8("  mpu divider %d not allowed (1, 2, 4, 6, 8)"), aMpu); return; }
    if (aL3 < 1 || aL3 > 31 || !(CoreField(ECoreL3).iAllowed & (1u << aL3)))
        { LogF(_L8("  l3 divider %d not allowed"), aL3); return; }
    if (aDss < 1 || aDss > 31 || !(CoreField(ECoreDss1).iAllowed & (1u << aDss)))
        { LogF(_L8("  dss divider %d not allowed"), aDss); return; }

    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TUint32 coreReg = CoreFieldSet(e->StockCore(), ECoreL3, aL3);
    coreReg = CoreFieldSet(coreReg, ECoreDss1, aDss);
    const TUint32 mpuReg = (e->StockMpu() & ~KMpuDivMask) | ((TUint32)aMpu & KMpuDivMask);

    // Що з цього вийде - рахуємо ДО запису, щоб у лозі лишився намір, з яким
    // конфігурацію ставили, а не лише те, що потім прочиталось.
    TOcClocks want;
    e->ClocksFor(aMult, want);
    want.iMpuDiv = aMpu;
    want.iL3Div = aL3;
    want.iDss1Div = aDss;
    LogF(_L8("  target: CORE=%08x MPU=%08x -> CORE=%u MPU=%u BUS=%u DSS1=%u kHz"),
         coreReg, mpuReg, want.CoreKhz(), want.MpuKhz(), want.L3Khz(), want.Dss1Khz());

    TClockSnap snap;
    SnapClocks(e, snap);
    LogSnap(_L8("before"), snap);

    // Прилад ДО зміни. Читання регістра каже, що ми записали; NOP-цикл і
    // пропускна здатність кажуть, що з цього вийшло насправді, - а правило
    // проєкту вимагає саме другого.
    TUint32 mhzBefore = BestMhz(5);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz (NOP), %u MB/s (MEM)"), mhzBefore, memBefore);

    // Крок 1: рампа до потрібного множника зі штатною компенсацією.
    //
    // Робимо її ТУТ, а не викликом ApplyMultRamp, лише заради логу: кожна
    // сходинка лягає у файл із flush, тож якщо апарат помре посеред рампи, з
    // логу буде видно, на якому саме множнику. Без цього єдина спроба спуститись
    // до 43 лишила по собі порожнечу після рядка BEFORE, і локалізувати відмову
    // не було чим (§26.10). Семантика та сама: крок на один пункт, автопідбір
    // дільників, 20 мс на осідання, зупинка при ненульовому статусі.
    TUint32 status = 0;
    TInt r = KErrNone;
    TInt cur = 0;
    r = e->LiveMult(cur);
    if (r != KErrNone)
        {
        LogF(_L8("  cannot read live multiplier: %d"), r);
        delete e;
        return;
        }
    LogF(_L8("  ramp %d -> %d, one step at a time"), cur, aMult);
    while (cur != aMult && r == KErrNone)
        {
        cur += (aMult > cur) ? 1 : -1;
        TUint32 st = 0;
        const TUint32 stepCore = e->CompensatedCore(cur);
        r = e->ApplyMultAndCore(cur, stepCore, 0, st);
        status |= st;
        TOcClocks sc;
        e->ClocksFor(cur, sc);
        sc.iL3Div = (TInt)(stepCore & KCoreL3Mask);
        sc.iDss1Div = (TInt)((stepCore & KCoreDss1Mask) >> KCoreDss1Shift);
        LogF(_L8("    m=%3d core=%08x BUS=%u DSS1=%u -> r=%d st=%u"),
             cur, stepCore, sc.L3Khz(), sc.Dss1Khz(), r, st);
        if (r != KErrNone || st != 0)
            break;
        User::After(20000);
        }
    LogF(_L8("  ramp done at m=%d -> r=%d status=%u"), cur, r, status);
    if (r != KErrNone || status != 0)
        {
        Log(_L8("  ramp failed; engine restores stock on exit"));
        delete e;
        return;
        }

    // Крок 2: один явний запис кінцевої розкладки на кінцевому множнику.
    status = 0;
    r = e->ApplyMultAndCore(aMult, coreReg, mpuReg, status);
    LogF(_L8("  layout write -> r=%d status=%u (0=ok,1=bypass,2=cfg,4=lock,8=rollback failed)"),
         r, status);
    if (r != KErrNone || status != 0)
        {
        Log(_L8("  layout NOT applied; engine restores stock on exit"));
        delete e;
        return;
        }

    SnapClocks(e, snap);
    LogSnap(_L8("applied"), snap);
    if ((snap.iMpu & KMpuDivMask) != (TUint32)aMpu ||
        (snap.iCore & KCoreL3Mask) != (TUint32)aL3 ||
        ((snap.iCore & KCoreDss1Mask) >> KCoreDss1Shift) != (TUint32)aDss)
        Log(_L8("  MISMATCH: hardware did not take the layout"));
    else
        Log(_L8("  layout confirmed by read-back"));

    TUint32 mhzAfter = BestMhz(5);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz (NOP), %u MB/s (MEM)"), mhzAfter, memAfter);
    if (mhzBefore && memBefore)
        {
        TInt pc = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        TInt pm = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT: CPU %d.%d%%  MEM %d.%d%%  <- these two must differ, that is the point"),
             pc / 10, (pc < 0 ? -pc : pc) % 10, pm / 10, (pm < 0 ? -pm : pm) % 10);
        }

    const TInt sec = (aSec > 0) ? aSec : 30;
    const TInt sample = (aSampleSec > 0) ? aSampleSec : 10;
    TInt drift = 0;
    if (aChild == EChildNone)
        {
        LogF(_L8("  watching layout for %d s under own load, sample %d s"), sec, sample);
        drift = WatchLayout(e, sec, sample, aHold);
        }
    else
        {
        _LIT(KPs1, "E:\\sys\\bin\\ps1stage1.exe");
        _LIT(KDsp, "E:\\sys\\bin\\dsptest.exe");
        _LIT(KDspArgs, "baseonlydcdopnobench");
        const TDesC &exe = (aChild == EChildPs1) ? KPs1() : KDsp();
        // Аргументи беремо з командного рядка (після "args:"). Свій сталий набір
        // є лише в DSP-стенду; стенд замовника без аргументів запускати не
        // вигадуємо - хай отримає порожній рядок і сам вирішує.
        const TDesC &args = (aChildArgs.Length() > 0) ? aChildArgs
                          : ((aChild == EChildDsp) ? KDspArgs() : KNullDesC());
        TBuf8<160> a8;
        a8.Copy(args.Left(a8.MaxLength()));
        LogF(_L8("  child args: [%S]"), &a8);
        RunChildWatched(exe, args, sec, e, sample, drift, aHold);
        }

    // Читання ПІСЛЯ дитини - критерій приймання §8 завдання. Воно ж і відповідь
    // на питання §5: чи тримається розкладка без резидентного патчу (між нашими
    // знімками патч у ядрі не живе - EndPatchSession знімає його щоразу).
    SnapClocks(e, snap);
    LogSnap(_L8("after"), snap);
    LogF(_L8("  drift samples: %d (0 = layout held the whole window)"), drift);

    // Повернення штатного стану розписане покроково, а не викликом RestoreStock.
    //
    // Причина - три смерті апарата поспіль саме на цьому місці (§26.14). Лог
    // щоразу обривався після рядка `drift samples`, тобто десь усередині
    // повернення, і що там сталось - трьома шляхами з трьох було не видно.
    // Тепер кожен крок пише про себе до того, як його зробити.
    Log(_L8("  restore: step 1, read live layout"));
    SnapClocks(e, snap);
    LogSnap(_L8("restore-before"), snap);

    const TInt liveMult = (TInt)((snap.iSel1 >> 12) & 0x3ff);
    if (liveMult != e->StockMult())
        {
        LogF(_L8("  restore: step 2, ramp %d -> %d"), liveMult, e->StockMult());
        status = 0;
        r = e->RestoreStock(status);
        LogF(_L8("  restore: ramp done -> r=%d status=%u"), r, status);
        }
    else
        {
        // Множник уже штатний, тож потрібні лише дільники. Легкий шлях: запис
        // плюс латч, без bypass DPLL.
        Log(_L8("  restore: step 2 skipped, multiplier already stock"));
        Log(_L8("  restore: step 3, ReassertDividers (no DPLL bypass)"));
        TInt rc = -1;
        r = e->ReassertDividers(KStockClkSel1Core, KStockClkSelMpu, rc);
        LogF(_L8("  restore: reassert -> r=%d rc=%d (rc=1 = CLKCFG latch stuck)"), r, rc);
        Log(_L8("  restore: step 4, read back"));
        SnapClocks(e, snap);
        LogSnap(_L8("restore-after-light"), snap);
        if ((snap.iCore & (KCoreL3Mask | KCoreDss1Mask)) !=
            (KStockClkSel1Core & (KCoreL3Mask | KCoreDss1Mask)))
            {
            Log(_L8("  restore: light path did NOT take, falling back to full relock"));
            status = 0;
            r = e->RestoreStock(status);
            LogF(_L8("  restore: full relock -> r=%d status=%u"), r, status);
            }
        }
    Log(_L8("  restore: step 5, final read"));
    SnapClocks(e, snap);
    LogSnap(_L8("restored"), snap);

    e->EndPatchSession();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         e->VerifyRestored());
    delete e;
    Log(_L8("CLOCKS run done"));
    }

// Етап розгону: міняє множник, міряє трьома приладами, витримує під
// навантаженням, повертає штатне значення.
static void RunOverclock(TInt aMult)
    {
    LogF(_L8("--- OVERCLOCK: target mult %d ---"), aMult);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TOcClocks want;
    e->ClocksFor(aMult, want);
    LogClocks(_L8("target"), want);
    LogCompensation(e, aMult);

    const TUint32 kAluIter = 3000000;
    TUint32 mhzBefore = BestMhz(5);
    TUint32 aluBefore = BestAlu(3, kAluIter);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz (NOP), %u.%02u Mitr/s (ALU), %u MB/s (MEM)"),
         mhzBefore, aluBefore / 100, aluBefore % 100, memBefore);

    TUint32 status = 0;
    TInt r = e->ApplyMultRamp(aMult, status);
    LogF(_L8("  ApplyMultRamp -> %d, status=%u (0=ok,1=bypass,2=cfg,4=lock,8=rollback failed)"),
         r, status);
    if (r != KErrNone)
        {
        delete e;
        return;
        }
    TInt live = 0;
    e->LiveMult(live);
    LogF(_L8("  live mult now %d"), live);

    TUint32 mhzAfter = BestMhz(5);
    TUint32 aluAfter = BestAlu(3, kAluIter);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz (NOP), %u.%02u Mitr/s (ALU), %u MB/s (MEM)"),
         mhzAfter, aluAfter / 100, aluAfter % 100, memAfter);

    Log(_L8("  soak under OC:"));
    TInt badTotal = 0;
    for (TInt s = 0; s < 5; ++s)
        {
        TUint32 a = BestAlu(1, kAluIter);
        TInt b = MemoryIntegrity(256 * 1024);
        if (b > 0)
            badTotal += b;
        LogF(_L8("    round %d: %u.%02u Mitr/s, %d bad words"),
             s + 1, a / 100, a % 100, b);
        }
    LogF(_L8("  soak total bad words: %d"), badTotal);

    TUint32 st2 = 0;
    e->RestoreStock(st2);
    e->LiveMult(live);
    LogF(_L8("  restored: live mult %d, status=%u"), live, st2);

    TUint32 mhzRestored = BestMhz(5);
    TUint32 aluRestored = BestAlu(3, kAluIter);
    LogF(_L8("  RESTORED: %u MHz (NOP), %u.%02u Mitr/s (ALU)"),
         mhzRestored, aluRestored / 100, aluRestored % 100);

    // Три незалежні прилади мусять дати той самий відсоток. Розбіжність між
    // ними - привід не вірити жодному (CLAUDE.md: міряти роботу, не напис).
    if (mhzBefore && mhzAfter)
        {
        TInt p = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        LogF(_L8("  RESULT NOP: %u -> %u MHz = %d.%d%%"),
             mhzBefore, mhzAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    if (aluBefore && aluAfter)
        {
        TInt p = (TInt)(((TInt64)aluAfter - aluBefore) * 1000 / aluBefore);
        LogF(_L8("  RESULT ALU: %u.%02u -> %u.%02u Mitr/s = %d.%d%%"),
             aluBefore / 100, aluBefore % 100, aluAfter / 100, aluAfter % 100,
             p / 10, (p < 0 ? -p : p) % 10);
        }
    if (memBefore && memAfter)
        {
        TInt p = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT MEM: %u -> %u MB/s = %d.%d%%"),
             memBefore, memAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    delete e;
    }

// Знімає зависші екземпляри з попередніх прогонів: інакше вони тримають
// відкритий канал до драйвера. Те саме, що робить демон для себе.
static void KillStaleInstances()
    {
    const TProcessId myId = RProcess().Id();
    TFindProcess finder(_L("ocprobe*"));
    TFullName name;
    TInt killed = 0;
    while (finder.Next(name) == KErrNone)
        {
        RProcess p;
        if (p.Open(name) != KErrNone)
            continue;
        if (p.Id() != myId && p.ExitType() == EExitPending)
            {
            p.Kill(KErrNone);
            ++killed;
            }
        p.Close();
        }
    if (killed)
        LogF(_L8("killed %d stale ocprobe instance(s)"), killed);
    }

static void ProbeHal()
    {
    Log(_L8("--- HAL ---"));
    struct TEntry { HALData::TAttribute iAttr; const char *iName; };
    const TEntry attrs[] =
        {
            { HALData::ECPUSpeed,             "ECPUSpeed(kHz)"       },
            { HALData::EFastCounterFrequency, "EFastCounterFreq(Hz)" },
            { HALData::ENanoTickPeriod,       "ENanoTickPeriod(us)"  },
            { HALData::ESystemTickPeriod,     "ESystemTickPeriod"    },
            { HALData::ECPU,                  "ECPU"                 },
            { HALData::ECPUABI,               "ECPUABI"              },
            { HALData::EMemoryRAM,            "EMemoryRAM"           },
            { HALData::EMemoryRAMFree,        "EMemoryRAMFree"       }
        };
    const TInt n = sizeof(attrs) / sizeof(attrs[0]);
    for (TInt i = 0; i < n; ++i)
        {
        TInt v = 0;
        TInt r = HAL::Get(attrs[i].iAttr, v);
        TPtrC8 nm((const TUint8 *)attrs[i].iName);
        if (r == KErrNone)
            LogF(_L8("%S = %d"), &nm, v);
        else
            LogF(_L8("%S -> err %d"), &nm, r);
        }
    }

// Перелічує вже завантажені драйвери. Це дає ТОЧНЕ зареєстроване ім'я каналу,
// замість здогадок за рядками всередині файлу.
static void ListDrivers()
    {
    TFullName name;

    Log(_L8("--- loaded LDDs ---"));
    TFindLogicalDevice findL(_L("*"));
    TInt count = 0;
    while (findL.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  LDD %S"), &n8);
        ++count;
        }
    LogF(_L8("  total: %d"), count);

    Log(_L8("--- loaded PDDs ---"));
    TFindPhysicalDevice findP(_L("*"));
    count = 0;
    while (findP.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  PDD %S"), &n8);
        ++count;
        }
    LogF(_L8("  total: %d"), count);
    }

static void ProbeDriver()
    {
    Log(_L8("--- DVFS driver ---"));

    TInt r = User::LoadLogicalDevice(KLddFile);
    LogF(_L8("LoadLogicalDevice(dvfs_interface.ldd) -> %d   [0=ok, -11=already loaded]"), r);

    // Версію не вгадуємо наосліп: драйвери зазвичай приймають будь-яку не вищу
    // за власну, тож починаємо з нуля і піднімаємось.
    const TVersion vers[] =
        { TVersion(0,0,0), TVersion(1,0,0), TVersion(1,1,0), TVersion(2,0,0) };
    const TInt nv = sizeof(vers) / sizeof(vers[0]);
    const TDesC *names[2]; names[0] = &KLddName; names[1] = &KLddNameAlt;

    for (TInt n = 0; n < 2; ++n)
        {
        TBuf8<64> nm8; nm8.Copy(*names[n]);
        for (TInt i = 0; i < nv; ++i)
            {
            RDvfs ch;
            TInt o = ch.Open(*names[n], vers[i]);
            LogF(_L8("DoCreate(%S, v%d.%d.%d) -> %d"), &nm8,
                 (TInt)vers[i].iMajor, (TInt)vers[i].iMinor, (TInt)vers[i].iBuild, o);
            if (o == KErrNone)
                {
                Log(_L8("  CHANNEL OPENED - user-side access to DVFS exists"));
                ch.Close();
                return;
                }
            }
        }
    }

// --- E90: перелік завантажених драйверів (носіїв READ-патча) -----------------
//
// На E90 dvfs немає, тож штатний READ-примітив RM-320 недоступний. Сам патч
// (cmp r1,#0; ldrne r0,[r1]; moveq r0,#0; bx lr) драйвер-агностичний і його
// можна покласти в тіло opcode будь-якого користувацьки-відкривного драйвера,
// чиє тіло лежить у ROM.
//
// ВАЖЛИВО (урок 2026-09-02): цей скан лише ПЕРЕЛІЧУЄ драйвери через
// TFindLogicalDevice - жодного DoCreate. Попередня версія сліпо відкривала
// канал до кожного завантаженого LDD; один із них завалив ядро й апарат
// перезавантажився. Відкривати kernel-канал наосліп не можна - деякі драйвери
// на DoCreate виконують ініціалізацію, що падає без коректних параметрів.
// Вибір конкретного носія й розбір його DoControl робиться ОФЛАЙН за
// резидентним XIP; відкриває наступна версія пробника рівно ОДИН вибраний
// драйвер, і робить це під захистом, з логом імені ПЕРЕД спробою.
static void E90ScanReaders()
    {
    Log(_L8("=== E90: loaded drivers (read-patch host candidates) ==="));
    Log(_L8("(list only - NO channel is opened here; opening is done later, one vetted driver)"));

    TFullName name;
    TFindLogicalDevice findL(_L("*"));
    TInt total = 0;
    while (findL.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  LDD %S"), &n8);
        ++total;
        }
    LogF(_L8("--- %d LDD total ---"), total);

    TFindPhysicalDevice findP(_L("*"));
    TInt tp = 0;
    while (findP.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  PDD %S"), &n8);
        ++tp;
        }
    LogF(_L8("=== %d LDD, %d PDD ==="), total, tp);
    }

// --- Бутстрап READ-примітиву E90 через хост Usbc (findings §32) --------------
//
// На E90 dvfs немає, тож примітив читання/виклику ставиться записом RomPatcher+
// у тіло DoControl стандартного драйвера USB-клієнта DLddUsbcChannel (LDD-ім'я
// "Usbc"): і на RM-320, і на E90 пролог DoControl байт-у-байт однаковий, а
// аргумент a1 (адреса) приходить у r2. READ-стаб (варіант r2):
//   cmp r2,#0 ; ldrne r0,[r2] ; moveq r0,#0 ; bx lr
// Диспетчер HandleMsg спеціально пропускає opcode 69 (0x45) до DoControl навіть
// коли канал ще не готовий (USB без кабелю), тож читаємо через DoControl(69,addr).
// Пролог не виконується (stmdb/sub пропущено), sp не чіпається, bx lr повертає
// r0 у результат DoControl користувача.

_LIT(KUsbcName, "Usbc");

static const TUint32 KUsbcReadStub[4] =
    { 0xe3520000u,   // cmp   r2, #0
      0x15920000u,   // ldrne r0, [r2]
      0x03a00000u,   // moveq r0, #0
      0xe12fff1eu }; // bx    lr

// Штатні перші 4 слова DLddUsbcChannel::DoControl (однакові на обох апаратах),
// для відкату. Решта прологу за ними не чіпається.
static const TUint32 KUsbcOriginal[4] =
    { 0xe92d4ff0u,   // stmdb sp!, {r4,r5,r6,r7,r8,r9,sl,fp,lr}
      0xe24dd044u,   // sub   sp, sp, #68
      0xe1a04000u,   // mov   r4, r0
      0xe1a06002u }; // mov   r6, r2

// Хост-канал: єдина потрібна дія - DoControl(69, addr) для читання після патчу.
class RUsbcHost : public RBusLogicalChannel
    {
public:
    TInt Open(const TVersion& aVer)
        { return DoCreate(KUsbcName, aVer, KNullUnit, NULL, NULL, EOwnerProcess); }
    TInt Read(TUint32 aAddress) { return DoControl(69, (TAny*)aAddress); }
    };

// Три відомі з ROM слова - незалежна перевірка примітиву (та сама на обох
// апаратах): вектор скидання, пул множника, перше слово SRAM-рутини.
struct TKnownWord { TUint32 iAddr; TUint32 iVal; const char* iName; };

static void UsbcBoot(TBool aIsE90)
    {
    Log(_L8("=== usbcboot: bootstrap READ primitive via Usbc host (findings 32) ==="));

    const TUint32 host = aIsE90 ? 0x80094ac4u : 0x8010bd04u;
    LogF(_L8("device=%s  host DLddUsbcChannel::DoControl=0x%08x  a1 in r2"),
         aIsE90 ? "E90" : "RM-320", host);

    // Відомі слова: pool однаковий за значенням, адреси різні між апаратами.
    const TUint32 pool = aIsE90 ? 0x800b76dcu : 0x800a7f6cu; // sram-routine addr
    const TUint32 poolMult = aIsE90 ? 0x800b76dcu : 0x800b16ecu;
    TKnownWord known[3];
    known[0].iAddr = 0x80000000u; known[0].iVal = 0xea00007eu; known[0].iName = "reset";
    known[1].iAddr = poolMult;    known[1].iVal = 0xc6008540u; known[1].iName = "mult-pool";
    known[2].iAddr = aIsE90 ? 0x80056874u : 0x800a7f6cu;
    known[2].iVal  = 0xe92d40feu; known[2].iName = "sram-routine";
    (void)pool;

    // 1) Патчер.
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    LogF(_L8("LoadLogicalDevice(patcher.ldd) -> %d  [0=ok, -11=already]"), r);
    if (r != KErrNone && r != KErrAlreadyExists)
        { Log(_L8("  patcher not loadable - ABORT (nothing patched)")); return; }
    RPatcherChannel patcher;
    r = patcher.Open();
    LogF(_L8("patcher.Open() -> %d"), r);
    if (r != KErrNone) { Log(_L8("  ABORT (nothing patched)")); return; }

    // 2) Хост Usbc. Лог імені ПЕРЕД відкриттям (урок 2026-09-02). Відкриваємо
    //    РІВНО цей один вивірений драйвер.
    Log(_L8("opening host channel \"Usbc\" (DLddUsbcChannel) ..."));
    const TVersion vers[] = { TVersion(0,0,0), TVersion(1,0,0), TVersion(2,0,0) };
    RUsbcHost usb;
    TInt o = KErrNotFound;
    for (TInt i = 0; i < 3 && o != KErrNone; ++i)
        {
        o = usb.Open(vers[i]);
        LogF(_L8("  DoCreate(Usbc, v%d.%d.%d) -> %d"),
             (TInt)vers[i].iMajor, (TInt)vers[i].iMinor, (TInt)vers[i].iBuild, o);
        }
    if (o != KErrNone)
        { Log(_L8("  Usbc channel did NOT open - ABORT (nothing patched)")); patcher.Close(); return; }
    Log(_L8("  Usbc channel OPENED"));

    // 3) READ-стаб у тіло DoControl.
    TUint32 addr = host;
    TBuf8<16> stub;
    stub.Copy((const TUint8*)KUsbcReadStub, sizeof(KUsbcReadStub));
    r = patcher.WriteBytes(addr, stub);
    LogF(_L8("WriteBytes(read-stub, 4 words @0x%08x) -> %d"), host, r);

    if (r == KErrNone)
        {
        // 4) Читаємо три відомі слова через хост.
        TInt ok = 0;
        for (TInt i = 0; i < 3; ++i)
            {
            TUint32 got = (TUint32)usb.Read(known[i].iAddr);
            const TBool match = (got == known[i].iVal);
            if (match) ++ok;
            TBuf8<24> nm; nm.Copy(TPtrC8((const TUint8*)known[i].iName));
            LogF(_L8("  READ [0x%08x] (%S) -> 0x%08x  expect 0x%08x  %s"),
                 known[i].iAddr, &nm, got, known[i].iVal, match ? "MATCH" : "*** MISMATCH ***");
            }
        LogF(_L8("  === %d/3 known words matched -> read primitive %s ==="),
             ok, (ok == 3) ? "WORKS" : "FAILED");
        }

    // 5) Відкат ЗАВЖДИ: оригінальні слова + FreeShadowPage (скидання I-cache).
    addr = host;
    stub.Copy((const TUint8*)KUsbcOriginal, sizeof(KUsbcOriginal));
    TInt rr = patcher.WriteBytes(addr, stub);
    LogF(_L8("restore WriteBytes(original, 4 words) -> %d"), rr);
    TUint32 faddr = host, flen = sizeof(KUsbcOriginal);
    TInt fs = patcher.FreeShadow(faddr, flen);
    LogF(_L8("FreeShadowPage(0x%08x, %d) -> %d"), host, (TInt)sizeof(KUsbcOriginal), fs);

    usb.Close();
    patcher.Close();
    Log(_L8("=== usbcboot done: host restored, channels closed ==="));
    }

// Найважливіше питання після першого прогону: 206 МГц - це простій чи стеля?
// Тримаємо ядро зайнятим і паралельно знімаємо те, що HAL каже про частоту.
// Якщо губернатор піднімає частоту під навантаженням, це видно тут.
static void SampleUnderLoad()
    {
    Log(_L8("--- CPU speed under sustained load ---"));
    for (TInt i = 0; i < 10; ++i)
        {
        TUint32 t0 = User::FastCounter();
        SpinAlu(3000000);
        TUint32 t1 = User::FastCounter();
        TInt sp = 0;
        HAL::Get(HALData::ECPUSpeed, sp);
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 m100 = us ? (TUint32)(((TUint64)3000000u * 100u) / us) : 0;
        LogF(_L8("  t=%d ECPUSpeed=%d kHz  %u.%02u Mitr/s"),
             i, sp, m100 / 100, m100 % 100);
        }
    }

// Читає caps драйвера. RDevice віддає структуру, що в переважної більшості
// драйверів починається з TVersion - звідти дізнаємось справжню версію, не
// перебираючи її навмання.
static void ProbeDeviceCaps()
    {
    Log(_L8("--- dvfs_interface caps ---"));
    RDevice dev;
    TInt r = dev.Open(KLddName);
    LogF(_L8("RDevice::Open -> %d"), r);
    if (r != KErrNone)
        return;

    TBuf8<64> caps;
    caps.FillZ(caps.MaxLength());
    dev.GetCaps(caps);
    LogF(_L8("GetCaps -> len %d"), caps.Length());
    TBuf8<160> hex;
    for (TInt i = 0; i < caps.Length() && i < 32; ++i)
        hex.AppendFormat(_L8("%02x "), caps[i]);
    LogF(_L8("caps bytes: %S"), &hex);
    if (caps.Length() >= 6)
        LogF(_L8("as TVersion: %d.%d.%d"),
             (TInt)caps[0] | ((TInt)caps[1] << 8),
             (TInt)caps[2] | ((TInt)caps[3] << 8),
             (TInt)caps[4] | ((TInt)caps[5] << 8));
    dev.Close();
    }

// Один замір абсолютної частоти. Повертає МГц, щоб можна було порівнювати
// стан "до" і "після" в межах одного прогону, без переносу через перезапуск.
static TUint32 MeasureMhz()
    {
    const TUint32 kIter = 400000;
    TUint32 t0 = User::FastCounter();
    SpinNop(kIter);
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    return us ? (TUint32)(((TUint64)kIter * KNopCyclesPerIter) / us) : 0;
    }

static void MeasureAbsoluteClock()
    {
    Log(_L8("--- absolute clock ---"));
    SpinNop(200000);                       // розігрів
    for (TInt run = 0; run < 3; ++run)
        LogF(_L8("NOP run %d: %u MHz"), run + 1, MeasureMhz());
    }

// Єдина зміна стану за весь пробник. Резервуємо максимальну продуктивність,
// міряємо, звільняємо, міряємо ще раз. Резерв/звільнення парні, тож лічильник
// у драйвері повертається туди ж, звідки почали.
static void ExperimentReserve()
    {
    Log(_L8("--- reserve experiment ---"));
    RDvfs ch;
    TInt r = ch.Open(KLddName, TVersion(0,0,0));
    if (r != KErrNone)
        {
        LogF(_L8("open failed: %d - experiment skipped"), r);
        return;
        }

    // Опкоди, крім 1, за кодом мають повертати 0 - перевіряємо це твердження.
    for (TInt op = 0; op < 4; ++op)
        LogF(_L8("DoControl(%d) -> %d"), op, ch.Ctl(op));

    LogF(_L8("before      : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());

    TInt rr = ch.SetPerformance(100);          // > 50 -> reserve
    LogF(_L8("reserve(100): status %d"), rr);
    LogF(_L8("reserved    : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());
    LogF(_L8("reserved    : %u MHz (2nd)"), MeasureMhz());

    TInt rl = ch.SetPerformance(0);            // <= 50 -> release
    LogF(_L8("release(0)  : status %d"), rl);
    LogF(_L8("released    : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());

    ch.Close();
    Log(_L8("channel closed, reservation balanced"));
    }

static void Benchmark()
    {
    Log(_L8("--- measurement ---"));

    // Розігрів: перший прогін ловить промахи кешу і можливий підйом частоти
    // губернатором DVFS, тому в залік не йде.
    SpinAlu(2000000);

    const TUint32 kIter = 8000000;
    TInt run;
    for (run = 0; run < 3; ++run)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinAlu(kIter);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        // Мітерацій/с у сотих: цілочисельно, без плаваючої коми.
        TUint32 m100 = us ? (TUint32)(((TUint64)kIter * 100u) / us) : 0;
        LogF(_L8("ALU run %d: %u us, %u.%02u Mitr/s (sink=%u)"),
             run + 1, us, m100 / 100, m100 % 100, sink);
        }

    const TInt kWords = 512 * 1024;          // 2 МБ - гарантовано повз кеш
    TUint32 *buf = (TUint32 *)User::Alloc(kWords * sizeof(TUint32));
    if (!buf)
        {
        Log(_L8("MEM: allocation failed, skipped"));
        return;
        }
    TInt i;
    for (i = 0; i < kWords; ++i)
        buf[i] = (TUint32)i;

    for (run = 0; run < 2; ++run)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinMem(buf, kWords, 8);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 lines = (TUint32)(kWords / 16) * 8u;
        TUint32 mbs = us ? (TUint32)(((TUint64)lines * 64u) / us) : 0;   // МБ/с
        LogF(_L8("MEM run %d: %u us, ~%u MB/s (sink=%u)"), run + 1, us, mbs, sink);
        }
    User::Free(buf);
    }

static void RunL()
    {
    User::LeaveIfError(gFs.Connect());
    gFs.MkDirAll(KLogFile);
    // Перезаписуємо: кожен прогін - самодостатній звіт.
    User::LeaveIfError(gLog.Replace(gFs, KLogFile, EFileWrite | EFileShareAny));

    Log(_L8("== ocprobe: DVFS probe, read-only =="));
    // ПРЯМИЙ доказ перезавантаження. Мережева доступність його не показує:
    // апарат встигає піднятись і знову відповідати між нашими перевірками.
    // Лічильник тіків росте від старту системи й обнуляється лише ребутом,
    // тож порівняння цього числа між прогонами - єдине надійне свідчення.
    {
    TUint ntick = User::NTickCount();
    TInt np = 1000;
    HAL::Get(HALData::ENanoTickPeriod, np);
    TUint upSec = (TUint)(((TUint64)ntick * (TUint64)np) / 1000000u);
    LogF(_L8("UPTIME: %u s (%u min) - ntick=%u"), upSec, upSec / 60u, ntick);
    }
    KillStaleInstances();

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        gFcFreq = (TUint32)fc;
    if (gFcFreq)
        LogF(_L8("fast counter: %u Hz"), gFcFreq);
    else
        Log(_L8("fast counter: UNAVAILABLE - all timings will read zero"));

    // Прогони з патчером ітеративні, тож повільну оглядову частину пропускаємо:
    // вона нічого не додає, а кожна зайва хвилина - це хвилина на апараті.
    // Командний рядок читаємо ОДИН раз і в буфер на 256 символів. Причина
    // конкретна: аргументи стенду замовника ("gui play nosnd frames6100 dsa
    // light blend imggt.pbp") самі по собі займають півсотні, і в попередньому
    // TBuf<64> команда мовчки обрізалась би посередині. 256 - рівно стільки ж,
    // скільки приймає демон розгортання (tester_exec.cpp, TBuf<256> args).
    //
    // Усе після маркера "args:" належить ДИТИНІ, а не нам, і відрізається
    // одразу. Інакше слово з чужих аргументів ловилось би нашим власним Find і
    // запускало б чужу гілку.
    TBuf<256> full;
    User::CommandLine(full);
    TPtrC cmd(full);
    TPtrC childArgs(KNullDesC);
    TInt sep = full.Find(_L("args:"));
    if (sep != KErrNotFound)
        {
        cmd.Set(full.Left(sep));
        childArgs.Set(full.Mid(sep + 5));
        }
    const TBool patcherRun = (cmd.Find(_L("patcher")) != KErrNotFound);
    // E90-режим: лише HAL + скан відкривних каналів, без повільної оглядової
    // частини й без dvfs-проби (dvfs на E90 нема).
    const TBool e90scan = (cmd.Find(_L("e90scan")) != KErrNotFound);
    // Бутстрап READ-примітиву через хост Usbc. "usbcboot e90" - E90-адреси,
    // інакше RM-320 (де є dvfs-оракул для звірки самого механізму).
    const TBool usbcboot = (cmd.Find(_L("usbcboot")) != KErrNotFound);
    const TBool quiet = patcherRun || e90scan || usbcboot;
    // Профіль E90 для patcher-команд: "patcher ... e90" бере KProfileE90. Тоді
    // Prepare() (шлях EReadDvfs) свідомо відмовить (EOcStepUnsupported) замість
    // патчити RM-320-адреси в чуже ядро E90 (findings §27). Розгінний шлях E90
    // (через Usbc) - окремо; тут лише безпечна відмова.
    if (patcherRun && cmd.Find(_L("e90")) != KErrNotFound)
        gUseE90Profile = ETrue;

    ProbeHal();
    if (e90scan)
        E90ScanReaders();
    if (usbcboot)
        UsbcBoot(cmd.Find(_L("e90")) != KErrNotFound);
    if (!quiet)
        {
        ListDrivers();
        ProbeDriver();
        }

    // TestPowerAPI лише на явну вимогу: "power <індекс>".
    TInt at = cmd.Find(_L("power"));
    if (at != KErrNotFound)
        {
        TLex lex(cmd.Mid(at + 5));
        lex.SkipSpace();
        TInt idx = -1;
        if (lex.Val(idx) == KErrNone && idx >= 0 && idx <= 12)
            ProbePowerApi(idx);
        else
            Log(_L8("--- TestPowerAPI: bad index, expected \"power <0..12>\" ---"));
        }
    else
        Log(_L8("--- TestPowerAPI skipped (pass \"power <0..12>\" to enable) ---"));

    if (!quiet)
        {
        ProbeDeviceCaps();
        ExperimentReserve();
        SampleUnderLoad();
        }
    // Прогони з патчером міряють самі, у парі "до/після" в межах одного запуску.
    if (!quiet)
        {
        MeasureAbsoluteClock();
        Benchmark();
        }

    // RomPatcher+ ніколи не вантажимо у звичайному прогоні. Команди окремі:
    //   "patcher selftest" - лише буфер цього процесу;
    //   "patcher read"     - self-test, потім READ-патч без запису PRCM.
    //   "patcher sram"     - self-test, READ-патч, ланцюг до SRAM-копії, SDRC.
    //   "patcher engine"       - безголова перевірка двигуна, лише читання;
    //   "patcher blobtest"     - виконання тестового блоба в режимі ядра;
    //   "patcher dsp [n]"      - регістри домену DSP, лише читання (n знімків);
    //   "patcher dspset <d>"   - змінити дільник DSP, повернути штатний на виході;
    //   "patcher dsphold <d>"  - те саме, але лишити (ребут усе одно поверне);
    //   "patcher oc <mult>"    - розгін, замір, витримка, повернення штатного.
    //   "patcher clocks mult=86 mpu=4 l3=4 dss=6 sec=300 [sample=10]
    //                   [child=ps1|dsp] [args:<аргументи дитини>]"
    //                          - повна розкладка клоків, витримка з наглядом,
    //                            повернення штатного стану. Аргументи дитини
    //                            йдуть ОСТАННІМИ, після маркера "args:".
    TInt lv = cmd.Find(_L("patcher level"));
    if (lv != KErrNotFound)
        {
        TLex lex(cmd.Mid(lv + 13));
        lex.SkipSpace();
        TInt n = -1;
        if (lex.Val(n) == KErrNone && n >= 0 && n <= 3)
            ProbeLevel(n);
        else
            Log(_L8("--- expected: patcher level <0..3> ---"));
        }
    else if (cmd.Find(_L("pmutest")) != KErrNotFound)
        ProbePmuTest();
    else if (cmd.Find(_L("blobtest")) != KErrNotFound)
        ProbeBlobTest();
    else if (cmd.Find(_L("patcher engine")) != KErrNotFound)
        ProbeEngine();
    else if (cmd.Find(_L("patcher clocks")) != KErrNotFound)
        {
        // Замовчування - рівно штатна розкладка: команда без жодного ключа
        // нічого не змінює, лише міряє й наглядає.
        TInt mult = KStockMult, mpu = 2, l3 = 4, dss = 6, sec = 30, sample = 10;
        KeyVal(cmd, _L("mult="), mult);
        KeyVal(cmd, _L("mpu="), mpu);
        KeyVal(cmd, _L("l3="), l3);
        KeyVal(cmd, _L("dss="), dss);
        KeyVal(cmd, _L("sec="), sec);
        KeyVal(cmd, _L("sample="), sample);
        // Стеля витримки та сама, що й у dspset: зависла дитина не має тримати
        // нештатну розкладку довше, ніж ми свідомо дозволили.
        if (sec < 0 || sec > 900)
            sec = 30;
        if (sample < 5 || sample > 60)
            sample = 10;
        TInt child = EChildNone;
        if (cmd.Find(_L("child=ps1")) != KErrNotFound)
            child = EChildPs1;
        else if (cmd.Find(_L("child=dsp")) != KErrNotFound)
            child = EChildDsp;
        // hold=1 вмикає переписування дільників при зсуві. Окремим ключем і
        // вимкнене за замовчуванням: цей блоб на апараті ще не виконувався.
        TInt hold = 0;
        KeyVal(cmd, _L("hold="), hold);
        RunClocks(mult, mpu, l3, dss, sec, child, childArgs, sample, hold != 0);
        }
    else if (cmd.Find(_L("patcher dspset")) != KErrNotFound ||
             cmd.Find(_L("patcher dsphold")) != KErrNotFound)
        {
        const TBool keep = (cmd.Find(_L("patcher dsphold")) != KErrNotFound);
        TInt at = cmd.Find(keep ? _L("patcher dsphold") : _L("patcher dspset"));
        TLex lex(cmd.Mid(at + (keep ? 15 : 14)));
        lex.SkipSpace();
        TInt n = -1;
        if (lex.Val(n) == KErrNone)
            {
            // Другий необовʼязковий аргумент - секунди витримки.
            lex.SkipSpace();
            TInt soak = 0;
            if (lex.Val(soak) != KErrNone || soak < 0 || soak > 900)
                soak = 3;
            TInt mode = ESoakPoll;
            if (cmd.Find(_L("quiet")) != KErrNotFound)
                mode = ESoakQuiet;
            else if (cmd.Find(_L("child")) != KErrNotFound)
                mode = ESoakRun;
            ProbeDspSet(n, keep, soak, mode);
            }
        else
            Log(_L8("--- expected: dspset|dsphold <div> [sec] [quiet|child] ---"));
        }
    else if (cmd.Find(_L("patcher dsp")) != KErrNotFound)
        {
        TInt at = cmd.Find(_L("patcher dsp"));
        TLex lex(cmd.Mid(at + 11));
        lex.SkipSpace();
        TInt n = 0;
        // Без аргументу - 12 знімків за 3 секунди: досить, щоб побачити
        // бімодальність, і мало, щоб не тримати апарат.
        if (lex.Val(n) != KErrNone || n < 1 || n > 60)
            n = 12;
        ProbeDsp(n);
        }
    else if (cmd.Find(_L("patcher hold")) != KErrNotFound)
        {
        TInt at = cmd.Find(_L("patcher hold"));
        TLex lex(cmd.Mid(at + 12));
        lex.SkipSpace();
        TInt mult = -1;
        if (lex.Val(mult) == KErrNone)
            HoldOverclock(mult);
        else
            Log(_L8("--- expected: patcher hold <multiplier> ---"));
        }
    else
        {
        TInt oc = cmd.Find(_L("patcher oc"));
        if (oc != KErrNotFound)
            {
            TLex lex(cmd.Mid(oc + 10));
            lex.SkipSpace();
            TInt mult = -1;
            if (lex.Val(mult) == KErrNone)
                RunOverclock(mult);
            else
                Log(_L8("--- expected: patcher oc <multiplier> ---"));
            }
        }

    Log(_L8("== end =="));
    gLog.Close();
    gFs.Close();
    }

TInt E32Main()
    {
    CTrapCleanup *tc = CTrapCleanup::New();
    if (!tc)
        return KErrNoMemory;
    TRAPD(err, RunL());
    delete tc;
    return err;
    }
