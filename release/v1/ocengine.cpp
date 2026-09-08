// Реалізація двигуна розгону. Див. ocengine.h і docs/findings.md 17-18.

#include "ocengine.h"
#include <e32svr.h>

_LIT(KPatcherFile, "patcher.ldd");
_LIT(KPatcherName, "RomPatcherPlus.Driver");
_LIT(KDvfsFile, "dvfs_interface.ldd");
_LIT(KDvfsName, "dvfs_interface");

// Тіло dvfs_interface DoControl(1): рівно 6 слів, 0x80148410..0x80148427.
// Диспетчер починається з 0x80148428 і лишається недоторканим.
const TUint32 KDvfsBody = 0x80148410u;
// Глобал power.dll: [+4] -> об'єкт power-down, [obj+16] -> лінійна адреса SRAM.
const TUint32 KPwrDownGlobal = 0xc80030c8u;
// Джерело копії в ROM: за ним звіряємо, що знайшли саме виконувану копію.
const TUint32 KRomSramRoutine = 0x800a7f6cu;
// Firmware займає перші 1024 байти чанка; пишемо зі зсувом 2048 з 4096.
const TUint32 KSramSlot = 0x800u;

// --- код, що виконується з SRAM ---------------------------------------------
//
// Вимоги жорсткі й неочевидні:
//   * жодного зовнішнього bl - після копіювання відносні переходи вели б у
//     нікуди. Константи - з власного пулу (.ltorg у кінці), тож ldr [pc,#off]
//     лишається коректним на будь-якій адресі;
//   * жодного звернення до SDRAM у критичному вікні: поки DPLL у bypass,
//     контролер SDRAM тактується мізерною частотою і refresh голодує. Стек
//     чіпаємо лише до і після вікна;
//   * EN_DPLL - це БІТИ [1:0] CM_CLKEN_PLL, а не [2:0]. Біти [3:2] - APLL
//     54 МГц; маска #7 вимкнула б його разом із DPLL. Маску #3 узято з коду
//     самої power.dll (and r2,r2,#0xfc);
//   * усі очікування обмежені лічильником: переривання вимкнені, тож
//     нескінченний цикл означав би виймання батареї.
//
// Порядок - за omap242x_sram_set_prcm і за idle-процедурою power.dll @0x800A7F6C.
__asm__(
"   .text                       \n"
"   .align 2                    \n"
"   .code 32                    \n"
"   .global OcReadBlobStart     \n"
"   .global OcReadBlobEnd       \n"
"OcReadBlobStart:               \n"
"   mrs  r1, cpsr               \n"
"   orr  r2, r1, #0xC0          \n"
"   msr  cpsr_c, r2             \n"
"   ldr  r3, =0xC6008000        \n"
"   ldr  r0, [r3, #0x540]       \n"
"   msr  cpsr_c, r1             \n"
"   bx   lr                     \n"
"   .ltorg                      \n"
"OcReadBlobEnd:                 \n"
"                               \n"
// EXEC-патч тіла DoControl(1). Критично: канал dvfs_interface відкриваємо не
// лише ми (§15 - mmrmserver.dll, fmradiotunercontrol.dll), і в застосунку з
// живим інтерфейсом ці клієнти активні. Простий "bx r1" означав би, що будь-який
// їхній виклик DoControl(1, a1) стрибає в ядрі за ЧУЖОЮ адресою - звідси
// зависання, і тим певніше, чим більше таких вікон ми відкриваємо.
// Тому стрибаємо ТІЛЬКИ на одну очікувану адресу (наш слот у SRAM), а всім
// іншим повертаємо 0 - безневинно, як і READ-патч на a1=0.
// Рівно 6 слів = 24 байти, тобто точний розмір оригінального тіла.
"   .global OcExecPatchStart    \n"
"   .global OcExecPatchEnd      \n"
"OcExecPatchStart:              \n"
"   ldr  r0, [pc, #12]          \n"   // -> літерал нижче
"   cmp  r1, r0                 \n"
"   bxeq r1                     \n"
"   mov  r0, #0                 \n"
"   bx   lr                     \n"
"   .word 0xDEADBEEF            \n"   // дозволена адреса; патчиться зовні
"OcExecPatchEnd:                \n"
"                               \n"
"   .global OcRelockBlobStart   \n"
"   .global OcRelockBlobEnd     \n"
"OcRelockBlobStart:             \n"
"   stmfd sp!, {r4-r11, lr}     \n"
"   ldr  r4, =0xC6008000        \n"
"   ldr  r5, =0xC6129000        \n"
"   ldr  r6, =0xDEADBEEF        \n"   // новий CM_CLKSEL1_PLL; патчиться зовні
"   ldr  r11, =0xFEEDFACE       \n"   // початковий, для апаратного відкату
"   ldr  r2, =0xCAFEBABE        \n"   // новий CM_CLKSEL1_CORE (дільники)
"   ldr  r3, =0xBAADF00D        \n"   // початковий CM_CLKSEL1_CORE
"   mov  r7, #0                 \n"
"   mrs  r8, cpsr               \n"
"   orr  r9, r8, #0xC0          \n"
"   msr  cpsr_c, r9             \n"
// SDRAM у self-refresh ДО вікна. Без цього кроку контролер памʼяті лишається
// в звичайному режимі, поки CORE (а з ним L3 і SDRAM) падає до sys_clk на весь
// час bypass+relock. Безголовий прогін це переживав, бо трафіку до памʼяті
// майже не було; із живим дисплеєм, який безперервно читає кадровий буфер,
// апарат зависав намертво, і то тим певніше, чим більший крок множника (довше
// захоплення DPLL = довше вікно). Значення 0x48 і саме це місце в
// послідовності - з idle-процедури power.dll @0x800A7F6C (findings 17), тобто
// вендорський код для цього ж SoC.
"   ldr  r0, [r5, #0x70]        \n"
"   orr  r0, r0, #0x48          \n"
"   str  r0, [r5, #0x70]        \n"
"   ldr  r0, [r5, #0x70]        \n"   // читання назад: запис мусить дійти
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #2             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"1: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #1                 \n"
"   beq  2f                     \n"
"   subs r10, r10, #1           \n"
"   bne  1b                     \n"
"   orr  r7, r7, #1             \n"
"   b    3f                     \n"
"2: str  r6, [r4, #0x540]       \n"
// Дільники домену CORE пишемо ТУТ, разом із множником: одна фіксація
// CLKCFG_CTRL латчить і PLL, і CLKSEL1_CORE, тож L3/SDRAM і DSS ніколи не
// бачать нову CORE зі старим дільником. Саме цього бракувало: дисплей
// тактується CORE/DSS1_div, і будь-яка зміна CORE без компенсації дільника
// зсувала його піксельний клок - звідси зависання відрисовки, причому
// симетрично, і при пониженні частоти теж (findings §21).
"   str  r2, [r4, #0x240]       \n"
"   mov  r0, #1                 \n"
"   str  r0, [r4, #0x80]        \n"
"   mov  r10, #0x10000          \n"
"4: ldr  r0, [r4, #0x80]        \n"
"   tst  r0, #1                 \n"
"   beq  3f                     \n"
"   subs r10, r10, #1           \n"
"   bne  4b                     \n"
"   orr  r7, r7, #2             \n"
"3: ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #3             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"5: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #2                 \n"
"   beq  6f                     \n"
"   subs r10, r10, #1           \n"
"   bne  5b                     \n"
"   orr  r7, r7, #4             \n"
// АПАРАТНИЙ ВІДКАТ. Якщо DPLL не захопився на новому множнику, лишити його в
// bypass не можна: CORE тоді дорівнює sys_clk, MPU падає до ~9.6 МГц, і апарат
// виглядає як намертво зависший, хоча формально живий. Повертаємось у bypass,
// пишемо ПОЧАТКОВИЙ множник і захоплюємось на ньому - усе всередині тієї самої
// критичної секції, поки переривання ще вимкнені.
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #2             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"8: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #1                 \n"
"   beq  9f                     \n"
"   subs r10, r10, #1           \n"
"   bne  8b                     \n"
"9: str  r11, [r4, #0x540]      \n"   // початковий CM_CLKSEL1_PLL
"   str  r3, [r4, #0x240]       \n"   // і початкові дільники
"   mov  r0, #1                 \n"
"   str  r0, [r4, #0x80]        \n"
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #3             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"0: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #2                 \n"
"   beq  6f                     \n"
"   subs r10, r10, #1           \n"
"   bne  0b                     \n"
"   orr  r7, r7, #8             \n"   // відкат теж не вдався
"6: ldr  r0, [r5, #0x64]        \n"
"   and  r0, r0, #0xFF00        \n"
"   ldr  r1, [r5, #0x60]        \n"
"   and  r1, r1, #0x0F          \n"
"   orr  r1, r1, #4             \n"
"   orr  r0, r0, r1             \n"
"   str  r0, [r5, #0x60]        \n"
"   ldr  r0, [r5, #0x6C]        \n"
"   and  r0, r0, #0xFF00        \n"
"   ldr  r1, [r5, #0x68]        \n"
"   and  r1, r1, #0x0F          \n"
"   orr  r1, r1, #4             \n"
"   orr  r0, r0, r1             \n"
"   str  r0, [r5, #0x68]        \n"
"   mov  r10, #0x400            \n"
"7: subs r10, r10, #1           \n"
"   bne  7b                     \n"
"   msr  cpsr_c, r8             \n"
"   ldr  r0, [r4, #0x540]       \n"
"   bic  r0, r0, #0xF0000000    \n"
"   orr  r0, r0, r7, lsl #28    \n"
"   ldmfd sp!, {r4-r11, pc}     \n"
"   .ltorg                      \n"
"OcRelockBlobEnd:               \n"
);

