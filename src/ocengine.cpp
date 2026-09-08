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

// Еталонний профіль. Значення - ті самі константи, що доведені на апараті, лише
// зібрані в одному місці. Поведінка RM-320 від цього не міняється ні на біт;
// сенс у тому, що ІНШИЙ апарат тепер додається як другий такий запис, а не
// правкою коду. Числа для нового профілю беруться з розпакованого z: (§29):
// dvfsBody - зсув тіла DoControl(1); romSram - idle-рутина power.dll; dspSetter -
// сеттер дільника DSP; pwrDown - глобал power-down. Де dvfs в прошивці нема
// (E90 та ін.) - iRead інший, а iDvfsBody = 0.
const TOcProfile KProfileRM320 =
    {
    "RM-320 31.0.015",
    EReadDvfs,
    KDvfsBody,
    KRomSramRoutine,
    KDspSetter,
    KPwrDownGlobal,
    KStockClkSel1Core,
    KStockClkSelMpu,
    KStockMult
    };

// --- Nokia E90 (RA-6, 400.34.93) --------------------------------------------
//
// Адреси виведено ОФЛАЙН із резидентного XIP E90 (out/roms/e90.rom, база
// 0x80000000) і байтово звірено (findings §31, out/analysis/e90.md):
//   iRomSramRoutine  0x80056874  пролог relock-рутини, як у RM-320;
//   iDspSetter       0x80059aa4  сеттер дільника DSP, маска 0x1f, як у RM-320;
//   iPwrDownGlobal   0xc8004194  геттер/сеттер str/ldr r0,[global,#4] у power-зоні.
// dvfs у прошивці E90 НЕМАЄ (перевірено: "dvfs" у XIP стосується дисплея), тож
// iDvfsBody = 0, а стратегія читання - EReadMeasureOnly (регістр не читаємо,
// міряємо роботу). Носій примітиву читання/виконання на E90 - бутстрап через
// хост Usbc (findings §32, ocprobe UsbcBoot), окремо від цього двигуна.
//
// ШТАТНА розкладка тактування (iStockClkSel1Core / iStockClkSelMpu) офлайн НЕ
// виведена: код ініціалізації лежить у стиснутій пейджованій частині, якої в
// резидентному XIP немає. Лишаємо 0 - жоден активний шлях їх ще не читає, бо
// Prepare() для цього профілю відмовляє (EOcStepUnsupported). Заповнити з
// апарата при першому вимірювальному прогоні через примітив Usbc; НЕ вгадувати
// (панель E90 інша, тож дільники CORE/DSS відрізняються від RM-320).
// iStockMult = 86 - лише робочий здогад для 330-МГц апарата, НЕ доведене число.
// Bootstrap E90 програмує mult=97 (266 МГц), а не 86 (findings §33); перехід на
// робочу частоту - у стиснутій ROFS, офлайн не видно. Перед розгоном множник
// читати з поля [21:12] CM_CLKSEL1_PLL на живому E90. До того Prepare() однаково
// відмовляє (EOcStepUnsupported), тож жоден шлях цього числа поки не вживає.
const TUint32 KE90RomSramRoutine = 0x80056874u;
const TUint32 KE90DspSetter      = 0x80059aa4u;
const TUint32 KE90PwrDownGlobal  = 0xc8004194u;
const TOcProfile KProfileE90 =
    {
    "RA-6 400.34.93",
    EReadMeasureOnly,
    0,                      // iDvfsBody: dvfs у прошивці немає
    KE90RomSramRoutine,
    KE90DspSetter,
    KE90PwrDownGlobal,
    0,                      // iStockClkSel1Core: не виведено офлайн, читати з апарата
    0,                      // iStockClkSelMpu:   те саме
    86                      // iStockMult: OMAP2420/330, підтвердити на апараті
    };

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
// Трамплін до ШТАТНОГО сеттера дільника DSP у power.dll (@0x800AC59C).
//
// Чому кличемо чужу функцію замість того, щоб самим писати в CM_CLKSEL_DSP:
//   * сеттер сам валідує значення (відкидає 0, 5, 9, 16, 18) і повертає -6,
//     тобто прошивка не дає записати те, що вважає недійсним для цього SoC -
//     рівно ті самі поручні, які проєкт уже бере для CM_CLKSEL1_CORE;
//   * усередині він робить Modify32 із маскою 0x1F, тобто решта регістра
//     зберігається чужим, перевіреним кодом, а не нашим read-modify-write;
//   * ми не вводимо нової форми патча ядра. WRITE-патч тіла DoControl(1)
//     ("strne r2,[r1]") був би гіршою за READ мішенню для чужих клієнтів
//     dvfs_interface, а до того ж просто не працює: диспетчер @0x80148428
//     знищує a2 (mov r3,#0) і передає в тіло r1 == r2 == a1.
// Функцію викликаємо ПРЯМО В ROM, не копіюючи: усі її власні bl лишаються
// коректними, зокрема bl на Modify32.
//
// Аргумент один - дільник у r1; r0 сеттер ігнорує (перезаписує одразу).
// Повертає 0 або -6, і це значення доїжджає до викликача як результат
// DoControl(1): EXEC-патч робить bxeq r1, тобто хвостовий перехід.
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
// Дільник MPU - у тій самій фіксації. Він дозволяє взяти потрібну частоту ядра
// при ВДВІЧІ нижчій CORE (діл. 1 замість 2), тобто перевірити гіпотезу, що межа
// не в самому ARM, а в CORE як найвищій внутрішній частоті.
"   ldr  r0, =0xF00DBEEF        \n"
"   str  r0, [r4, #0x140]       \n"
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
"   ldr  r0, =0xFEEDC0DE        \n"   // початковий CM_CLKSEL_MPU
"   str  r0, [r4, #0x140]       \n"
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
"                               \n"
// Трамплін до ШТАТНОГО сеттера дільника DSP у power.dll (@0x800AC59C).
//
// Чому кличемо чужу функцію замість того, щоб самим писати в CM_CLKSEL_DSP:
//   * сеттер сам валідує значення (відкидає 0, 5, 9, 16, 18) і повертає -6,
//     тобто прошивка не дає записати те, що вважає недійсним для цього SoC -
//     рівно ті самі поручні, які проєкт уже бере для CM_CLKSEL1_CORE;
//   * усередині він робить Modify32 (@0x800A1C50) - leaf-функцію з IRQ/FIQ off
//     і DMB, без блокувань і викликів, тож із нашого контексту вона безпечна, а
//     решта бітів регістра зберігається чужим перевіреним кодом;
//   * ми не вводимо нової форми патча ядра. WRITE-патч тіла DoControl(1)
//     ("strne r2,[r1]") був би гіршою за READ мішенню для чужих клієнтів
//     dvfs_interface, а до того ж просто не працює: диспетчер @0x80148428
//     знищує a2 (mov r3,#0) і передає в тіло r1 == r2 == a1.
// Функцію викликаємо ПРЯМО В ROM, не копіюючи: усі її власні bl лишаються
// коректними, зокрема bl на Modify32.
//
// ФІКСАЦІЯ. Самого запису в CM_CLKSEL_DSP НЕ ДОСИТЬ - перевірено на апараті:
// сеттер повертав 0, а регістр лишався зі старим значенням. Нове значення
// латчиться тільки записом 1 у PRCM_CLKCFG_CTRL (+0x80), як і для CLKSEL1_CORE
// та CLKSEL_MPU у relock-блобі вище. Штатний сеттер цього не робить - і саме
// тому він, вочевидь, ніде в ROM не викликається: у такому вигляді він не
// працює. Ланцюг тримаємо під вимкненими перериваннями: губернатор DVFS у
// рантаймі сам рухає CM_CLKSEL2_PLL і фіксує його тим самим латчем, тож між
// нашим записом і нашою фіксацією йому вклинюватись не можна.
//
// Повертає: 0 = записано, -6 = прошивка відкинула значення, 1 = латч не
// звільнився за відведені цикли.
"   .global OcDspBlobStart      \n"
"   .global OcDspBlobEnd        \n"
"OcDspBlobStart:                \n"
"   stmfd sp!, {r4, r5, lr}     \n"
"   mrs  r5, cpsr               \n"
"   orr  r3, r5, #0xC0          \n"
"   msr  cpsr_c, r3             \n"
"   ldr  r1, =0xD5D1D1D1        \n"   // дільник; патчиться зовні
"   ldr  r2, =0xD5A0A0A0        \n"   // адреса сеттера; патчиться зовні
"   mov  r0, #0                 \n"
"   blx  r2                     \n"
"   movs r4, r0                 \n"
"   bne  13f                    \n"   // сеттер відмовив - фіксувати нічого
"   ldr  r3, =0xC6008000        \n"
"   mov  r0, #1                 \n"
"   str  r0, [r3, #0x80]        \n"
"   mov  r2, #0x10000           \n"
"11:ldr  r0, [r3, #0x80]        \n"
"   tst  r0, #1                 \n"
"   beq  13f                    \n"
"   subs r2, r2, #1             \n"
"   bne  11b                    \n"
"   mov  r4, #1                 \n"   // таймаут латча
"13:msr  cpsr_c, r5             \n"
"   mov  r0, r4                 \n"
"   ldmfd sp!, {r4, r5, pc}     \n"
"   .ltorg                      \n"
"OcDspBlobEnd:                  \n"
"                               \n"
// Переписати ЛИШЕ дільники - CM_CLKSEL1_CORE і CM_CLKSEL_MPU - і зафіксувати їх
// латчем PRCM_CLKCFG_CTRL. DPLL при цьому не зупиняється.
//
// Навіщо окремий блоб, коли relock уже вміє писати ті самі два регістри. Бо
// relock заради цього щоразу заганяє DPLL у bypass, а це те саме вікно, яке при
// активному дисплеї вішає апарат (findings §20, §21). Тут CORE не змінюється
// взагалі, змінюються лише похідні від неї дільники - рівно те, що робить сам
// губернатор при переході OPP. Тобто це не новий клас операції для цього SoC, а
// той самий, яким прошивка користується постійно.
//
// Потрібен цей шлях для одного: утримати розкладку, поки в тому ж вікні працює
// вимірюваний процес. Замовник спостерігав, що записані дільники живуть секунди
// й повертаються до штатного слова; перезаписати їх повним relock посеред
// прогону емулятора не можна, а так - можна.
//
// Повертає 0, якщо латч звільнився, і 1, якщо ні.
//
// НЕ ПЕРЕВІРЕНО НА АПАРАТІ станом на 2026-08-15: код написаний, зібраний, але
// жодного разу не виконаний. Увімкнення - лише явним ключем hold=1.
"   .global OcDivBlobStart      \n"
"   .global OcDivBlobEnd        \n"
"OcDivBlobStart:                \n"
"   mrs  r12, cpsr              \n"
"   orr  r3, r12, #0xC0         \n"
"   msr  cpsr_c, r3             \n"
"   ldr  r3, =0xC6008000        \n"
"   ldr  r0, =0xD1C0DE01        \n"   // нове CM_CLKSEL1_CORE; патчиться зовні
"   str  r0, [r3, #0x240]       \n"
"   ldr  r0, =0xD1C0DE02        \n"   // нове CM_CLKSEL_MPU; патчиться зовні
"   str  r0, [r3, #0x140]       \n"
"   mov  r1, #1                 \n"
"   str  r1, [r3, #0x80]        \n"
"   mov  r2, #0x10000           \n"
"   mov  r0, #1                 \n"   // поки латч не звільнився - це таймаут
"21:ldr  r1, [r3, #0x80]        \n"
"   tst  r1, #1                 \n"
"   moveq r0, #0                \n"
"   beq  23f                    \n"
"   subs r2, r2, #1             \n"
"   bne  21b                    \n"
"23:msr  cpsr_c, r12            \n"
"   bx   lr                     \n"
"   .ltorg                      \n"
"OcDivBlobEnd:                  \n"
);

