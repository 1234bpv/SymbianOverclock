// romscan.js - signature search for overclock machinery in a RAW firmware image
// (MCU/CORE FPSX, extensions .C0R/.C0/.C01/.c00), without device and without emulator.
//
// Works because the kernel side of the ROM (ekern, power.dll, *.ldd/*.pdd) resides in
// XIP CORE uncompressed and executes directly from flash. Therefore instructions of these modules
// are present in the image as is, and can be found by signature. Compressed ROFS sections
// (mostly user-side) are not included here - this is an intentional boundary of the method:
// romscan inspects only the kernel part, which is what we need.
//
// What it looks for:
//   1. dvfs_interface DoControl(1) body - our clock reading primitive. Six
//      words, two of them are address-dependent, so matched by mask. Presence = the method
//      ports to this firmware as is; absence = a different reader is needed.
//   2. PRCM literal pools - clock register addresses (CM_CLKSEL*_PLL,
//      CM_CLKSEL1_CORE, CM_CLKSEL_MPU, CM_CLKEN_PLL), packed together. This is the tail
//      of the power.dll function that writes clocks, i.e. the overclock mechanism itself.
//
// Usage:  node tools/romscan.js <image.C0R> [more images...]
//         node tools/romscan.js --json <image>   (machine-readable output)

const fs = require("fs");

function scan(path) {
  const b = fs.readFileSync(path);

  // 1) DoControl(1) body: stmdb sp!,{r4,lr}; mov r0,#1; bl ?; ldr r1,[pc,#imm];
  //    ldmia sp!,{r4,lr}; b ?   (step 2 bytes: XIP can be located at mod4=2 offset)

  // 3) presence of the dvfs_interface driver itself by name. The name of the kernel
  //    module resides in XIP both as ASCII (E32 header) and UTF-16 (ROM directory),
  //    so it is found even when the DoControl(1) body is compiled differently and
  //    the signature does not catch it. This is the reliable test for "is driver present".
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

  // 2) PRCM literals and their clusters (>=3 within 0x40 bytes = pool)
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
  if (r.error) { console.log(r.file + "  ERROR: " + r.error); continue; }
  const dv = r.dvfs.length
    ? "yes (x" + r.dvfs.length + " @0x" + r.dvfs.map(x => x.toString(16)).join(",0x") + ")"
    : "none";
  const cl = r.clusters.length
    ? r.clusters.map(c => "@0x" + c.at.toString(16) + "(x" + c.n + ")").join(" ")
    : "none";
  console.log(r.file);
  console.log("  dvfs_interface driver: " + (r.dname.present ? "yes" : "no"));
  console.log("  DoControl(1) body (patch site): " + dv);
  console.log("  PRCM pool (write mechanism): " + cl + "  [literals in total " + r.prcm + "]");
}