extern "C" void OcRelockBlobStart(void);
extern "C" void OcRelockBlobEnd(void);
extern "C" void OcExecPatchStart(void);
extern "C" void OcExecPatchEnd(void);

// Оригінальні 6 слів тіла DoControl(1), зняті з дизасемблера ROM
// (out/asm/dvfs_if.asm, 0x80148410..0x80148427). Тримати їх тут обов'язково:
// без відкату наш патч лишається в ROM-shadow ЖИТИ ПІСЛЯ ВИХОДУ процесу, тобто
// в системі назавжди (до перезавантаження) висить підмінена функція ядра. Якщо
// її колись викличе не наш код і не з нулем у a1, ми розіменуємо довільну
// адресу в режимі ядра - апарат перезавантажиться.
static const TUint32 KDvfsOriginal[6] =
    { 0xe92d4010u,   // stmdb sp!, {r4, lr}
      0xe3a00001u,   // mov   r0, #1
      0xeb00014cu,   // bl    0x80148950
      0xe59f11d0u,   // ldr   r1, [pc, #464]
      0xe8bd4010u,   // ldmia sp!, {r4, lr}
      0xea000135u }; // b     0x80148900

// READ-патч: cmp r1,#0 ; ldrne r0,[r1] ; moveq r0,#0 ; bx lr
static const TUint32 KReadPatch[4] =
    { 0xe3510000u, 0x15910000u, 0x03a00000u, 0xe12fff1eu };