extern "C" void OcRelockBlobStart(void);
extern "C" void OcRelockBlobEnd(void);
extern "C" void OcExecPatchStart(void);
extern "C" void OcExecPatchEnd(void);
extern "C" void OcDspBlobStart(void);
extern "C" void OcDspBlobEnd(void);
extern "C" void OcDivBlobStart(void);
extern "C" void OcDivBlobEnd(void);

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

// --- поля CM_CLKSEL1_CORE ---------------------------------------------------
//
// Дозволені значення - точно ті, які приймають сеттери power.dll. Кожен робить
// cmp/таблицю переходів перед Modify32; хто не пройшов - отримує KErrNotSupported.
// Маски дозволених нижче зібрані з тих перевірок (findings §21).
#define ALL_EXCEPT(n)  (0xfffffffeu & ~(1u << (n)))

// ВИПРАВЛЕНО 2026-08-15. Для L3 тут стояло ALL_EXCEPT(18) - тобто «дозволено
// все, крім 18». Це помилка читання валідації.
//
// Валідатор L3 @0x800AC424 читається так:
//   cmp r1,#5 / beq reject          -> 5 відкинуто
//   bgt >5                          -> гілка для великих
//   cmp r1,#0 / cmpne r1,#3 / bne accept   -> 0 і 3 відкинуто
//   cmp r1,#9 / cmpne r1,#18 / bne accept  -> 9 і 18 відкинуто
// Тобто відкидаються 0, 3, 5, 9, 18 - той самий за формою перелік, що й у
// сеттера дільника DSP (0, 5, 9, 16, 18).
//
// АЛЕ САМОЇ ЦІЄЇ ТАБЛИЦІ НЕ ДОСИТЬ, і це головний урок §26. Валідатор DSS1
// @0x800AC49C відкидає ЛИШЕ 18, проте апарат відмовився взяти `dssdiv=7`: два
// прогони з ним (у другому - при завідомо дійсному `l3div=6`) не змінили
// регістр зовсім, а той самий прогін із `dssdiv=6` записався. Отже апаратний
// набір дільників вужчий за той, що валідує прошивка, і недійсне значення в
// ОДНОМУ полі відкидає ВЕСЬ запис у CM_CLKSEL1_CORE, а не лише своє поле.
//
// Звідси два різні набори. Тут - широкий, з валідації прошивки, плюс виміряне
// виключення 7 для DSS1: ручний режим лишається гнучким, а відмову апарата
// ловить читання назад (рядок MISMATCH). Для АВТОМАТИЧНОГО підбору
// використовується завідомо вужчий KAutoDivAllowed - там помилятись нема на що.
static const TCoreField KCoreFields[ECoreFieldCount] =
    {
    // назва      маска         зсув  дозволені            база
    { "L3   ",  0x0000001fu,   0, ALL_EXCEPT(18) & ~((1u<<3)|(1u<<5)|(1u<<9)), 0 },
    { "L4   ",  0x00000060u,   5, (1u<<1)|(1u<<2),        1 },
    { "DSS1 ",  0x00001f00u,   8, ALL_EXCEPT(18) & ~(1u<<7), 0 },
    { "DSS2 ",  0x00002000u,  13, (1u<<0)|(1u<<1),       -1 },
    { "VLYNQ",  0x000f8000u,  15, ALL_EXCEPT(5),          0 },
    { "SSI  ",  0x01f00000u,  20, (1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<6)|(1u<<8), 0 },
    { "USB  ",  0x0e000000u,  25, (1u<<1)|(1u<<2)|(1u<<4), -1 }
    };

