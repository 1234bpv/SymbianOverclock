// Двигун розгону: спільний для консольного пробника (ocprobe) і для
// застосунку на апараті (ocgui). Тримати його в одному місці критично - тут
// лежить послідовність relock DPLL, і дві копії, що розійшлись, означали б
// зависання апарата.
//
// Повний розбір - docs/findings.md, розділи 17 і 18. Стисло, три примітиви:
//   1. READ  - патч тіла dvfs_interface DoControl(1) @0x80148410 у "ldr r0,[r1]";
//   2. EXEC  - той самий патч у "bx r1": виклик довільної адреси ядра;
//   3. SRAM  - плацдарм поза SDRAM: чанк, який power.dll мапить на буті.
//
// Усе рантаймове. Перезавантаження повертає апарат у первісний стан, у флеш не
// пишеться нічого.

#ifndef OCENGINE_H
#define OCENGINE_H

#include <e32std.h>
#include <e32base.h>

// DoCreate/DoControl у RBusLogicalChannel захищені, тож канали загортаємо.
// Ім'я каналу patcher - з дизасемблера самого драйвера, не з назви файлу.
class RPatcherChannel : public RBusLogicalChannel
    {
public:
    TInt Open();
    // func 105: a1 -> цільова адреса, a2 -> дескриптор із байтами.
    TInt WriteBytes(TUint32& aAddress, const TDesC8& aBytes);
    // func 100: a1 -> адреса, a2 -> довжина. Звільняє ROM-shadow сторінки на
    // діапазоні (0x848c -> ekern ординал 238 FreeShadowPage посторінково),
    // тобто повертає ділянці штатне відображення прямо на ROM.
    TInt FreeShadow(TUint32& aAddress, TUint32& aLength);
    };

class RDvfsChannel : public RBusLogicalChannel
    {
public:
    TInt Open();
    // Після READ-патчу - читання за адресою; після EXEC-патчу - виклик за нею.
    TInt ReadOrCall(TUint32 aAddress);
    };

// Межі повзунка. 129 - це рівно +50% від штатних 86; 43 - симетрично вниз.
// Штатне значення читається з апарата, а не припускається.
const TInt KOcMultMin = 43;
const TInt KOcMultMax = 129;

// ШТАТНИЙ стан клоків ЦЬОГО апарата - КОНСТАНТИ, а не те, що прочитано на
// старті процесу. Різниця принципова.
//
// Раніше опорну точку знімали з живих регістрів у Prepare(). Поки апарат
// штатний, різниці немає; але щойно він уже розігнаний, опорна точка з'їжджає, і
// CompensatedCore() починає цілитись у чужу частоту. Сусідній проєкт спіймав це
// на своїх логах: цикл "hold 107 -> hold 86" лишив dssdiv=5 замість 6, тобто
// піксельний клок 132.1 МГц замість штатних 110.08, і рядок "vs stock: DSS1
// -3.5%" при цьому звітував, що дисплей майже не зачеплено. Кожен наступний цикл
// зсував дільник ще на крок і ніколи не повертав - ратчет, непомітний з логів
// саме тому, що логи міряли зсув від уже зсунутої точки.
//
// Значення зняті з апарата після холодного завантаження
// (PS1Emulator/plans/runs/oc107_apply.log:15-17).
const TUint32 KStockClkSel1Core = 0x04360644u;   // l3div=4, dss1div=6
const TUint32 KStockClkSelMpu   = 0x00000002u;   // mpu_div=2
const TInt    KStockMult        = 86;            // DPLL 330.24 -> CORE 660.48

// Стратегія читання поточної частоти - залежить від того, що є в прошивці.
enum TOcReadStrategy
    {
    EReadDvfs = 0,     // патч тіла dvfs_interface DoControl(1): штатний шлях RM-320
    EReadPowerHelper,  // виклик helper'а читання регістра з power.dll (де dvfs нема)
    EReadMeasureOnly   // регістр не читаємо взагалі, міряємо роботу (найпереносніше)
    };