// --- канали -----------------------------------------------------------------

TInt RPatcherChannel::Open()
    {
    return DoCreate(KPatcherName, TVersion(0,0,0), KNullUnit, NULL, NULL,
                    EOwnerProcess);
    }

TInt RPatcherChannel::WriteBytes(TUint32& aAddress, const TDesC8& aBytes)
    {
    return DoControl(105, (TAny*)&aAddress, (TAny*)&aBytes);
    }

TInt RPatcherChannel::FreeShadow(TUint32& aAddress, TUint32& aLength)
    {
    return DoControl(100, (TAny*)&aAddress, (TAny*)&aLength);
    }

TInt RDvfsChannel::Open()
    {
    return DoCreate(KDvfsName, TVersion(0,0,0), KNullUnit, NULL, NULL,
                    EOwnerProcess);
    }

TInt RDvfsChannel::ReadOrCall(TUint32 aAddress)
    {
    return DoControl(1, (TAny*)aAddress);
    }

// --- розклад тактування -----------------------------------------------------

void TOcClocks::Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv,
                       TUint32 aCoreDiv)
    {
    TUint32 sysSel = (aClkSel1 >> 23) & 7;
    iSysKhz = (sysSel == 0) ? 19200u : ((sysSel == 2) ? 13000u : 12000u);
    iMult = (TInt)((aClkSel1 >> 12) & 0x3ff);
    iDiv = (TInt)((aClkSel1 >> 8) & 0xf);
    iCoreSel = (TInt)(aClkSel2 & 3);
    iMpuDiv = (TInt)(aMpuDiv & 0xf);
    iL3Div = (TInt)(aCoreDiv & 0x1f);
    iDss1Div = (TInt)((aCoreDiv & KCoreDss1Mask) >> KCoreDss1Shift);
    }

TUint32 TOcClocks::Dss1Khz() const
    {
    return iDss1Div ? CoreKhz() / (TUint32)iDss1Div : 0;
    }