// Набір для АВТОМАТИЧНОГО підбору: 1, 2, 4, 5, 6, 8, 12, 16.
//
// Вужчий за валідацію прошивки, і навмисно: недійсне значення означає, що не
// запишеться НІЧОГО, і CORE поїде зі старими дільниками - рівно та ситуація,
// яка ламала відрисовку (§21). Тут лише те, що або виміряно, або безспірне.
//
// Виміряно на апараті: 2, 4, 6 і 8 записуються; 5 теж (апарат 13 годин
// пропрацював із dssdiv=5, залишеним чужим прогоном); 7 НЕ записується.
// Значення 1, 12 і 16 не перевірялись, але в підборі для робочого діапазону
// множників не з'являються.
//
// 3 і 9 свідомо НЕ включені: валідатор L3 їх відкидає, а для DSS1 вони не
// перевірені. Ціна - компенсація дисплея неточна на множниках нижче ~60, де
// потрібен саме дільник 3. Для робочого діапазону 86..111 це значення не
// потрібне; для рампи вниз до 43 - потрібне, але той шлях і без того
// перекритий (§26.10).
const TUint32 KAutoDivAllowed =
    (1u<<1)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<12)|(1u<<16);

// Для L3 - те саме, але без 5: його відкидає валідатор @0x800AC424. Два набори,
// а не один, саме тому: спільний набір або пропустив би недійсне для L3
// значення, або даремно відібрав би дійсне для DSS1.
const TUint32 KAutoL3Allowed = KAutoDivAllowed & ~(1u<<5);