// Профіль апарата/прошивки: РІВНО ті значення, що змінюються між ROM. Регістри
// PRCM, маски полів і дозволені значення сюди НЕ входять - це сам OMAP2420,
// однаковий на всіх цих апаратах. Заповнюється офлайн із розпакованого z:
// (findings §29); числа беруться лише з ROM або з апарата, ніколи з голови.
struct TOcProfile
    {
    const char*     iId;              // ідентифікатор прошивки, напр. "RM-320 31.0.015"
    TOcReadStrategy iRead;
    TUint32         iDvfsBody;        // тіло DoControl(1); 0 якщо dvfs в прошивці нема
    TUint32         iRomSramRoutine;  // idle/relock рутина power.dll (для обходу DPLL)
    TUint32         iDspSetter;       // штатний сеттер дільника DSP у power.dll
    TUint32         iPwrDownGlobal;   // глобал power-down (резолв адреси SRAM)
    // Штатна розкладка тактування САМЕ цього апарата: панель різна, тож DSS/L3
    // між моделями відрізняються, і опорну точку не можна брати спільну.
    TUint32         iStockClkSel1Core;
    TUint32         iStockClkSelMpu;
    TInt            iStockMult;
    };

// Еталонний профіль, доведений на апараті (N95 8GB RM-320, прошивка 31.0.015).
extern const TOcProfile KProfileRM320;

// Регістри (віртуальні адреси; фізична = мінус 0x7E000000 для вікна 0x48xxxxxx).
const TUint32 KRegClkSel1Pll = 0xc6008540u;
const TUint32 KRegClkSel2Pll = 0xc6008544u;
const TUint32 KRegClkSelMpu  = 0xc6008140u;
const TUint32 KRegClkEnPll   = 0xc6008500u;
const TUint32 KRegIdlestPll  = 0xc6008520u;
const TUint32 KRegClkSel1Core= 0xc6008240u;
const TUint32 KRegSdrcRfr0   = 0xc61290a4u;
const TUint32 KRegSdrcActimA = 0xc612909cu;

// Домен DSP (C55x). Та сама 4-КБ сторінка PRCM, що й регістри вище, тож
// READ-патч дістає їх без жодних змін у механіці. Адреса CM_CLKSEL_DSP не
// здогад: рівно її вантажить power.dll @0x800B16F4, коли рахує частоту DSP.
const TUint32 KRegFclkenDsp  = 0xc6008800u;   // CM_FCLKEN_DSP
const TUint32 KRegIclkenDsp  = 0xc6008810u;   // CM_ICLKEN_DSP
const TUint32 KRegIdlestDsp  = 0xc6008820u;   // CM_IDLEST_DSP
const TUint32 KRegClkSelDsp  = 0xc6008840u;   // CM_CLKSEL_DSP

// Поля CM_CLKSEL_DSP. Виведено з сеттерів ehwaomap.pdd @0x801AE900 і
// @0x801AE960 (кожен - bic/orr/str у [r0,#0x840]) і збігається з розкладкою
// clksel для OMAP2420 у ядрі Linux - два незалежні джерела.
//   [3:0]   дільник, з якого САМА прошивка рахує частоту DSP: power.dll
//           ord60 @0x800B1644 маскує рівно 0xF і ділить на нього CORE.
//           Даташит дає полю 5 бітів [4:0]; сеттер PDD його не чіпає взагалі,
//           тобто в рантаймі прошивка низьке поле не змінює.
//   [6:5]   CLKSEL_DSP_IF - інтерфейсний клок, похідний від дільника DSP.
//   [7]     поле 0 сеттера, призначення невідоме.
//   [12:8]  CLKSEL_IVA.
//   [13]    поле другого сеттера, призначення невідоме.
const TUint32 KDspDivMask   = 0x0000001fu;   // [4:0] за даташитом
const TUint32 KDspFwDivMask = 0x0000000fu;   // [3:0] - стільки бачить ord60
const TUint32 KDspIfMask    = 0x00000060u;   // [6:5]
const TUint32 KDspBit7Mask  = 0x00000080u;   // [7]
const TUint32 KDspIvaMask   = 0x00001f00u;   // [12:8]
const TUint32 KDspBit13Mask = 0x00002000u;   // [13]

