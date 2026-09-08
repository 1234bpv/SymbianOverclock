// fpsx_extract.js - reassemble the XIP CORE ROM (SYM.ROM) from a BB5/FPSX MCU image.
//
// Faithful port of EKA2L1's open-source FPSX loader:
//   src/emu/loader/src/fpsx.cpp  +  src/tools/fpsxdump/src/main.cpp
// Purpose: get a disassemblable SYM.ROM for OMAP2420 devices we do NOT physically
// own, WITHOUT EKA2L1's crash-prone GUI installer. All vaddrs come from the ROM's
// own TRomImageHeaders after reassembly - nothing here is guessed.
//
// Usage:
//   node tools/fpsx_extract.js <mcu-image .C0R/.c00/.fpsx> [out.rom]
// Default output: <input dir>/SYM.ROM
//
// All multi-byte header fields in FPSX are BIG-ENDIAN.

const fs = require('fs');
const path = require('path');

// --- block / content / tlv type constants (from fpsx.h) ---
const CONTENT_TYPE_CODE = 84;      // 0x54
const CONTENT_TYPE_UNK1 = 86;      // 0x56
const CONTENT_TYPE_DATA_CERT = 93; // 0x5D

const BLOCK_TYPE_BINARY    = 0x17;
const BLOCK_TYPE_ROFS_HASH = 0x27;
const BLOCK_TYPE_CORE_CERT = 0x28;
const BLOCK_TYPE_H2E       = 0x2E;
const BLOCK_TYPE_H30       = 0x30;
const BLOCK_TYPE_H3A       = 0x3A;
const BLOCK_TYPE_H49       = 0x49;

// TLV entry types that carry a 16-bit big-endian length instead of 8-bit.
const TLV_LARGE = new Set([238 /*ARRAY*/, 228, 229, 234, 243]);

// A minimal forward-only cursor over the whole buffer.
class Cur {
    constructor(buf) { this.b = buf; this.p = 0; }
    tell() { return this.p; }
    seek(pos) { this.p = pos; }
    skip(n) { this.p += n; }
    valid() { return this.p < this.b.length; }
    u8() { const v = this.b[this.p]; this.p += 1; return v; }
    u16be() { const v = this.b.readUInt16BE(this.p); this.p += 2; return v; }
    u32be() { const v = this.b.readUInt32BE(this.p); this.p += 4; return v; }
    bytes(n) { const v = this.b.slice(this.p, this.p + n); this.p += n; return v; }
    cstr(n) { const raw = this.bytes(n); const z = raw.indexOf(0); return raw.slice(0, z < 0 ? n : z).toString('latin1'); }
}

// Read one TLV entry (header section). We only need to advance correctly.
function readTlv(c) {
    const type = c.u8();
    let size;
    if (TLV_LARGE.has(type)) { size = c.u16be(); } else { size = c.u8(); }
    c.skip(size);
    return type;
}

// Parse a block header body. `c` is positioned at the size_ byte (crr_pos).
// Returns { size_, data_size_, data_offset_to_extract_, description_ }.
// We read the fields we care about, then the caller re-seeks by size_ anyway,
// so exact field-by-field parity past what we read is not required.
function readBlockHeader(c, btype) {
    const start = c.tell();          // crr_pos, points at size_
    const size_ = c.u8();
    let data_size_ = 0, data_offset_to_extract_ = 0, description_ = '';

    switch (btype) {
        case BLOCK_TYPE_BINARY:
            c.u8();                   // flash_memory_
            c.u16be();                // unk2_
            c.u16be();                // data_checksum_
            c.u8();                   // padding_ (must be 1)
            data_size_ = c.u32be();
            data_offset_to_extract_ = c.u32be();
            break;
        case BLOCK_TYPE_ROFS_HASH:
            c.skip(20);               // cmt_root_key_hash_sha1_
            description_ = c.cstr(12);
            c.u8();                   // flash_memory_
            c.u16be();                // unk2_
            c.u16be();                // data_checksum_
            data_size_ = c.u32be();
            data_offset_to_extract_ = c.u32be();
            break;
        case BLOCK_TYPE_CORE_CERT:    // = rofs_hash + sha1(20) + unk2b(2)
            c.skip(20);               // cmt_root_key_hash_sha1_
            description_ = c.cstr(12);
            c.u8();                   // flash_memory_
            c.u16be();                // unk2_
            c.u16be();                // data_checksum_
            data_size_ = c.u32be();
            data_offset_to_extract_ = c.u32be();
            // remaining sha1(20)+unk2b(2) are inside size_; skipped by re-seek
            break;
        case BLOCK_TYPE_H2E:
            c.u8(); c.u16be(); c.u16be();
            description_ = c.cstr(12);
            data_size_ = c.u32be();
            data_offset_to_extract_ = c.u32be();
            break;
        case BLOCK_TYPE_H30:
            c.u8(); c.u16be(); c.u16be();
            c.skip(8);                // unk3_
            data_size_ = c.u32be();
            data_offset_to_extract_ = c.u32be();
            break;
        case BLOCK_TYPE_H3A:
            c.u8(); c.u16be(); c.u16be();
            c.skip(12);               // description_
            data_size_ = 0;           // H3A carries no payload
            data_offset_to_extract_ = 0;
            break;
        case BLOCK_TYPE_H49:
            data_size_ = c.u32be();
            c.u16be();                // data_checksum_
            c.skip(18);               // unk_
            data_offset_to_extract_ = c.u32be();
            break;
        default:
            throw new Error('Unrecognised block type 0x' + btype.toString(16));
    }
    return { start, size_, data_size_, data_offset_to_extract_, description_ };
}