TInt MpuDivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 0 && v <= 31; v += aDir)
        {
        if (KMpuAllowed & (1u << v))
            return v;
        }
    return aValue;
    }

// Наступний/попередній дільник DSP із дозволених прошивкою. Та сама механіка,
// що й у MpuDivStep: поручні беремо з валідації штатного сеттера, а не з
// власних уявлень про те, що «має працювати».
TInt DspDivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 1 && v <= 31; v += aDir)
        {
        if (KDspDivAllowed & (1u << v))
            return v;
        }
    return aValue;
    }

const TCoreField& CoreField(TInt aId)
    {
    if (aId < 0 || aId >= ECoreFieldCount)
        aId = 0;
    return KCoreFields[aId];
    }

TInt CoreFieldValue(TUint32 aCoreReg, TInt aId)
    {
    const TCoreField& f = CoreField(aId);
    return (TInt)((aCoreReg & f.iMask) >> f.iShift);
    }

TUint32 CoreFieldSet(TUint32 aCoreReg, TInt aId, TInt aValue)
    {
    const TCoreField& f = CoreField(aId);
    return (aCoreReg & ~f.iMask) | ((((TUint32)aValue) << f.iShift) & f.iMask);
    }

TInt CoreFieldStep(TInt aId, TInt aValue, TInt aDir)
    {
    const TCoreField& f = CoreField(aId);
    // Ширина поля обмежує діапазон: для 1-бітного DSS2 це 0..1, для 5-бітного 0..31.
    TInt maxv = (TInt)(f.iMask >> f.iShift);
    for (TInt v = aValue + aDir; v >= 0 && v <= maxv; v += aDir)
        {
        if (v < 32 && (f.iAllowed & (1u << v)))
            return v;
        }
    return aValue;                 // далі допустимих немає - лишаємо як було
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
    iMpuDiv = (TInt)(aMpuDiv & KMpuDivMask);
    iL3Div = (TInt)(aCoreDiv & 0x1f);
    iDss1Div = (TInt)((aCoreDiv & KCoreDss1Mask) >> KCoreDss1Shift);
    }