TUint32 TOcClocks::DpllKhz() const
    {
    return (iDiv + 1) ? (iSysKhz * (TUint32)iMult) / (TUint32)(iDiv + 1) : 0;
    }

TUint32 TOcClocks::CoreKhz() const { return DpllKhz() * (TUint32)iCoreSel; }

TUint32 TOcClocks::MpuKhz() const
    {
    return iMpuDiv ? CoreKhz() / (TUint32)iMpuDiv : 0;
    }

TUint32 TOcClocks::L3Khz() const
    {
    return iL3Div ? CoreKhz() / (TUint32)iL3Div : 0;
    }

// --- двигун -----------------------------------------------------------------

COcEngine::COcEngine()
  : iPatcherOpen(EFalse), iDvfsOpen(EFalse), iReady(EFalse), iExecMode(EFalse), iPatched(EFalse), iShadowed(EFalse),
    iApplied(EFalse), iKeep(EFalse), iAppliedMult(0), iSram(0), iStockClkSel1(0), iStockCore(0),
    iStep(EOcStepIdle)
    {
    }

COcEngine* COcEngine::NewL()
    {
    COcEngine* self = new (ELeave) COcEngine();
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

void COcEngine::ConstructL() {}

COcEngine::~COcEngine()
    {
    // Порядок обов'язковий і саме такий:
    //   1. зняти розгін - частота переживає процес;
    //   2. повернути оригінальні байти ядра - патч теж переживає процес, і
    //      лишати підмінену функцію в системі означає ставити міну на потім;
    //   3. лише тоді закривати канали (без патчера крок 2 неможливий).
    if (iApplied && !iKeep && iSram)
        {
        TUint32 st = 0;
        RestoreStock(st);
        }
    RestoreKernelCode();
    // Останнім - зняти shadow. Після цього в ядрі не лишається ні наших
    // байтів, ні виділеної під них сторінки.
    UnshadowKernelPage();
    if (iDvfsOpen)
        iDvfs.Close();
    if (iPatcherOpen)
        iPatcher.Close();
    }

// Повертає тіло DoControl(1) у первісний вигляд. Безпечно викликати повторно.
//
// Пишемо всі 6 слів: EXEC-патч займає рівно стільки. Раніше тут стояло 4 слова
// з обґрунтуванням "16 байтів - перевірений шлях memmove"; те обґрунтування було
// хибним діагнозом - справжньою причиною невдалого відкату був I-cache, а не
// довжина запису (findings §19).
TInt COcEngine::RestoreKernelCode()
    {
    if (!iPatcherOpen || !iPatched)
        return KErrNone;
    TInt r = PokeWords(KDvfsBody, KDvfsOriginal, 6);
    if (r == KErrNone)
        {
        iPatched = EFalse;
        iExecMode = EFalse;
        }
    return r;
    }

// Знімає ROM-shadow із тіла DoControl(1): сторінці повертається штатне
// відображення прямо на ROM, а виділена патчером RAM-сторінка звільняється.
// Робиться ОДИН раз, при завершенні: вміст ми й так відновлюємо після кожної
// операції, а от alloc/free сторінок ядра на кожне читання - зайва churn.
TInt COcEngine::UnshadowKernelPage()
    {
    if (!iPatcherOpen || !iShadowed)
        return KErrNone;
    TUint32 addr = KDvfsBody;
    TUint32 len = 16;
    TInt r = iPatcher.FreeShadow(addr, len);
    if (r == KErrNone)
        iShadowed = EFalse;
    return r;
    }

// Перевірка відкату. Прочитати відновлені байти нічим - примітив читання це
// і Є ті байти. Але в оригінальної функції є відома поведінка: DoControl(1, 0)
// віддає відсоток від номіналу (findings 3), тобто 50 або 100. Наш READ-патч
// на a1=0 повертає рівно 0, а EXEC-патч стрибнув би за нульовою адресою.
// Отже ненульовий відсоток тут - доказ, що в ядрі знову штатний код.
TInt COcEngine::VerifyRestored()
    {
    if (!iDvfsOpen || iPatched)
        return KErrNotReady;
    return iDvfs.ReadOrCall(0);
    }

_LIT(KStepIdle,        "idle");
_LIT(KStepLoadDriver,  "loading patcher.ldd");
_LIT(KStepOpenPatcher, "opening RomPatcherPlus.Driver");
_LIT(KStepSelfTest,    "patcher self-test");
_LIT(KStepReadPatch,   "applying READ patch");
_LIT(KStepOpenDvfs,    "opening dvfs_interface");
_LIT(KStepResolveSram, "resolving SRAM chunk");
_LIT(KStepVerifySram,  "verifying SRAM copy");
_LIT(KStepReadRegs,    "reading PRCM registers");
_LIT(KStepReady,       "ready");

const TDesC& COcEngine::StepName(TOcStep aStep)
    {
    switch (aStep)
        {
        case EOcStepLoadDriver:  return KStepLoadDriver;
        case EOcStepOpenPatcher: return KStepOpenPatcher;
        case EOcStepSelfTest:    return KStepSelfTest;
        case EOcStepReadPatch:   return KStepReadPatch;
        case EOcStepOpenDvfs:    return KStepOpenDvfs;
        case EOcStepResolveSram: return KStepResolveSram;
        case EOcStepVerifySram:  return KStepVerifySram;
        case EOcStepReadRegs:    return KStepReadRegs;
        case EOcStepReady:       return KStepReady;
        default:                 return KStepIdle;
        }
    }

TInt COcEngine::Poke(TUint32 aAddr, const TDesC8& aBytes)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TUint32 addr = aAddr;
    return iPatcher.WriteBytes(addr, aBytes);
    }

