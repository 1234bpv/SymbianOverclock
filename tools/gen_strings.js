// Генератор таблиць локалізації: src/strings.json -> src/ocstr.h, src/ocstr.cpp
//
// Навіщо генератор, а не текст прямо в коді. GCCE старий, і покладатись на те,
// що він правильно перетворить UTF-8 усередині L"..." у UTF-16, підстав немає:
// помилка тут була б тихою і вилізла б лише на екрані апарата. Тому в .cpp
// їдуть самі екрани виду backslash-x-XXXX, тобто чистий ASCII, а читабельне
// джерело лишається в JSON. Кожен рядок продубльовано коментарем.
//
// Мова може бути перекладена ЧАСТКОВО. Відсутній рядок стає NULL у таблиці, і
// OcStr підставляє англійський. Це свідомий вибір: службові формати з іменами
// регістрів не варті перекладу п'ятнадцятьма мовами, а текст, який пояснює
// небезпеку, має бути правильним - тож довідка лишається лише тими мовами, де
// її написано, а не машинно розмноженою.
//
// Запуск: node tools/gen_strings.js
'use strict';
const fs = require('fs');

const data = JSON.parse(fs.readFileSync('src/strings.json', 'utf8'));
const langs = data.languages;          // перша мова - базова (англійська)
const base = langs[0];
const keys = Object.keys(data.strings);