// ... existing frequency getters ...
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
  : iProfile(&KProfileRM320),
    iPatcherOpen(EFalse), iDvfsOpen(EFalse), iReady(EFalse), iExecMode(EFalse), iPatched(EFalse), iShadowed(EFalse),
    iApplied(EFalse), iKeep(EFalse), iAppliedMult(0), iSram(0), iStockClkSel1(0), iStockCore(0), iStockMpu(0),
    iBootCore(0), iBootMpu(0), iBootMult(0),
    iStockDspDiv(0), iDspDiv(0), iStep(EOcStepIdle)
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
    // Дільник DSP - за тим самим правилом, що й частота ядра: процес помирає, а
    // домен лишається переналаштованим, і тихо віддати апарат таким було б
    // негідно. KeepOnExit покриває і це.
    if (!iKeep && iSram && iDspDiv != iStockDspDiv)
        {
        TInt rc = 0;
        RestoreDspDiv(rc);
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
    TInt r = PokeWords(iProfile->iDvfsBody, KDvfsOriginal, 6);
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
    TUint32 addr = iProfile->iDvfsBody;
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
_LIT(KStepUnsupported, "profile has no dvfs read path (E90)");
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
        case EOcStepUnsupported: return KStepUnsupported;
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
    TInt r = PokeWords(iProfile->iDvfsBody, KReadPatch, 4);
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

    TInt r = Poke(iProfile->iDvfsBody, patch);
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

// Читання кількох адрес за ОДИН цикл патч/unshadow. Кожен такий цикл - це
// вікно, у якому пропатчене тіло DoControl(1) живе в ядрі, а канал
// dvfs_interface відкриваємо не лише ми (§15: mmrmserver.dll,
// fmradiotunercontrol.dll). Тож N окремих ReadReg - це N вікон замість одного,
// і платити цим за зручність нема за що.
TInt COcEngine::ReadRegs(const TUint32* aAddr, TUint32* aOut, TInt aCount)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TInt r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    for (TInt i = 0; i < aCount; ++i)
        aOut[i] = (TUint32)iDvfs.ReadOrCall(aAddr[i]);
    EndPatchSession();
    return KErrNone;
    }

TInt COcEngine::ReadReg(TUint32 aAddr, TUint32& aOut)
    {
    return ReadRegs(&aAddr, &aOut, 1);
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
    // Запобіжник переносу: уся ця процедура патчить iDvfsBody і читає ядро через
    // цей патч. На профілі без dvfs (E90: iRead != EReadDvfs, iDvfsBody == 0)
    // накладання патча пішло б в адресу 0 чужого ядра й повісило б апарат
    // (findings §27). Тому відмовляємо ДО будь-якого дотику до ядра. Шлях
    // читання для таких апаратів - бутстрап Usbc, окрема процедура.
    if (iProfile->iRead != EReadDvfs || iProfile->iDvfsBody == 0)
        {
        iStep = EOcStepUnsupported;
        return KErrNotSupported;
        }

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

    // Позначаємо патч накладеним ДО запису: якщо PokeWords ляже частково (драйвер
    // міг уже створити shadow-сторінку), EndPatchSession нижче все одно відкотить.
    iStep = EOcStepReadPatch;
    iExecMode = EFalse;
    iPatched = ETrue;
    r = PokeWords(iProfile->iDvfsBody, KReadPatch, 4);
    if (r == KErrNone)
        r = PrepareProbe();

    // Патч знімаємо ЗАВЖДИ - і на успіху, і на будь-якій помилці проби. Це
    // головний запобіжник переносу: на апараті з іншими адресами ReopenDvfs або
    // звірка SRAM обірвуться, і без цього рядка пропатчене чуже тіло ядра
    // лишилося б жити до перезавантаження (findings §27).
    EndPatchSession();
    if (r != KErrNone)
        return r;

    iStep = EOcStepReady;
    iReady = ETrue;
    return KErrNone;
    }