// read_next_block: returns a block descriptor or null at end.
function readNextBlock(c) {
    if (c.tell() + 3 > c.b.length) return null;
    const ctype_ = c.u8();
    c.u8();                           // padding
    const btype_ = c.u8();
    const crr_pos = c.tell();
    let hdr;
    try { hdr = readBlockHeader(c, btype_); }
    catch (e) { return null; }        // unknown block -> stop, like the C++ (returns nullopt)
    // Skip to end of header, then the 1-byte block checksum.
    c.seek(crr_pos + hdr.size_ + 1);
    if (c.tell() >= c.b.length) return null;
    c.u8();                           // block_checksum8_
    const data_offset_in_stream_ = c.tell();
    // Blocks in these BB5/FPSX images are packed contiguously by data_size_
    // (verified against the RM-320 SYM.ROM oracle); no 512-byte alignment.
    c.skip(hdr.data_size_);
    return {
        ctype_, btype_,
        data_size_: hdr.data_size_,
        data_offset_to_extract_: hdr.data_offset_to_extract_,
        description_: hdr.description_,
        data_offset_in_stream_,
    };
}

// Port of block_tree_entry::add - groups a block by content type.
function entryAdd(entry, blck) {
    if (blck.ctype_ === CONTENT_TYPE_CODE) { entry.code.push(blck); return blck; }
    if (blck.ctype_ === CONTENT_TYPE_DATA_CERT) { entry.cert.push(blck); return blck; }
    if (blck.ctype_ === CONTENT_TYPE_UNK1) { entry.unk56.push(blck); return blck; }
    return null;
}
function newEntry() { return { code: [], cert: [], unk56: [] }; }

// Port of block_tree::add - decides which root a block belongs to.
function treeAdd(tree, blck) {
    if (!tree.current) {
        const e = newEntry(); tree.current = entryAdd(e, blck); tree.roots.push(e); return;
    }
    if (tree.current.ctype_ === CONTENT_TYPE_CODE && blck.ctype_ === CONTENT_TYPE_DATA_CERT) {
        const e = newEntry(); tree.current = entryAdd(e, blck); tree.roots.push(e); return;
    }
    const cur = tree.current;
    if (blck.data_offset_to_extract_ >= cur.data_offset_to_extract_ &&
        blck.data_offset_to_extract_ <= cur.data_offset_to_extract_ + cur.data_size_) {
        const last = tree.roots[tree.roots.length - 1];
        tree.current = entryAdd(last, blck);
        return;
    }
    const e = newEntry(); tree.current = entryAdd(e, blck); tree.roots.push(e);
}

function parseFpsx(buf) {
    const c = new Cur(buf);
    const magic = c.u8();
    if (magic < 176 || magic > 178) throw new Error('bad FPSX magic ' + magic);
    const header_size = c.u32be();
    const tlvNum = c.u32be();
    for (let i = 0; i < tlvNum; i++) readTlv(c);
    // Blocks begin at header_size + 5 (magic(1) + header_size(4)).
    c.seek(header_size + 5);
    const tree = { roots: [], current: null };
    while (c.valid()) {
        const blck = readNextBlock(c);
        if (!blck) break;
        treeAdd(tree, blck);
    }
    return tree;
}