// Усі символи екрануємо однаково. Змішувати літерали з екранами не можна: у C
// escape-послідовність жадібна і з'їла б наступну шістнадцяткову цифру тексту.
const BS = String.fromCharCode(92);
function esc(s) {
    let out = '';
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c > 0xffff) throw new Error('поза BMP: ' + s);
        out += BS + 'x' + c.toString(16).padStart(4, '0');
    }
    return out;
}
function cmt(s) { return s.replace(/\*\//g, '* /'); }

// Symbian TLanguage для кожного коду. Мова без відповідника тут не збереться -
// це навмисно, щоб мовчки не з'явилась таблиця, до якої неможливо дійти.
const TLANG = {
    en: 'ELangEnglish',    uk: 'ELangUkrainian', ru: 'ELangRussian',
    pl: 'ELangPolish',     de: 'ELangGerman',    fr: 'ELangFrench',
    es: 'ELangSpanish',    it: 'ELangItalian',   pt: 'ELangPortuguese',
    cs: 'ELangCzech',      nl: 'ELangDutch',     sv: 'ELangSwedish',
    tr: 'ELangTurkish',    ro: 'ELangRomanian',  hu: 'ELangHungarian',
    el: 'ELangGreek',      fi: 'ELangFinnish',   da: 'ELangDanish',
    no: 'ELangNorwegian',  sk: 'ELangSlovak',    bg: 'ELangBulgarian',
    hr: 'ELangCroatian',   sl: 'ELangSlovenian', sr: 'ELangSerbian',
    et: 'ELangEstonian',   lv: 'ELangLatvian',   lt: 'ELangLithuanian',
    ca: 'ELangCatalan',
};

// Регіональні варіанти. Symbian розрізняє австрійську німецьку, бельгійську
// французьку, бразильську португальську й таке інше окремими константами, і
// апарат, налаштований на таку мову, без цієї таблиці мовчки отримував би
// англійську, хоча переклад у нас є. Ідею взято з PS1Emulator/src/symbian/i18n.cpp.
const ALIAS = {
    en: ['ELangAmerican', 'ELangAustralian', 'ELangCanadianEnglish',
         'ELangInternationalEnglish', 'ELangSouthAfricanEnglish',
         'ELangNewZealand'],
    de: ['ELangAustrian', 'ELangSwissGerman'],
    fr: ['ELangSwissFrench', 'ELangBelgianFrench', 'ELangCanadianFrench',
         'ELangInternationalFrench'],
    it: ['ELangSwissItalian'],
    pt: ['ELangBrazilianPortuguese'],
    es: ['ELangInternationalSpanish', 'ELangLatinAmericanSpanish'],
    nl: ['ELangBelgianFlemish'],
    sv: ['ELangFinlandSwedish'],
    tr: ['ELangCyprusTurkish'],
    el: ['ELangCyprusGreek'],
    no: ['ELangNorwegianNynorsk'],
};

let h = `// ЗГЕНЕРОВАНО tools/gen_strings.js з src/strings.json. Руками не правити.
#ifndef OCSTR_H
#define OCSTR_H

#include <e32std.h>

enum TOcStrId
    {
`;
keys.forEach(k => { h += `    EStr${k},\n`; });
h += `    EStrCount
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

#endif
`;
fs.writeFileSync('src/ocstr.h', h, 'utf8');

let c = `// ЗГЕНЕРОВАНО tools/gen_strings.js з src/strings.json. Руками не правити.
//
// Таблиці лежать у екранах виду backslash-x-XXXX навмисно: так вміст не
// залежить від того, як компілятор тлумачить кодування сирця. Читабельне
// джерело - src/strings.json.
//
// NULL у таблиці означає "цією мовою рядка немає" - OcStr віддасть англійський.

#include <e32std.h>
#include "ocstr.h"

`;
langs.forEach(lang => {
    c += `static const TUint16* const KOcStr_${lang}[EStrCount] =\n    {\n`;
    keys.forEach(k => {
        const v = data.strings[k][lang];
        if (v === undefined || v === null) {
            c += `    NULL,   // ${cmt(data.strings[k][base])}\n`;
        } else {
            c += `    // ${cmt(v)}\n    (const TUint16*)L"${esc(v)}",\n`;
        }
    });
    c += `    };\n\n`;
});
const helpLangs = langs.filter(l => data.help[l]);
helpLangs.forEach(lang => {
    const lines = data.help[lang];
    c += `static const TUint16* const KOcHelp_${lang}[] =\n    {\n`;
    lines.forEach(v => {
        c += `    // ${cmt(v)}\n    (const TUint16*)L"${esc(v)}",\n`;
    });
    c += `    };\nstatic const TInt KOcHelpCount_${lang} = ${lines.length};\n\n`;
});

c += `static const TUint16* const* gStr = KOcStr_${base};
static const TUint16* const* gHelp = KOcHelp_${base};
static TInt gHelpCount = KOcHelpCount_${base};
static const TUint16* gActiveName = (const TUint16*)L"${esc(base)}";

void OcStrInit()
    {
    switch (User::Language())
        {
`;
langs.slice(1).forEach(lang => {
    const sym = TLANG[lang];
    if (!sym) throw new Error('немає TLanguage для ' + lang);
    c += `        case ${sym}:
`;
    (ALIAS[lang] || []).forEach(a => { c += `        case ${a}:
`; });
    c += `            gStr = KOcStr_${lang};
`;
    if (data.help[lang])
        c += `            gHelp = KOcHelp_${lang};
            gHelpCount = KOcHelpCount_${lang};
`;
    c += `            gActiveName = (const TUint16*)L"${esc(lang)}";
            break;
`;
});
c += `        default:
            break;
        }
    }

TPtrC OcStr(TInt aId)
    {
    if (aId < 0 || aId >= EStrCount)
        return TPtrC();
    const TUint16* s = gStr[aId];
    if (!s)
        s = KOcStr_${base}[aId];      // часткова мова: добираємо англійським
    return TPtrC(s);
    }

TInt OcHelpLineCount()
    {
    return gHelpCount;
    }

TPtrC OcHelpLine(TInt aIndex)
    {
    if (aIndex < 0 || aIndex >= gHelpCount)
        return TPtrC();
    return TPtrC(gHelp[aIndex]);
    }

TInt OcStrLanguageCount()
    {
    return ${langs.length};
    }

TPtrC OcStrActiveLanguage()
    {
    return TPtrC(gActiveName);
    }
`;
fs.writeFileSync('src/ocstr.cpp', c, 'utf8');

const filled = {};
langs.forEach(l => filled[l] = keys.filter(k => data.strings[k][l] != null).length);
console.log('мов:', langs.length, '| рядків:', keys.length,
            '| довідка:', helpLangs.join(','));
console.log('заповнено:', langs.map(l => l + '=' + filled[l]).join(' '));