// ШТАТНИЙ сеттер дільника DSP у power.dll. Знайдено так: літерал 0xC6008840
// зустрічається в усьому ROM рівно двічі, обидва рази в power.dll, і одне з
// входжень - у пулі констант поряд із CM_CLKSEL_MPU і CM_CLKSEL1_CORE, тобто в
// зоні валідованих сеттерів PRCM (@0x800AC410..0x800AC698). Тіло:
//   cmp r1,#9 / #16 / #18 / #0 / #5 -> mvn r0,#5 (KErrArgument)
//   інакше Modify32(0xC6008840, mask 0x1F, r1); return 0
const TUint32 KDspSetter = 0x800ac59cu;
// Значення, які прошивка САМА вважає дійсними: усе 1..31, крім 0, 5, 9, 16, 18.
// Це не наш вибір - це перелік із валідації сеттера.
// = ~((1<<0)|(1<<5)|(1<<9)|(1<<16)|(1<<18))
const TUint32 KDspDivAllowed = 0xfffafddeu;
TInt DspDivStep(TInt aValue, TInt aDir);

// Розклад тактування, розібраний із регістрів. Формула - з коду power.dll:
//   DPLL = sys_clk * mult / (div + 1)      @0x800B1544
//   CORE = DPLL * CM_CLKSEL2_PLL[1:0]      @0x800B15B0
//   MPU  = CORE / CM_CLKSEL_MPU[3:0]       @0x800B15DC
//   L3   = CORE / CM_CLKSEL1_CORE[4:0]     (звідси ж тактується SDRAM)
// Поля CM_CLKSEL1_CORE. Виведено з сеттерів power.dll @0x800AC458..0x800AC598:
// кожен валідує значення, зсуває і кличе Modify32 із власною маскою. Розклад
// збігається з заголовками Linux для OMAP2420 - два незалежні джерела.
const TUint32 KCoreL3Mask   = 0x0000001fu;   // [4:0]
const TUint32 KCoreL4Mask   = 0x00000060u;   // [6:5]
const TUint32 KCoreDss1Mask = 0x00001f00u;   // [12:8]
const TInt    KCoreDss1Shift = 8;
const TInt    KCoreL3Shift   = 0;

// Усі поля CM_CLKSEL1_CORE з ДОПУСТИМИМИ значеннями, взятими з валідації в
// сеттерах power.dll @0x800AC458..0x800AC598 (findings §21). Дозволені значення
// - це не наша вигадка, а те, що прошивка сама вважає дійсним для цього SoC,
// тож вони й служать поручнями для ручного режиму.
enum TCoreFieldId
    {
    ECoreL3 = 0,      // [4:0]   L3 = CORE / n   (на L3 сидить контролер SDRAM)
    ECoreL4,          // [6:5]   L4 = L3 / n     (повільна периферійна шина)
    ECoreDss1,        // [12:8]  DSS1 = CORE / n (дисплей)
    ECoreDss2,        // [13]    селектор джерела DSS2, не дільник
    ECoreVlynq,       // [19:15] VLYNQ = CORE / n
    ECoreSsi,         // [24:20] SSI = CORE / n  (у нокіївських OMAP - канал до модема)
    ECoreUsb,         // [27:25] USB; джерело не 100% встановлене, тож лише сире значення
    ECoreFieldCount
    };

struct TCoreField
    {
    const char* iName;
    TUint32     iMask;
    TInt        iShift;
    TUint32     iAllowed;    // бітова маска дозволених значень 0..31
    TInt        iBase;       // 0 = CORE, 1 = L3 (для L4), -1 = не дільник
    };

// Дільник MPU живе в ОКРЕМОМУ регістрі CM_CLKSEL_MPU, не в CLKSEL1_CORE.
// Поле [4:0] (маска 0x1F у сеттері power.dll @0x800AC410), а допустимі значення
// - 1, 2, 4, 6, 8: таблиця переходів @0x800AC3D0 відкидає 0, 3, 5, 7. Тобто
// дільника 3 на цьому SoC не існує, хоч поле його й вмістило б.
const TUint32 KMpuDivMask = 0x0000001fu;
const TUint32 KMpuAllowed = (1u<<1)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8);
TInt MpuDivStep(TInt aValue, TInt aDir);

