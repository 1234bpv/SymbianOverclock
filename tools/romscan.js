// romscan.js - сигнатурний пошук машинерії розгону в СИРОМУ образі прошивки
// (MCU/CORE FPSX, розширення .C0R/.C0/.C01/.c00), без апарата й без емулятора.
//
// Працює тому, що ядровий бік ROM (ekern, power.dll, *.ldd/*.pdd) лежить у
// XIP CORE нестисненим і виконується прямо з флешу. Тому інструкції цих модулів
// присутні в образі як є, і їх можна знайти за сигнатурою. Стиснені секції
// ROFS (переважно user-side) сюди не потрапляють - це навмисна межа методу:
// romscan судить лише про ядрову частину, якої нам і треба.
//
// Що шукає:
//   1. Тіло dvfs_interface DoControl(1) - наш примітив читання частоти. Шість
//      слів, два з них адресозалежні, тож звіряються за маскою. Наявність = на
//      цю прошивку метод переноситься як є; відсутність = потрібен інший читач.
//   2. Літеральні пули PRCM - адреси регістрів тактування (CM_CLKSEL*_PLL,
//      CM_CLKSEL1_CORE, CM_CLKSEL_MPU, CM_CLKEN_PLL), упаковані поряд. Це хвіст
//      функції power.dll, яка пише клоки, тобто сам механізм розгону.
//
// Використання:  node tools/romscan.js <образ.C0R> [ще образи...]
//                node tools/romscan.js --json <образ>   (машиночитаний вивід)

const fs = require("fs");

function scan(path) {
  const b = fs.readFileSync(path);

  // 1) тіло DoControl(1): stmdb sp!,{r4,lr}; mov r0,#1; bl ?; ldr r1,[pc,#imm];
  //    ldmia sp!,{r4,lr}; b ?   (крок 2 байти: XIP може лежати зі зсувом mod4=2)

  // 3) наявність самого драйвера dvfs_interface за іменем. Імʼя ядрового
  //    модуля лежить у XIP і як ASCII (заголовок E32), і як UTF-16 (каталог
  //    ROM), тож знаходиться навіть тоді, коли тіло DoControl(1) скомпільовано
  //    інакше й сигнатура його не ловить. Це і є надійний тест "чи є драйвер".
  function hasDvfsName(){
    const a=b.indexOf(Buffer.from("dvfs_interface","latin1"));
    const u=b.indexOf(Buffer.from("dvfs_interface","utf16le"));
    return { ascii:a, u16:u, present:(a>=0||u>=0) };
  }
  const dname=hasDvfsName();

  const dvfs = [];
  for (let p = 0; p + 24 <= b.length; p += 2) {
    if (b.readUInt32LE(p) !== 0xe92d4010) continue;
    if (b.readUInt32LE(p + 4) !== 0xe3a00001) continue;
    if ((b.readUInt32LE(p + 8) >>> 24) !== 0xeb) continue;
    if (((b.readUInt32LE(p + 12) & 0xfffff000) >>> 0) !== 0xe59f1000) continue;
    if (b.readUInt32LE(p + 16) !== 0xe8bd4010) continue;
    if ((b.readUInt32LE(p + 20) >>> 24) !== 0xea) continue;
    dvfs.push(p);
  }

  // 2) літерали PRCM і їх скупчення (>=3 у межах 0x40 байтів = пул)
  const regs = [0xc6008540, 0xc6008544, 0xc6008140, 0xc6008500, 0xc6008240];
  const pos = [];
  for (let p = 0; p + 4 <= b.length; p += 2) {
    if (regs.includes(b.readUInt32LE(p) >>> 0)) pos.push(p);
  }
  pos.sort((x, y) => x - y);
  const clusters = [];
  let cur = [];
  for (const x of pos) {
    if (cur.length && x - cur[cur.length - 1] > 0x40) {
      if (cur.length >= 3) clusters.push(cur.slice());
      cur = [];
    }
    cur.push(x);
  }
  if (cur.length >= 3) clusters.push(cur);

  return { file: path.split(/[\/]/).pop(), size: b.length, dname,
           dvfs, prcm: pos.length, clusters: clusters.map(c => ({ at: c[0], n: c.length })) };
}

const args = process.argv.slice(2);
const json = args[0] === "--json";
const files = json ? args.slice(1) : args;
const out = [];
for (const f of files) {
  try { out.push(scan(f)); }
  catch (e) { out.push({ file: f, error: String(e.message || e) }); }
}
if (json) { console.log(JSON.stringify(out, null, 2)); process.exit(0); }
for (const r of out) {
  if (r.error) { console.log(r.file + "  ПОМИЛКА: " + r.error); continue; }
  const dv = r.dvfs.length
    ? "Є (x" + r.dvfs.length + " @0x" + r.dvfs.map(x => x.toString(16)).join(",0x") + ")"
    : "немає";
  const cl = r.clusters.length
    ? r.clusters.map(c => "@0x" + c.at.toString(16) + "(x" + c.n + ")").join(" ")
    : "немає";
  console.log(r.file);
  console.log("  драйвер dvfs_interface: " + (r.dname.present ? "Є" : "НЕМАЄ"));
  console.log("  тіло DoControl(1) (патч-сайт): " + dv);
  console.log("  PRCM-пул (механізм запису): " + cl + "  [літералів усього " + r.prcm + "]");
}