TInt COcEngine::PokeWords(TUint32 aAddr, const TUint32* aWords, TInt aCount)
    {
    TBuf8<512> b;
    if (aCount * 4 > b.MaxLength())
        return KErrOverflow;
    b.SetLength(aCount * 4);
    for (TInt i = 0; i < aCount; ++i)
        {
        b[i*4+0] = (TUint8)(aWords[i]);
        b[i*4+1] = (TUint8)(aWords[i] >> 8);
        b[i*4+2] = (TUint8)(aWords[i] >> 16);
        b[i*4+3] = (TUint8)(aWords[i] >> 24);
        }
    return Poke(aAddr, b);
    }

// Канал доводиться перевідкривати після кожної зміни патчу: драйвер тримає
// стан, а нам треба гарантувати, що наступний DoControl(1) піде у свіже тіло.
TInt COcEngine::ReopenDvfs()
    {
    if (iDvfsOpen)
        {
        iDvfs.Close();
        iDvfsOpen = EFalse;
        }
    TInt r = iDvfs.Open();
    if (r == KErrNone)
        iDvfsOpen = ETrue;
    return r;
    }

// Канал НЕ перевідкриваємо. Доведений прогін пробника відкривав його рівно
// один раз, і перевідкриття тут нічого не дає: диспетчер драйвера викликає
// пропатчене тіло через "bl 0x80148410" напряму, нічого не кешуючи. Зайві
// цикли close/open - це зайві операції над драйвером ядра на кожне читання,
// а апарат до цієї роботи був стабільний, тож нову churn вносити нема за що.
TInt COcEngine::ApplyReadPatch()
    {
    TInt r = PokeWords(KDvfsBody, KReadPatch, 4);
    if (r != KErrNone)
        return r;
    iExecMode = EFalse;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

TInt COcEngine::ApplyExecPatch()
    {
    // Байти беремо з асемблерного шаблону, а не з масиву констант: кодування
    // інструкцій рахує асемблер, а не я вручну.
    const TUint8* p = (const TUint8*)((TUint32)&OcExecPatchStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcExecPatchEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len != 24)
        return KErrCorrupt;
    TBuf8<24> patch;
    patch.Copy(p, len);

    // Підставляємо єдину адресу, стрибок на яку дозволено.
    const TUint32 allowed = iSram + KSramSlot;
    TBool found = EFalse;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)patch[i] | ((TUint32)patch[i+1] << 8) |
                    ((TUint32)patch[i+2] << 16) | ((TUint32)patch[i+3] << 24);
        if (w != 0xdeadbeefu)
            continue;
        patch[i+0] = (TUint8)(allowed);
        patch[i+1] = (TUint8)(allowed >> 8);
        patch[i+2] = (TUint8)(allowed >> 16);
        patch[i+3] = (TUint8)(allowed >> 24);
        found = ETrue;
        break;
        }
    if (!found)
        return KErrNotFound;

    TInt r = Poke(KDvfsBody, patch);
    if (r != KErrNone)
        return r;
    iExecMode = ETrue;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

