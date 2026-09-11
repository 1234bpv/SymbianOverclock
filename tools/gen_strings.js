// Генератор таблиць локалізації: src/strings.json -> src/ocstr.h, src/ocstr.cpp
//
// Навіщо генератор, а не текст прямо в коді. GCCE старий, і покладатись на те,
// що він правильно перетворить UTF-8 усередині L"..." у UTF-16, підстав немає:
// помилка тут була б тихою і вилізла б лише на екрані апарата. Тому в .cpp
// їдуть самі escape-послідовностях-екрани, тобто чистий ASCII, а читабельне джерело лишається
// в JSON. Кожен рядок продубльовано коментарем, щоб код лишався зрозумілим.
//
// Запуск: node tools/gen_strings.js
'use strict';
const fs = require('fs');

const data = JSON.parse(fs.readFileSync('src/strings.json', 'utf8'));
const langs = data.languages;
const keys = Object.keys(data.strings);

// Усі символи екрануємо однаково. Змішувати літерали з \x не можна: у C escape
// \x жадібний і з'їв би наступну шістнадцяткову цифру тексту.
function esc(s) {
    let out = '';
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c > 0xffff) throw new Error('поза BMP: ' + s);
        out += String.fromCharCode(92) + 'x' + c.toString(16).padStart(4, '0');
    }
    return out;
}
// Коментар мусить лишитись однорядковим і не закрити сам себе.
function cmt(s) { return s.replace(/\*\//g, '* /'); }

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
// Рядок активною мовою. Для дійсного id ніколи не порожній.
TPtrC OcStr(TInt aId);
// Вбудована довідка: скільки рядків і який рядок за номером.
TInt OcHelpLineCount();
TPtrC OcHelpLine(TInt aIndex);

#endif
`;
fs.writeFileSync('src/ocstr.h', h, 'utf8');

let c = `// ЗГЕНЕРОВАНО tools/gen_strings.js з src/strings.json. Руками не правити.
//
// Таблиці лежать у екранах виду backslash-x-XXXX навмисно: так вміст не залежить від того,
// як компілятор тлумачить кодування сирця. Читабельне джерело - src/strings.json.

#include <e32std.h>
#include "ocstr.h"

`;
langs.forEach(lang => {
    c += `static const TUint16* const KOcStr_${lang}[EStrCount] =\n    {\n`;
    keys.forEach(k => {
        const v = data.strings[k][lang];
        c += `    // ${cmt(v)}\n    (const TUint16*)L"${esc(v)}",\n`;
    });
    c += `    };\n\n`;
});
langs.forEach(lang => {
    const lines = data.help[lang];
    c += `static const TUint16* const KOcHelp_${lang}[] =\n    {\n`;
    lines.forEach(v => {
        c += `    // ${cmt(v)}\n    (const TUint16*)L"${esc(v)}",\n`;
    });
    c += `    };\nstatic const TInt KOcHelpCount_${lang} = ${lines.length};\n\n`;
});

c += `static const TUint16* const* gStr = KOcStr_${langs[0]};
static const TUint16* const* gHelp = KOcHelp_${langs[0]};
static TInt gHelpCount = KOcHelpCount_${langs[0]};

void OcStrInit()
    {
    switch (User::Language())
        {
`;
const map = { uk: 'ELangUkrainian' };
langs.slice(1).forEach(lang => {
    const sym = map[lang];
    if (!sym) throw new Error('немає TLanguage для ' + lang);
    c += `        case ${sym}:
            gStr = KOcStr_${lang};
            gHelp = KOcHelp_${lang};
            gHelpCount = KOcHelpCount_${lang};
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
    return TPtrC(gStr[aId]);
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
`;
fs.writeFileSync('src/ocstr.cpp', c, 'utf8');
console.log('ocstr.h/.cpp generated:', keys.length, 'strings,', langs.length, 'languages');