TInt COcEngine::PrepareProbe()
    {
    iStep = EOcStepOpenDvfs;
    TInt r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    iStep = EOcStepResolveSram;
    TUint32 obj = (TUint32)iDvfs.ReadOrCall(iProfile->iPwrDownGlobal + 4);
    if (obj < 0xc0000000u || obj >= 0xf0000000u || (obj & 3))
        return KErrNotFound;
    TUint32 sram = (TUint32)iDvfs.ReadOrCall(obj + 16);
    if (sram < 0xc0000000u || (sram & 3))
        return KErrNotFound;

    // Адресі не віримо без звірки: перше слово копії мусить дорівнювати
    // першому слову оригіналу в ROM. Інакше писали б у випадкову память ядра.
    iStep = EOcStepVerifySram;
    TUint32 w0 = (TUint32)iDvfs.ReadOrCall(sram);
    TUint32 romw0 = (TUint32)iDvfs.ReadOrCall(iProfile->iRomSramRoutine);
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
    // Стан, у якому застали апарат. Він потрібен, щоб знати, звідки починати
    // рампу, і щоб голосно сказати про нештатний старт, - але опорною точкою
    // компенсації він НЕ Є.
    iBootCore = core;
    iBootMpu = mpud;
    iBootMult = (TInt)((sel1 >> 12) & 0x3ff);
    // Опорна точка - стала. Чому саме так - у коментарі при KStockClkSel1Core.
    iStockCore = iProfile->iStockClkSel1Core;
    iStockMpu = iProfile->iStockClkSelMpu;
    // Дільник DSP. Прошивка його в рантаймі не змінює жодного разу: обидва
    // штатні сеттери (power.dll @0x800AC59C, ehwaomap.pdd @0x801AE900) в усьому
    // ROM не викликаються й не експортуються. Тож знятого тут значення досить
    // як опорного - воно не «попливе» саме собою.
    TUint32 dsel = (TUint32)iDvfs.ReadOrCall(KRegClkSelDsp);
    iStockDspDiv = (TInt)(dsel & KDspFwDivMask);
    if (iStockDspDiv == 0)
        iStockDspDiv = 3;
    iDspDiv = iStockDspDiv;
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
    // Декодуємо ШТАТНУ розкладку, а не застану: sys_clk і дільник DPLL беремо з
    // заліза (вони незмінні), множник і дільники - з констант.
    iStockClocks.Decode(sel1, sel2, iProfile->iStockClkSelMpu, iProfile->iStockClkSel1Core);
    iStockClocks.iMult = iProfile->iStockMult;
    // А ось поточний множник - саме той, що в залізі: рампа має йти звідти, де
    // апарат стоїть насправді. iApplied при цьому лишається хибним: він означає
    // "це МИ змінили", і чужий розгін чіпати мовчки, у деструкторі, не можна.
    iAppliedMult = iBootMult;

    // Проба завершена. Патч знімає викликач (Prepare) - і тут, і на аварійних
    // виходах вище, одним EndPatchSession, тож жоден шлях не лишає його в ядрі.
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
// aAllowed - бітова маска значень, які приймає САМ АПАРАТ. Пропустити її не
// можна: недійсне значення хоч в одному полі означає, що запис у
// CM_CLKSEL1_CORE не проходить ЦІЛКОМ, разом із коректними полями (§26).
static TInt PickDivider(TUint32 aNewCoreKhz, TUint32 aWantKhz, TInt aMin, TInt aMax,
                        TUint32 aAllowed)
    {
    if (!aWantKhz)
        return aMin;
    TInt best = aMin;
    TUint32 bestErr = 0xffffffffu;
    for (TInt d = aMin; d <= aMax; ++d)
        {
        if (d < 32 && !(aAllowed & (1u << d)))
            continue;
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
    const TInt dss = PickDivider(newCore, iStockClocks.Dss1Khz(), 1, 31,
                                 KAutoDivAllowed);

    // L3/SDRAM - беремо НАЙВИЩУ шину, яка не перевищує виміряну стелю.
    //
    // ВИПРАВЛЕНО 2026-08-15 - тут був хибний висновок, і він схиляв рішення не в
    // той бік. Стояло, що повільніша шина безпечніша, бо "refresh рахується в
    // тактах L3, тож повільніша шина означає частіший refresh". Насправді
    // навпаки. `SDRC_RFR_CTRL0 = 0x0004DA01`, тобто ARCV = 1242 такти L3: при
    // 165.12 МГц це 7.5 мкс, рівно вікно авторефрешу для 8192 рядків за 64 мс.
    // Повільніша шина РОЗТЯГУЄ це вікно в реальному часі, тобто refresh стає
    // рідшим, а не частішим, і памʼять втрачає заряд. Прошивка свій нижчий OPP
    // компенсує сама, про наш дільник вона не знає. Отже опускати шину нижче
    // штатної - це ризик, а не запобіжник.
    //
    // Стеля 213120 кГц узята не зі стелі: це та частота шини, на якій апарат
    // фактично працює щодня при mult 111. Компенсація там ніколи не
    // застосовувалась (§26), тобто шина весь час їхала разом із CORE, і в кожному
    // лозі перевірка цілісності SDRAM давала нуль помилок.
    const TUint32 kL3Max = 213120u;
    TInt l3 = iStockClocks.iL3Div ? iStockClocks.iL3Div : 4;
    for (TInt d = 1; d <= 31; ++d)
        {
        if (!(KAutoL3Allowed & (1u << d)))
            continue;
        if (newCore / (TUint32)d <= kL3Max)
            {
            l3 = d;
            break;
            }
        }

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

TInt COcEngine::WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue,
                                TUint32 aMpuValue)
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
        else if (w == 0xf00dbeefu)
            v = aMpuValue;
        else if (w == 0xfeedc0deu)
            v = iStockMpu;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 6)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Кладе трамплін до сеттера DSP у той самий SRAM-слот, куди EXEC-патч і
// стрибає. Слот один, тож relock-блоб і цей блоб просто перезаписують один
// одного - обидва пишуться безпосередньо перед своїм єдиним викликом.
TInt COcEngine::WriteDspBlob(TInt aDiv)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcDspBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcDspBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 160)
        return KErrCorrupt;
    TBuf8<160> blob;
    blob.Copy(p, len);

    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xd5d1d1d1u)
            v = (TUint32)aDiv;
        else if (w == 0xd5a0a0a0u)
            v = iProfile->iDspSetter;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    // Обидві заглушки мусять знайтись. Якщо асемблер згорнув котрусь у
    // безпосередній операнд, блоб не той, на який ми розраховуємо, і виконувати
    // його в режимі ядра не можна.
    if (patched != 2)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Той самий слот SRAM, той самий EXEC-патч - але блоб, який лише переписує