// Вужчі набори дільників для АВТОМАТИЧНОГО підбору. Причина - у коментарі при
// визначенні: апаратний набір вужчий за той, що валідує прошивка, і недійсне
// значення в ОДНОМУ полі відкидає ВЕСЬ запис у CM_CLKSEL1_CORE.
extern const TUint32 KAutoDivAllowed;   // DSS1: 1, 2, 4, 5, 6, 8, 12, 16
extern const TUint32 KAutoL3Allowed;    // L3: те саме без 5 (валідатор його відкидає)

const TCoreField& CoreField(TInt aId);
TInt CoreFieldValue(TUint32 aCoreReg, TInt aId);
TUint32 CoreFieldSet(TUint32 aCoreReg, TInt aId, TInt aValue);
// Наступне/попереднє ДОПУСТИМЕ значення поля. Повертає старе, якщо далі нема.
TInt CoreFieldStep(TInt aId, TInt aValue, TInt aDir);

class TOcClocks
    {
public:
    void Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv, TUint32 aCoreDiv);
    TUint32 DpllKhz() const;
    TUint32 CoreKhz() const;
    TUint32 MpuKhz() const;
    TUint32 L3Khz() const;

    TUint32 iSysKhz;      // 19200, 13000 або 12000 - за полем [25:23]
    TInt    iMult;        // [21:12]
    TInt    iDiv;         // [11:8], ділиться на (div+1)
    TInt    iCoreSel;     // CM_CLKSEL2_PLL[1:0]
    TInt    iMpuDiv;      // CM_CLKSEL_MPU[3:0]
    TInt    iL3Div;       // CM_CLKSEL1_CORE[4:0]
    TInt    iDss1Div;     // CM_CLKSEL1_CORE[12:8]
    TUint32 Dss1Khz() const;
    };

// Коди етапів підготовки: дають змістовну діагностику замість "не вийшло".
enum TOcStep
    {
    EOcStepIdle = 0,
    EOcStepLoadDriver,        // User::LoadLogicalDevice(patcher.ldd)
    EOcStepOpenPatcher,       // DoCreate("RomPatcherPlus.Driver")
    EOcStepSelfTest,          // func 105 у власний буфер
    EOcStepReadPatch,         // накладання READ-патчу
    EOcStepOpenDvfs,          // DoCreate("dvfs_interface")
    EOcStepResolveSram,       // ланцюг до лінійної адреси SRAM-копії
    EOcStepVerifySram,        // звірка копії з ROM-оригіналом
    EOcStepReadRegs,
    EOcStepReady
    };

