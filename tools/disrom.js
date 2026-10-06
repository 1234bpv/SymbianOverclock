// node tools/disrom.js <rom> <vaddrHex> <lenHex> [outbin]
const fs=require('fs'),cp=require('child_process');
const rom=process.argv[2], va=parseInt(process.argv[3],16), len=parseInt(process.argv[4],16);
const base=0x80000000; const off=va-base;
const b=fs.readFileSync(rom).slice(off,off+len);
const tmp=process.argv[5]||(require('os').tmpdir()+'/dis.bin'); fs.writeFileSync(tmp,b);
const OBJ=require("path").resolve(__dirname,"../../toolchain/CSL_Arm_Toolchain/arm-none-symbianelf/bin/objdump.exe");
const out=cp.execSync(`"${OBJ}" -D -b binary -m arm --adjust-vma=0x${va.toString(16)} "${tmp}"`,{maxBuffer:1<<28}).toString();
process.stdout.write(out.split('\n').filter(l=>/^\s*8[0-9a-f]{7}:/.test(l)).join('\n'));