// дільники й фіксує їх латчем, без зупинки DPLL. Обґрунтування - при
// OcDivBlobStart.
TInt COcEngine::WriteDivBlob(TUint32 aCoreReg, TUint32 aMpuReg)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcDivBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcDivBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 160)
        return KErrCorrupt;
    TBuf8<160> blob;
    blob.Copy(p, len);

    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xd1c0de01u)
            v = aCoreReg;
        else if (w == 0xd1c0de02u)
            v = aMpuReg;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 2)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Переписати дільники поверх того, що там стоїть зараз. aRc - результат самого
// блоба: 0 = латч звільнився, 1 = не звільнився за відведені цикли.
//
// Множник НЕ чіпається, тож викликати це можна й тоді, коли в системі працює
// вимірюваний процес із живим дисплеєм, - на відміну від ApplyMultAndCore.
TInt COcEngine::ReassertDividers(TUint32 aCoreReg, TUint32 aMpuReg, TInt& aRc)
    {
    aRc = KErrGeneral;
    if (!iReady || !iSram)
        return KErrNotReady;
    TInt r = WriteDivBlob(aCoreReg, aMpuReg);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aRc = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    return KErrNone;
    }

// Виконує довільний блоб ARM-коду в режимі ядра через слот SRAM.
// aBlob - готові байти коду, до 2048 байтів. aResult - те, що блоб
// повернув у r0.
TInt COcEngine::ExecCustomBlob(const TDesC8& aBlob, TInt& aResult)
    {
    aResult = 0;
    if (!iReady || !iSram)
        return KErrNotReady;
    if (aBlob.Length() <= 0 || aBlob.Length() > 2048)
        return KErrArgument;

    TInt r = Poke(iSram + KSramSlot, aBlob);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aResult = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    return KErrNone;
    }

// Змінює дільник домену DSP. aSetterRc - те, що повернув штатний сеттер:
// 0 = записано, -6 (KErrArgument) = прошивка значення відкинула.
//
// На відміну від relock DPLL, тут НЕ потрібні ні bypass, ні полінг IDLEST, ні
// self-refresh SDRAM: це дільник окремого домену, а не PLL. CORE, SDRAM, дисплей
// і USB не зачіпаються.
//
// ВИПРАВЛЕННЯ попереднього висновку. Тут раніше стояло, що латч
// PRCM_CLKCFG_CTRL не потрібен, бо обидва штатні сеттери роблять звичайний
// read-modify-write без коміту. Це виявилось хибним і показано на апараті:
// сеттер повертав 0, а регістр лишався зі старим значенням. Латч обов'язковий, і
// його робить наш блоб; заразом це й пояснює, чому обидва штатні сеттери ніде в
// ROM не викликаються - у такому вигляді вони не працюють. Переривання на час
// "запис + латч" теж вимикаються, бо той самий латч використовує губернатор.
TInt COcEngine::ApplyDspDiv(TInt aDiv, TInt& aSetterRc)
    {
    aSetterRc = KErrGeneral;
    if (!iReady)
        return KErrNotReady;
    // Перевіряємо і в себе теж. Сеттер пропускає значення >31, а Modify32
    // маскує їх 0x1F - тобто 32 записало б у поле 0 і зупинило клок DSP.
    if (aDiv < 1 || aDiv > 31)
        return KErrArgument;
    if (!(KDspDivAllowed & (1u << aDiv)))
        return KErrArgument;

    TInt r = WriteDspBlob(aDiv);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aSetterRc = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    if (aSetterRc == KErrNone)
        iDspDiv = aDiv;
    return KErrNone;
    }

TInt COcEngine::RestoreDspDiv(TInt& aSetterRc)
    {
    aSetterRc = KErrNone;
    if (iDspDiv == iStockDspDiv)
        return KErrNone;
    return ApplyDspDiv(iStockDspDiv, aSetterRc);
    }

// Перечитує CM_CLKSEL_DSP з апарата. Своєму запамʼятованому значенню не
// віримо - те саме правило, що й для LiveMult.
TInt COcEngine::LiveDspDiv(TInt& aOut)
    {
    TUint32 v = 0;
    TInt r = ReadReg(KRegClkSelDsp, v);
    if (r != KErrNone)
        return r;
    aOut = (TInt)(v & KDspFwDivMask);
    return KErrNone;
    }

// Частота DSP так, як її рахує сама прошивка: CORE / CM_CLKSEL_DSP[3:0]
// (power.dll ord60 @0x800B1620). Саме це число драйвер ehwaomap.pdd вписує в
// символ _BRIDGEINIT_DSP_FREQ базового образу DSP.
TUint32 COcEngine::DspKhz(TInt aDiv) const
    {
    if (aDiv <= 0)
        return 0;
    return iStockClocks.CoreKhz() / (TUint32)aDiv;
    }

TInt COcEngine::ApplyMult(TInt aMult, TUint32& aStatus)
    {
    return ApplyMultAndCore(aMult, CompensatedCore(aMult), 0, aStatus);
    }