// Знімає патч НАДІЙНО. Перезапису байтів для цього НЕ досить: жоден шлях
// патчера не робить Cache::IMB (findings §14), а пропатчена функція вже
// виконувалась, тож її інструкції лишаються в I-cache. Спостережено прямо:
// після відновлення байтів DoControl(1,0) повертав 0, тобто патч був живий.
// FreeShadowPage натомість викидає RAM-сторінку й повертає відображення на
// справжній ROM - разом із коректним обслуговуванням кешу.
// Байти все одно переписуємо першими: якщо unshadow колись не спрацює, вміст
// сторінки має бути штатним.
TInt COcEngine::EndPatchSession()
    {
    RestoreKernelCode();
    return UnshadowKernelPage();
    }

TInt COcEngine::ReadReg(TUint32 aAddr, TUint32& aOut)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TInt r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    aOut = (TUint32)iDvfs.ReadOrCall(aAddr);
    EndPatchSession();
    return KErrNone;
    }

TInt COcEngine::SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue)
    {
    aVerify = -1;
    aReadValue = 0;

    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;

    iStep = EOcStepOpenPatcher;
    r = iPatcher.Open();
    if (r != KErrNone)
        return r;
    iPatcherOpen = ETrue;
    if (aLevel < 1)
        return KErrNone;

    // Рівень 1: контракт func 105 на власному буфері. Ядро не чіпається.
    iStep = EOcStepSelfTest;
    TBuf8<4> target;
    TBuf8<4> bytes;
    target.SetLength(4);
    bytes.SetLength(4);
    target[0] = target[1] = target[2] = target[3] = 0;
    bytes[0] = 0x51; bytes[1] = 0xa7; bytes[2] = 0x3c; bytes[3] = 0xd2;
    TUint32 selfAddr = (TUint32)target.Ptr();
    r = iPatcher.WriteBytes(selfAddr, bytes);
    if (r != KErrNone || target.Compare(bytes) != 0)
        return (r != KErrNone) ? r : KErrCorrupt;
    if (aLevel < 2)
        return KErrNone;

    // Рівень 2: перший дотик до коду ядра. Саме тут виникає ROM-shadow сторінка,
    // і вона лишається до перезавантаження навіть після відновлення вмісту.
    iStep = EOcStepOpenDvfs;
    User::LoadLogicalDevice(KDvfsFile);
    r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    iStep = EOcStepReadPatch;
    r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    aReadValue = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Pll);
    // Перевіряти ПІСЛЯ unshadow, а не після перезапису байтів: саме unshadow
    // і є надійним зняттям патчу.
    EndPatchSession();
    aVerify = VerifyRestored();
    if (aLevel < 3)
        return KErrNone;

    return Prepare();
    }