class COcEngine : public CBase
    {
public:
    static COcEngine* NewL();
    ~COcEngine();

    // Робить усе до готовності. KErrNone = можна розганяти.
    TInt Prepare();
    TBool IsReady() const { return iReady; }
    TOcStep FailedAt() const { return iStep; }
    static const TDesC& StepName(TOcStep aStep);

    TUint32 SramAddress() const { return iSram; }
    TInt StockMult() const { return iStockClocks.iMult; }
    const TOcClocks& StockClocks() const { return iStockClocks; }
    // Розклад для гіпотетичного множника - для показу "що буде", до застосування.
    void ClocksFor(TInt aMult, TOcClocks& aOut) const;
    // Підбирає дільники L3 і DSS1 так, щоб їхні частоти лишились якнайближче
    // до штатних при новій CORE. Повертає готове значення CM_CLKSEL1_CORE.
    TUint32 CompensatedCore(TInt aMult) const;
    TUint32 StockCore() const { return iStockCore; }
    TUint32 StockMpu() const { return iStockMpu; }

    // Стан, у якому Prepare() ЗАСТАВ апарат. Опорною точкою він більше не
    // служить (див. KStockClkSel1Core), але мовчати про нього не можна: розбіжність
    // із штатним означає, що попередній прогін лишив по собі нештатну розкладку.
    TUint32 BootCore() const { return iBootCore; }
    TUint32 BootMpu() const { return iBootMpu; }
    TInt BootMult() const { return iBootMult; }
    TBool BootedNonStock() const
        { return iBootMult != KStockMult || iBootCore != KStockClkSel1Core ||
                 (iBootMpu & KMpuDivMask) != (KStockClkSelMpu & KMpuDivMask); }

    TInt ReadReg(TUint32 aAddr, TUint32& aOut);
    // Кілька адрес за одне накладання READ-патчу. Менше вікон, у які може
    // влетіти чужий DoControl(1), - див. коментар при реалізації.
    TInt ReadRegs(const TUint32* aAddr, TUint32* aOut, TInt aCount);

    // --- домен DSP (C55x) ---------------------------------------------------
    // Дільник міняється викликом ШТАТНОГО сеттера power.dll через EXEC-патч, а
    // не власним записом у MMIO: сеттер сам валідує значення і сам зберігає
    // решту регістра. Ні CORE, ні SDRAM, ні дисплей це не зачіпає.
    // aSetterRc - результат самого сеттера: 0 або KErrArgument.
    TInt ApplyDspDiv(TInt aDiv, TInt& aSetterRc);
    TInt RestoreDspDiv(TInt& aSetterRc);
    TInt LiveDspDiv(TInt& aOut);
    TInt StockDspDiv() const { return iStockDspDiv; }
    TInt DspDiv() const { return iDspDiv; }
    TBool IsDspChanged() const { return iDspDiv != iStockDspDiv; }
    // Частота DSP за формулою самої прошивки: CORE / дільник.
    TUint32 DspKhz(TInt aDiv) const;
    // Перечитує CM_CLKSEL1_PLL з апарата й повертає фактичний множник.
    TInt LiveMult(TInt& aOut);

    // Виконує relock. aStatus: 0 = чисто, біт0 = таймаут bypass (множник НЕ
    // записано), біт1 = таймаут CLKCFG, біт2 = таймаут lock.
    TInt ApplyMult(TInt aMult, TUint32& aStatus);
    // Ручний режим: множник І дільники задає викликач. Автопідбір не працює.
    // aMpuReg == 0 -> лишити штатний дільник MPU.
    TInt ApplyMultAndCore(TInt aMult, TUint32 aCoreReg, TUint32 aMpuReg,
                          TUint32& aStatus);
    // Той самий результат, але СХОДИНКАМИ по одному пункту множника, з паузою
    // на осідання між ними. Причина: крок +1 стабільний навіть із живим
    // дисплеєм, а +2 і більше вішає апарат (findings 20) - вікно bypass довше,
    // і контролер дисплея не переживає падіння L3. Один великий стрибок нічим
    // не кращий за кілька малих, тож робимо тільки малі.
    TInt ApplyMultRamp(TInt aTarget, TUint32& aStatus);
    // Те саме, але з явними дільниками на кожній сходинці.
    TInt ApplyRampWithCore(TInt aTarget, TUint32 aCoreReg, TUint32 aMpuReg,
                           TUint32& aStatus);
    // Переписати ЛИШЕ дільники (CM_CLKSEL1_CORE і CM_CLKSEL_MPU) із фіксацією
    // латчем PRCM_CLKCFG_CTRL, не зупиняючи DPLL. Потрібне, щоб утримати
    // розкладку, поки в тому ж вікні працює вимірюваний процес: повний relock
    // посеред такого прогону небезпечний (findings §20), а це - та сама
    // операція, яку сам губернатор робить при кожному переході OPP.
    // aRc: 0 = латч звільнився, 1 = таймаут.
    TInt ReassertDividers(TUint32 aCoreReg, TUint32 aMpuReg, TInt& aRc);
    // Виконує довільний блоб ARM-коду в режимі ядра через слот SRAM.
    // aBlob - готові байти коду, до 2048 байтів. aResult - те, що блоб
    // повернув у r0.
    TInt ExecCustomBlob(const TDesC8 &aBlob, TInt &aResult);
    TInt RestoreStock(TUint32& aStatus);
    // Повертає підмінене тіло DoControl(1) у первісний вигляд. Викликається з
    // деструктора, але явний виклик перед виходом надійніший.
    TInt RestoreKernelCode();
    // Повертає відсоток від номіналу зі ШТАТНОЇ функції. Ненульове значення
    // означає, що відкат ліг і в ядрі знову оригінальний код.
    TInt VerifyRestored();
    // Звільняє shadow-сторінку, створену патчером під тіло DoControl(1).
    // Без цього кожен прогін лишає в ядрі назавжди виділену RAM-сторінку
    // поверх сторінки коду ROM.
    TInt UnshadowKernelPage();
    // Повне зняття патчу: байти назад + unshadow. Саме unshadow робить це
    // надійним, бо перезапис байтів не інвалідовує I-cache.
    TInt EndPatchSession();

    // Бісекція причини перезавантажень. Кожен рівень лишає по собі рівно на
    // одну річ більше за попередній:
    //   0 - завантажений patcher.ldd, канал відкрито й закрито;
    //   1 - + запис func 105 у ВЛАСНИЙ буфер (ядро не змінюється);
    //   2 - + READ-патч, одне читання, відкат із перевіркою (ROM-shadow сторінка);
    //   3 - повний Prepare (усе, як у робочому прогоні).
    // Перший рівень, після якого апарат падає в простої, і вказує на винуватця.
    TInt SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue);
    TBool IsOverclocked() const { return iApplied; }
    // За замовчуванням деструктор повертає штатну частоту: процес помирає, а
    // частота лишається, і тихо віддати апарат розігнаним було б негідно.
    // Але коли розгін застосовують САМЕ щоб потім користуватися апаратом, це
    // треба вимкнути явно. Патч ядра при цьому знімається все одно - він
    // небезпечний для чужих викликів (findings §20), а частота ні.
    void KeepOnExit(TBool aKeep) { iKeep = aKeep; }
    TInt AppliedMult() const { return iAppliedMult; }