TInt COcEngine::ApplyMultAndCore(TInt aMult, TUint32 aCoreReg, TUint32 aMpuReg,
                                 TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        return KErrArgument;

    // Міняємо рівно біти [21:12], решту регістра зберігаємо.
    TUint32 target = (iStockClkSel1 & ~0x003ff000u) |
                     (((TUint32)aMult & 0x3ff) << 12);

    TInt r = WriteRelockBlob(target, aCoreReg, aMpuReg ? aMpuReg : iStockMpu);
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
    // "Ми лишили апарат нештатним" - це не лише про множник. Дільник MPU при
    // штатному множнику змінює частоту ядра вдвічі, і не помітити цього в
    // деструкторі означало б віддати апарат наполовину повільнішим.
    iApplied = (aMult != iStockClocks.iMult) ||
               (aCoreReg != 0 && aCoreReg != iStockCore) ||
               (aMpuReg != 0 && aMpuReg != iStockMpu);
    return KErrNone;
    }

TInt COcEngine::ApplyMultRamp(TInt aTarget, TUint32& aStatus)
    {
    return ApplyRampWithCore(aTarget, 0, 0, aStatus);
    }

// aCoreReg == 0 означає автопідбір на кожній сходинці; інакше на всіх сходинках
// стоять ті самі, задані вручну дільники.
TInt COcEngine::ApplyRampWithCore(TInt aTarget, TUint32 aCoreReg,
                                  TUint32 aMpuReg, TUint32& aStatus)
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
        r = aCoreReg ? ApplyMultAndCore(cur, aCoreReg, aMpuReg, st)
                     : ApplyMult(cur, st);
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

// Повернення штатного стану - у два кроки, і другий обов'язковий.
//
// Рампа веде множник до штатного, компенсуючи дільники на кожній сходинці, і
// цього досить, поки нештатним був сам множник. Але якщо множник уже штатний, а
// нештатним лишався ДІЛЬНИК (розкладка виду "mult 86, mpu_div 4"), рампа не
// робить жодного запису: її цикл while одразу закінчується. Тому після неї йде
// один явний запис штатної розкладки цілком.
TInt COcEngine::RestoreStock(TUint32& aStatus)
    {
    aStatus = 0;
    TInt r = ApplyMultRamp(iStockClocks.iMult, aStatus);
    if (r != KErrNone)
        return r;

    // Другий крок робимо лише тоді, коли дільники справді не штатні, і
    // з'ясовуємо це ЧИТАННЯМ, а не з власного обліку. Причина: цей запис -
    // повний relock, тобто вікно bypass, а при живому дисплеї таке вікно вішало
    // апарат (§20). Робити його "про всяк випадок" не можна.
    const TUint32 regs[2] = { KRegClkSel1Core, KRegClkSelMpu };
    TUint32 v[2] = { 0, 0 };
    if (ReadRegs(regs, v, 2) == KErrNone &&
        (v[0] & (KCoreL3Mask | KCoreDss1Mask)) ==
            (iStockCore & (KCoreL3Mask | KCoreDss1Mask)) &&
        (v[1] & KMpuDivMask) == (iStockMpu & KMpuDivMask))
        {
        iApplied = EFalse;
        return KErrNone;
        }

    // Дільники повертаємо ЛЕГКИМ шляхом: запис плюс латч, без bypass DPLL.
    //
    // Множник тут уже штатний - про це подбала рампа вище, - тож повний relock
    // не додає нічого, крім свого вікна bypass. А саме це вікно, схоже, і вбивало
    // апарат при поверненні з `l3div=8`: у bypass CORE падає до sys_clk, і шина
    // при дільнику 8 отримує 2.4 МГц замість 4.8 при штатному дільнику 4. Зміна
    // 4 -> 8 проходила чисто двічі, зворотна 8 -> 4 двічі вбивала, і вмирала саме
    // тут (§26.14). Асиметрія вказує на стан ПЕРЕД вікном, а не на цільові
    // значення.
    TInt rc = -1;
    r = ReassertDividers(iStockCore, iStockMpu, rc);
    if (r == KErrNone && rc == 0)
        {
        // Читаємо назад: легкий шлях довіри без перевірки теж не заслуговує.
        TUint32 w[2] = { 0, 0 };
        if (ReadRegs(regs, w, 2) == KErrNone &&
            (w[0] & (KCoreL3Mask | KCoreDss1Mask)) ==
                (iStockCore & (KCoreL3Mask | KCoreDss1Mask)) &&
            (w[1] & KMpuDivMask) == (iStockMpu & KMpuDivMask))
            {
            iApplied = EFalse;
            return KErrNone;
            }
        }

    // Не вийшло - повний relock як запасний шлях. Він гірший, але лишити апарат
    // із нештатними дільниками гірше за нього.
    TUint32 st = 0;
    r = ApplyMultAndCore(iStockClocks.iMult, iStockCore, iStockMpu, st);
    aStatus |= st;
    if (r == KErrNone)
        iApplied = EFalse;
    return r;
    }