TInt COcEngine::Prepare()
    {
    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;
    // dvfs_interface у ROM уже є, але спроба не завадить.
    User::LoadLogicalDevice(KDvfsFile);

    iStep = EOcStepOpenPatcher;
    r = iPatcher.Open();
    if (r != KErrNone)
        return r;
    iPatcherOpen = ETrue;

    // Самоперевірка контракту func 105 на власному буфері: ядро не чіпається.
    iStep = EOcStepSelfTest;
    TBuf8<4> target;
    TBuf8<4> bytes;
    target.SetLength(4);
    bytes.SetLength(4);
    target[0] = target[1] = target[2] = target[3] = 0;
    bytes[0] = 0x51; bytes[1] = 0xa7; bytes[2] = 0x3c; bytes[3] = 0xd2;
    TUint32 selfAddr = (TUint32)target.Ptr();
    r = iPatcher.WriteBytes(selfAddr, bytes);
    if (r != KErrNone || target.Compare(bytes) != 0)
        return (r != KErrNone) ? r : KErrCorrupt;

    iStep = EOcStepReadPatch;
    r = PokeWords(KDvfsBody, KReadPatch, 4);
    if (r != KErrNone)
        return r;
    iExecMode = EFalse;
    iPatched = ETrue;

    iStep = EOcStepOpenDvfs;
    r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    iStep = EOcStepResolveSram;
    TUint32 obj = (TUint32)iDvfs.ReadOrCall(KPwrDownGlobal + 4);
    if (obj < 0xc0000000u || obj >= 0xf0000000u || (obj & 3))
        return KErrNotFound;
    TUint32 sram = (TUint32)iDvfs.ReadOrCall(obj + 16);
    if (sram < 0xc0000000u || (sram & 3))
        return KErrNotFound;

    // Адресі не віримо без звірки: перше слово копії мусить дорівнювати
    // першому слову оригіналу в ROM. Інакше писали б у випадкову память ядра.
    iStep = EOcStepVerifySram;
    TUint32 w0 = (TUint32)iDvfs.ReadOrCall(sram);
    TUint32 romw0 = (TUint32)iDvfs.ReadOrCall(KRomSramRoutine);
    if (w0 != romw0)
        return KErrCorrupt;
    iSram = sram;

    iStep = EOcStepReadRegs;
    TUint32 sel1 = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Pll);
    TUint32 sel2 = (TUint32)iDvfs.ReadOrCall(KRegClkSel2Pll);
    TUint32 mpud = (TUint32)iDvfs.ReadOrCall(KRegClkSelMpu);
    TUint32 core = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Core);
    if (((sel1 >> 12) & 0x3ff) == 0)
        return KErrCorrupt;
    iStockClkSel1 = sel1;
    iStockCore = core;
    // core_sel читається то як 1, то як 2: губернатор DVFS рухає його в
    // рантаймі (findings 12, 18). Миттєвий відлік тут брехливий - спіймавши
    // апарат на нижній OPP, ми показали б 165 МГц замість 330. Тому знімаємо
    // серію ПІД НАВАНТАЖЕННЯМ і беремо максимум: губернатор піднімає OPP лише
    // тоді, коли є робота, а сповільнити відлік завада може, завищити - ні.
    TUint32 bestSel2 = sel2 & 3;
    for (TInt k = 0; k < 8; ++k)
        {
        volatile TUint32 x = 1;
        for (TInt i = 0; i < 150000; ++i)
            x = x * 1664525u + 1013904223u;
        TUint32 v = (TUint32)iDvfs.ReadOrCall(KRegClkSel2Pll) & 3;
        if (v > bestSel2)
            bestSel2 = v;
        }
    sel2 = bestSel2 ? bestSel2 : 2;
    iStockClocks.Decode(sel1, sel2, mpud, core);
    iAppliedMult = iStockClocks.iMult;

    // Пакет читань закінчено - знімаємо патч повністю. Далі кожна операція
    // накладає його сама, рівно на час своєї роботи.
    EndPatchSession();

    iStep = EOcStepReady;
    iReady = ETrue;
    return KErrNone;
    }

void COcEngine::ClocksFor(TInt aMult, TOcClocks& aOut) const
    {
    aOut = iStockClocks;
    aOut.iMult = aMult;
    }

// Підбір дільників під нову CORE. Мета - лишити L3 і DSS1 якнайближче до їхніх
// ШТАТНИХ частот: саме їхній зсув ламав апарат, а не сама частота ядра.
// Дільники цілочисельні, тож точна компенсація можлива не завжди; за рівної
// похибки віддаємо перевагу МЕНШІЙ частоті - вона безпечніша і для панелі,
// і для памʼяті (refresh рахується в тактах L3, тож повільніша шина означає
// частіший refresh, а не рідший).
static TInt PickDivider(TUint32 aNewCoreKhz, TUint32 aWantKhz, TInt aMin, TInt aMax)
    {
    if (!aWantKhz)
        return aMin;
    TInt best = aMin;
    TUint32 bestErr = 0xffffffffu;
    for (TInt d = aMin; d <= aMax; ++d)
        {
        TUint32 got = aNewCoreKhz / (TUint32)d;
        TUint32 err = (got > aWantKhz) ? (got - aWantKhz) : (aWantKhz - got);
        // Строго менше: за рівної похибки лишається БІЛЬШИЙ дільник, тобто
        // нижча частота.
        if (err < bestErr)
            {
            bestErr = err;
            best = d;
            }
        }
    return best;
    }