// Is there an ARM XIP reset-vector table at buf[off]? word0 is a B (byte3==0xEA);
// the next `n` words are branches too (byte3 in {0xEA,0xEB}). Distinctive enough
// to locate the XIP ROM start and to tell an uncompressed XIP from a compressed
// blob. Returns true if the pattern holds.
function isResetVector(buf, off, n = 6) {
    if (off + 4 * (n + 1) > buf.length) return false;
    if (buf[off + 3] !== 0xEA) return false;              // reset: B
    for (let w = 1; w <= n; w++) {
        const b3 = buf[off + w * 4 + 3];
        if (b3 !== 0xEA && b3 !== 0xEB) return false;     // exception vectors: B/BL
    }
    return true;
}

// Concatenate a root's code blocks (contiguous by data_size_) into one buffer.
function concatRoot(buf, root) {
    return Buffer.concat(root.code.map(cb => buf.slice(cb.data_offset_in_stream_, cb.data_offset_in_stream_ + cb.data_size_)));
}

// Find the XIP ROM root by CONTENT, not by description string: the uncompressed
// root whose data carries an ARM reset-vector table near its start. Names vary
// across product lines - N95's XIP is "SOS+CORE", E90's is "SOS+UPDAPP" while its
// "SOS*CORE" is a compressed blob. Among the uncompressed XIP candidates we take
// the largest (the full OS XIP, not a small bootstrap/updater). Returns
// { root, xipStart } or null.
function findXipRoot(buf, tree, forceIdx) {
    if (forceIdx != null) {
        const root = tree.roots[forceIdx];
        const cc = concatRoot(buf, root);
        let s = 0; for (; s <= 0x2000 && s < cc.length; s += 4) if (isResetVector(cc, s)) break;
        return { root, xipStart: (s <= 0x2000 ? s : 0), concat: cc };
    }
    let best = null;
    for (const root of tree.roots) {
        if (root.code.length === 0) continue;
        // Probe the first code block for a reset vector within the first 0x2000 bytes
        // (N95 prefixes ~0xC00 of bootstrap before the vector table; E90 has none).
        const first = buf.slice(root.code[0].data_offset_in_stream_,
                                root.code[0].data_offset_in_stream_ + Math.min(root.code[0].data_size_, 0x2100));
        let start = -1;
        for (let s = 0; s + 0x1c <= first.length && s <= 0x2000; s += 4) {
            if (isResetVector(first, s)) { start = s; break; }
        }
        if (start < 0) continue;                          // not an uncompressed XIP
        const total = root.code.reduce((a, b) => a + b.data_size_, 0);
        if (!best || total > best.total) best = { root, xipStart: start, total };
    }
    if (!best) return null;
    return { root: best.root, xipStart: best.xipStart, concat: concatRoot(buf, best.root) };
}

// Extract the XIP ROM: the chosen root's concatenated code, trimmed to the XIP
// start (dropping any leading bootstrap before the reset vector).
function extractCore(buf, tree, forceIdx) {
    const xip = findXipRoot(buf, tree, forceIdx);
    if (!xip) return null;
    return { rom: xip.concat.slice(xip.xipStart), desc: xip.root.cert[0] ? xip.root.cert[0].description_ : '', xipStart: xip.xipStart };
}

function main() {
    const input = process.argv[2];
    if (!input) { console.error('usage: node fpsx_extract.js <mcu image> [out.rom]'); process.exit(1); }
    const out = process.argv[3] || path.join(path.dirname(input), 'SYM.ROM');
    // Optional: --root=N forces a specific root index (diagnostics).
    let forceIdx = null;
    for (const a of process.argv.slice(4)) { const m = /^--root=(\d+)$/.exec(a); if (m) forceIdx = parseInt(m[1], 10); }

    const buf = fs.readFileSync(input);
    const tree = parseFpsx(buf);
    console.error(`roots=${tree.roots.length}`);

    const res = extractCore(buf, tree, forceIdx);
    if (!res) { console.error('no uncompressed XIP root found (reset vector absent) - CORE may be compressed'); process.exit(2); }
    fs.writeFileSync(out, res.rom);
    console.error(`XIP root desc="${res.desc}" bootstrap=0x${res.xipStart.toString(16)}`);
    console.error(`wrote ${out}  ${res.rom.length} bytes (0x${res.rom.length.toString(16)})`);
}

main();
