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
    EStrBusLeftLowered,
    EStrWarnBusBelow,
    EStrWarnBusReturn,
    EStrWarnBusAbove,
    EStrAboveTested,
    EStrRelockFailed,
    EStrHelpTitle,
    EStrVerdictClean,
    EStrVerdictWeakBus,
    EStrVerdictWeakCache,
    EStrVerdictWeakCpu,
    EStrHintArrows,
    EStrBusBackToAuto,
    EStrWarnDspAbove,
    EStrWarnDspUseMult,
    EStrUnknownSend,
    EStrUnknownSeeDocs,
    EStrUnknownDevice,
    EStrNoProfileUid,
    EStrNoProfileBuild,
    EStrPatcherMissing,
    EStrHintArrowsDiv,
    EStrNeedManual,
    EStrDspAlreadySet,
    EStrApplying,
    EStrDspNoSetter,
    EStrDspNoSetterWhy,
    EStrMoreRows,
    EStrCombinedRound,
    EStrDspRejected,
    EStrDspMismatch,
    EStrDspNoTimer,
    EStrDspHeld,
    EStrBusClamped,
    EStrRowDspRevert,
    EStrRowDspLive,
    EStrRowBusManual,
    EStrRowBusAuto,
    EStrDspReverted,
    EStrReasonTimeout,
    EStrReasonRestore,
    EStrTestProfileKnown,
    EStrTestProfileRow,
    EStrTestProfileNone,
    EStrTestProfilePick,
    EStrTestProfileWarn1,
    EStrTestProfileWarn2,
    EStrTestProfileTrying,
    EStrTestProfileOk,
    EStrTestProfileFoundGlobal,
    EStrKeepRowOn,
    EStrKeepRowOff,
    EStrKeepNote,
    EStrRowLanguage,
    EStrCount
    };

// Вибирає мову один раз, за User::Language(). Невідома мова - англійська.
void OcStrInit();
// Рядок активною мовою; якщо для неї перекладу немає - англійський.
TPtrC OcStr(TInt aId);
// Вбудована довідка активною мовою, інакше англійська.
TInt OcHelpLineCount();
TPtrC OcHelpLine(TInt aIndex);
// Скільки мов у збірці і код активної - для діагностики.
TInt OcStrLanguageCount();
TPtrC OcStrActiveLanguage();
// Ручний вибір мови. Індекс замикається по колу, тож викликач може просто
// додавати або віднімати одиницю, не дбаючи про межі.
void OcStrSetLanguage(TInt aIndex);
TInt OcStrLanguageIndex();
// Власна назва мови за індексом; без аргументу - активної.
TPtrC OcStrLanguageName(TInt aIndex);
TPtrC OcStrActiveLanguageName();

#endif