TUint32 COcEngine::CompensatedCore(TInt aMult) const
    {
    TOcClocks want;
    ClocksFor(aMult, want);
    const TUint32 newCore = want.CoreKhz();
    if (!newCore)
        return iStockCore;

    // DSS1 - точна компенсація до найближчого. Це та частота, зсув якої ламав
    // відрисовку, і вона нам не потрібна вища: дисплею байдуже до швидкості,
    // йому потрібна СВОЯ частота.
    const TInt dss = PickDivider(newCore, iStockClocks.Dss1Khz(), 1, 31);

    // L3/SDRAM - навпаки, тримаємо ЯКНАЙВИЩОЮ в межах допустимого. Компенсувати
    // її "до найближчого" було б помилкою: на mult 96 це опустило б памʼять
    // нижче штатної, тоді як замір показав, що вона спокійно тримає +11.5% з
    // нульовими помилками. Тож беремо найменший дільник, за якого L3 не
    // перевищує штатну більш ніж на 15%.
    const TUint32 l3Cap = iStockClocks.L3Khz() + iStockClocks.L3Khz() / 7u;
    TInt l3 = iStockClocks.iL3Div ? iStockClocks.iL3Div : 4;
    for (TInt d = 1; d <= 31; ++d)
        {
        if (newCore / (TUint32)d <= l3Cap)
            {
            l3 = d;
            break;
            }
        }
    if (l3 < iStockClocks.iL3Div)
        l3 = iStockClocks.iL3Div;      // ніколи не розганяємо шину сильніше, ніж вийшло б само

    TUint32 v = iStockCore;
    v = (v & ~KCoreL3Mask) | (((TUint32)l3 << KCoreL3Shift) & KCoreL3Mask);
    v = (v & ~KCoreDss1Mask) | (((TUint32)dss << KCoreDss1Shift) & KCoreDss1Mask);
    return v;
    }

TInt COcEngine::LiveMult(TInt& aOut)
    {
    TUint32 v = 0;
    TInt r = ReadReg(KRegClkSel1Pll, v);
    if (r != KErrNone)
        return r;
    aOut = (TInt)((v >> 12) & 0x3ff);
    return KErrNone;
    }

TInt COcEngine::WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcRelockBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcRelockBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 480)
        return KErrCorrupt;
    TBuf8<512> blob;
    blob.Copy(p, len);

    // Два слова-заглушки: цільове значення і початкове (для апаратного відкату
    // всередині критичної секції). Обидва мусять знайтись, інакше блоб не той,
    // на який ми розраховуємо, і виконувати його не можна.
    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xdeadbeefu)
            v = aClkSel1Value;
        else if (w == 0xfeedfaceu)
            v = iStockClkSel1;
        else if (w == 0xcafebabeu)
            v = aCoreValue;
        else if (w == 0xbaadf00du)
            v = iStockCore;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 4)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

TInt COcEngine::ApplyMult(TInt aMult, TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        return KErrArgument;

    // Міняємо рівно біти [21:12], решту регістра зберігаємо.
    TUint32 target = (iStockClkSel1 & ~0x003ff000u) |
                     (((TUint32)aMult & 0x3ff) << 12);

    TInt r = WriteRelockBlob(target, CompensatedCore(aMult));
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;

    TUint32 rc = (TUint32)iDvfs.ReadOrCall(iSram + KSramSlot);
    aStatus = rc >> 28;

    // Знімаємо патч НЕГАЙНО й повністю: далі йдуть заміри, і тримати підмінене
    // тіло функції ядра весь цей час не можна.
    EndPatchSession();

    iAppliedMult = aMult;
    iApplied = (aMult != iStockClocks.iMult);
    return KErrNone;
    }

TInt COcEngine::ApplyMultRamp(TInt aTarget, TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aTarget < KOcMultMin || aTarget > KOcMultMax)
        return KErrArgument;

    // Йдемо від ФАКТИЧНОГО множника, а не від запамʼятованого: якщо попередня
    // спроба відкатилась сама, стан у залізі може не збігатися з нашим.
    TInt cur = 0;
    TInt r = LiveMult(cur);
    if (r != KErrNone)
        return r;
    if (cur < KOcMultMin || cur > KOcMultMax)
        return KErrCorrupt;

    while (cur != aTarget)
        {
        cur += (aTarget > cur) ? 1 : -1;
        TUint32 st = 0;
        r = ApplyMult(cur, st);
        aStatus |= st;
        if (r != KErrNone)
            return r;
        // Ненульовий статус означає, що блоб уже сам відкатився на попереднє
        // значення. Далі не йдемо: наступна сходинка лише повторила б відмову.
        if (st != 0)
            return KErrAbort;
        User::After(20000);            // 20 мс на осідання підсистем
        }
    return KErrNone;
    }

TInt COcEngine::RestoreStock(TUint32& aStatus)
    {
    TInt r = ApplyMultRamp(iStockClocks.iMult, aStatus);
    if (r == KErrNone)
        iApplied = EFalse;
    return r;
    }