private:
    COcEngine();
    void ConstructL();
    TInt Poke(TUint32 aAddr, const TDesC8& aBytes);
    TInt PokeWords(TUint32 aAddr, const TUint32* aWords, TInt aCount);
    TInt ApplyReadPatch();
    TInt ApplyExecPatch();
    // Тіло Prepare ПІСЛЯ накладання READ-патча. Винесене окремо навмисно: так
    // Prepare знімає патч (EndPatchSession) на КОЖНОМУ виході з проби, і успіх,
    // і будь-яка помилка. Інакше аварійний вихід лишав би пропатчену функцію
    // ядра назавжди - смертельно на апараті з іншими адресами (findings §27).
    TInt PrepareProbe();
    TInt ReopenDvfs();
    TInt WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue,
                         TUint32 aMpuValue);
    TInt WriteDspBlob(TInt aDiv);
    TInt WriteDivBlob(TUint32 aCoreReg, TUint32 aMpuReg);

private:
    // Активний профіль апарата. Усі залежні від прошивки адреси беруться звідси,
    // а не з констант, тож перенесення на інший ROM - це заповнення профілю, а
    // не правка коду (findings §29). Поки що завжди &KProfileRM320.
    const TOcProfile* iProfile;
    RPatcherChannel iPatcher;
    RDvfsChannel    iDvfs;
    TBool    iPatcherOpen;
    TBool    iDvfsOpen;
    TBool    iReady;
    TBool    iExecMode;        // який патч зараз накладено
    TBool    iPatched;         // чи підмінене зараз тіло DoControl(1)
    TBool    iShadowed;        // чи створена патчером ROM-shadow сторінка
    TBool    iApplied;
    TBool    iKeep;            // не повертати частоту в деструкторі
    TInt     iAppliedMult;
    TUint32  iSram;
    TUint32  iStockClkSel1;
    TUint32  iStockCore;       // ШТАТНЕ CM_CLKSEL1_CORE (константа)
    TUint32  iStockMpu;        // ШТАТНЕ CM_CLKSEL_MPU (константа)
    TUint32  iBootCore;        // те, що реально стояло на старті процесу
    TUint32  iBootMpu;
    TInt     iBootMult;
    TInt     iStockDspDiv;     // CM_CLKSEL_DSP[3:0] на старті
    TInt     iDspDiv;          // те, що стоїть зараз
    TOcClocks iStockClocks;
    TOcStep  iStep;
    };

#endif
