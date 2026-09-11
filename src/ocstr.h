// ЗГЕНЕРОВАНО tools/gen_strings.js з src/strings.json. Руками не правити.
#ifndef OCSTR_H
#define OCSTR_H

#include <e32std.h>

enum TOcStrId
    {
    EStrRowApply,
    EStrRowConfirmArmed,
    EStrRowConfirmIdle,
    EStrRowRestore,
    EStrRowTestAlu,
    EStrRowTestMem,
    EStrRowTestInt,
    EStrRowTestCache,
    EStrRowTestAll,
    EStrRowHelp,
    EStrModeAuto,
    EStrModeManual,
    EStrApplied,
    EStrRejected,
    EStrRunning,
    EStrNotReady,
    EStrEngineDisabled,
    EStrNoMemory,
    EStrPressConfirm,
    EStrNothingArmed,
    EStrGuardOn,
    EStrGuardOff,
    EStrGuardNote,
    EStrGuardRowOn,
    EStrGuardRowOff,
    EStrBusLeftLowered,
    EStrWarnBusBelow,
    EStrWarnBusReturn,
    EStrWarnBusAbove,
    EStrAboveTested,
    EStrRelockFailed,
    EStrHelpTitle,
    EStrCount
    };

// Вибирає мову один раз, за User::Language(). Невідома мова - англійська.
void OcStrInit();
// Рядок активною мовою. Для дійсного id ніколи не порожній.
TPtrC OcStr(TInt aId);
// Вбудована довідка: скільки рядків і який рядок за номером.
TInt OcHelpLineCount();
TPtrC OcHelpLine(TInt aIndex);

#endif
