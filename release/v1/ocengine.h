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

// Регістри (віртуальні адреси; фізична = мінус 0x7E000000 для вікна 0x48xxxxxx).
const TUint32 KRegClkSel1Pll = 0xc6008540u;
const TUint32 KRegClkSel2Pll = 0xc6008544u;
const TUint32 KRegClkSelMpu  = 0xc6008140u;
const TUint32 KRegClkEnPll   = 0xc6008500u;
const TUint32 KRegIdlestPll  = 0xc6008520u;
const TUint32 KRegClkSel1Core= 0xc6008240u;
const TUint32 KRegSdrcRfr0   = 0xc61290a4u;
const TUint32 KRegSdrcActimA = 0xc612909cu;

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

    TInt ReadReg(TUint32 aAddr, TUint32& aOut);
    // Перечитує CM_CLKSEL1_PLL з апарата й повертає фактичний множник.
    TInt LiveMult(TInt& aOut);

    // Виконує relock. aStatus: 0 = чисто, біт0 = таймаут bypass (множник НЕ
    // записано), біт1 = таймаут CLKCFG, біт2 = таймаут lock.
    TInt ApplyMult(TInt aMult, TUint32& aStatus);
    // Той самий результат, але СХОДИНКАМИ по одному пункту множника, з паузою
    // на осідання між ними. Причина: крок +1 стабільний навіть із живим
    // дисплеєм, а +2 і більше вішає апарат (findings 20) - вікно bypass довше,
    // і контролер дисплея не переживає падіння L3. Один великий стрибок нічим
    // не кращий за кілька малих, тож робимо тільки малі.
    TInt ApplyMultRamp(TInt aTarget, TUint32& aStatus);
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
    TInt ReopenDvfs();
    TInt WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue);

private:
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
    TUint32  iStockCore;       // CM_CLKSEL1_CORE як воно було на старті
    TOcClocks iStockClocks;
    TOcStep  iStep;
    };

#endif
