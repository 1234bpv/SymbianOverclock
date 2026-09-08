'use strict';
// Faithful port of Symbian's Huffman deflate decompressor (KUidCompressionDeflate).
// Ported verbatim from Symbian OS source (elf2e32): huffman.cpp / inflate.cpp / huffman.h / inflate.h.
// All 32-bit unsigned semantics preserved with >>> and |0 masking.

// ---- constants (huffman.h / inflate.h) ----
const KMaxCodeLength = 27;
const KMetaCodes = KMaxCodeLength + 1;              // 28
const KDeflateLengthMag = 8, KDeflateDistanceMag = 12;
const ELiterals = 256, ELengths = (KDeflateLengthMag - 1) * 4;   // 28
const ESpecials = 1, EDistances = (KDeflateDistanceMag - 1) * 4; // 44
const ELitLens = ELiterals + ELengths + ESpecials;               // 285
const EEos = ELiterals + ELengths;                               // 284
const KDeflationCodes = ELitLens + EDistances;                   // 329
const KDeflateMinLength = 3;
const KDeflateMaxDistance = 1 << KDeflateDistanceMag;            // 4096
const KDeflateDistCodeBase = 0x200;                             // 512
const KHuffTerminate = 0x0001;
const KBranch1 = (4 << 16) >>> 0;                               // sizeof(TUint32)<<16

// Predefined decoding tree for the externalised code-length stream (huffman.cpp).
const HuffmanDecoding = new Uint32Array([
  0x0004006c, 0x00040064, 0x0004005c, 0x00040050, 0x00040044, 0x0004003c,
  0x00040034, 0x00040021, 0x00040023, 0x00040025, 0x00040027, 0x00040029,
  0x00040014, 0x0004000c, 0x00040035, 0x00390037, 0x00330031, 0x0004002b,
  0x002f002d, 0x001f001d, 0x001b0019, 0x00040013, 0x00170015, 0x0004000d,
  0x0011000f, 0x000b0009, 0x00070003, 0x00050001,
]);

// reverse the byte-order of a 32-bit value (huffman.cpp)
function reverse(aVal) {
  let v = ((aVal << 16) | (aVal >>> 16)) >>> 0;
  v = (v ^ aVal) >>> 0;
  v = (v & 0xff00ffff) >>> 0;
  aVal = ((aVal >>> 8) | (aVal << 24)) >>> 0;
  return (aVal ^ (v >>> 8)) >>> 0;
}

// ---- TBitInput ----
class TBitInput {
  constructor(buf) {
    // buf: Uint8Array (already the compressed slice). Length is in bits.
    this.buf = buf;
    this.Set(0, buf.length * 8, 0);
  }
  // read a 32-bit little-endian word at byte index i (word-aligned reads, matching *(TUint32*)ptr)
  word(byteIndex) {
    const b = this.buf;
    const i = byteIndex;
    // little-endian load of 4 bytes (out-of-range bytes read as 0; matches reading past end -> underflow guarded elsewhere)
    const b0 = i < b.length ? b[i] : 0;
    const b1 = i + 1 < b.length ? b[i + 1] : 0;
    const b2 = i + 2 < b.length ? b[i + 2] : 0;
    const b3 = i + 3 < b.length ? b[i + 3] : 0;
    return ((b0) | (b1 << 8) | (b2 << 16) | (b3 << 24)) >>> 0;
  }
  Set(byteBase, aLength, aOffset) {
    // Emulates TBitInput::Set. iPtr is a *word* cursor: we track it as a byte index (multiple of 4).
    let p = byteBase + (aOffset >> 3);
    aOffset &= 7;
    let ptrByte = p & ~3;              // word containing this byte
    aOffset += (p & 3) << 3;           // bit offset within the word
    if (aLength === 0) {
      this.iCount = 0;
    } else {
      this.iBits = (reverse(this.word(ptrByte)) << aOffset) >>> 0;
      ptrByte += 4;
      aOffset = 32 - aOffset;
      aLength -= aOffset;
      if (aLength < 0) aOffset += aLength;
      this.iCount = aOffset;
    }
    this.iRemain = aLength;
    this.iPtrByte = ptrByte;
  }
  UnderflowL() { throw new Error('HUFFMAN buffer underflow'); }
  ReadL1() {
    let c = this.iCount;
    let bits = this.iBits >>> 0;
    if (--c < 0) return this.ReadN(1);
    this.iCount = c;
    this.iBits = (bits << 1) >>> 0;
    return bits >>> 31;
  }
  ReadN(aSize) {
    if (!aSize) return 0;
    let val = 0;
    let bits = this.iBits >>> 0;
    this.iCount -= aSize;
    while (this.iCount < 0) {
      // val |= bits >> (32-(iCount+aSize)) << (-iCount)
      const sh1 = 32 - (this.iCount + aSize);
      if (this.iCount + aSize !== 0) {
        val = (val | (((bits >>> sh1) << (-this.iCount)) >>> 0)) >>> 0;
      }
      aSize = -this.iCount;
      if (this.iRemain > 0) {
        bits = reverse(this.word(this.iPtrByte)) >>> 0;
        this.iPtrByte += 4;
        this.iCount += 32;
        this.iRemain -= 32;
        if (this.iRemain < 0) this.iCount += this.iRemain;
      } else {
        this.UnderflowL();
        bits = this.iBits >>> 0;
        this.iCount -= aSize;
      }
    }
    this.iBits = aSize === 32 ? 0 : (bits << aSize) >>> 0;
    return (val | (bits >>> (32 - aSize))) >>> 0;
  }
  ReadL(aSize) { return aSize === undefined ? this.ReadL1() : this.ReadN(aSize); }
  // aTree: Uint32Array of the decode tree; treeBase: element index of the tree root.
  HuffmanL(tree, treeBase) {
    let idx = treeBase | 0;
    let huff = 0;
    do {
      idx += (huff >>> 16) >> 2;    // byte offset -> element offset
      huff = tree[idx] >>> 0;
      if (this.ReadL1() === 0) huff = (huff << 16) >>> 0;
    } while ((huff & 0x10000) === 0);
    return huff >>> 17;
  }
}

// ---- Huffman ----
function IsValid(aHuffman, base, aNumCodes) {
  let remain = (1 << KMaxCodeLength) >>> 0;
  let totlen = 0;
  for (let i = aNumCodes - 1; i >= 0; --i) {
    const len = aHuffman[base + i] | 0;
    if (len > 0) {
      totlen += len;
      if (len > KMaxCodeLength) return false;
      const c = (1 << (KMaxCodeLength - len)) >>> 0;
      if (c > remain) return false;
      remain = (remain - c) >>> 0;
    }
  }
  return remain === 0 || totlen <= 1;
}

// Restore code-length table from bit stream (huffman.cpp InternalizeL)
function InternalizeL(aInput, aHuffman, base, aNumCodes) {
  const list = new Uint8Array(KMetaCodes);
  for (let i = 0; i < KMetaCodes; ++i) list[i] = i;
  let last = 0;
  let p = 0;                 // index within [0,aNumCodes)
  let rl = 0;
  while (p + rl < aNumCodes) {
    let c = aInput.HuffmanL(HuffmanDecoding, 0);
    if (c < 2) {
      rl += rl + c + 1;
    } else {
      while (rl > 0) {
        if (p > aNumCodes) throw new Error('HUFFMAN invalid coding (rle overrun)');
        aHuffman[base + p++] = last; --rl;
      }
      --c;
      list[0] = last;
      last = list[c];
      // memmove(&list[1], &list[0], c)
      for (let k = c; k >= 1; --k) list[k] = list[k - 1];
      if (p > aNumCodes) throw new Error('HUFFMAN invalid coding (overrun)');
      aHuffman[base + p++] = last;
    }
  }
  while (rl > 0) {
    if (p > aNumCodes) throw new Error('HUFFMAN invalid coding (tail overrun)');
    aHuffman[base + p++] = last; --rl;
  }
}

// Build canonical decoding tree in place (huffman.cpp Decoding + HuffmanSubTree)
function Decoding(T, base, aNumCodes, aSymbolBase) {
  if (!IsValid(T, base, aNumCodes)) throw new Error('HUFFMAN invalid coding (decode)');
  const counts = new Int32Array(KMaxCodeLength);
  let codes = 0;
  for (let ii = 0; ii < aNumCodes; ++ii) {
    let len = T[base + ii] | 0;
    T[base + ii] = len;         // store length temporarily (low byte survives later OR)
    if (--len >= 0) { ++counts[len]; ++codes; }
  }
  const level = new Int32Array(KMaxCodeLength);   // element indices (relative to base)
  let lit = codes;
  for (let ii = 0; ii < KMaxCodeLength; ++ii) { level[ii] = lit; lit -= counts[ii]; }

  const symBase = (((aSymbolBase << 17) >>> 0) + ((KHuffTerminate << 16) >>> 0)) >>> 0;
  for (let ii = 0; ii < aNumCodes; ++ii) {
    const len = T[base + ii] & 0xff;
    if (len) {
      const idx = --level[len - 1];
      T[base + idx] = (T[base + idx] | (((ii << 17) >>> 0) + symBase)) >>> 0;
    }
  }

  if (codes === 1) {
    const term = T[base + 0] >>> 16;
    T[base + 0] = ((term | (term << 16)) >>> 0);
  } else if (codes > 1) {
    // HuffmanSubTree(T + codes-1, T + codes-1, &level[0]) using element indices.
    // aLevel is passed BY VALUE in C++ (*aLevel++ advances only the local copy),
    // so levelIdx must be a by-value parameter, not shared mutable state.
    const st = { level, T, base };
    huffmanSubTree(st, codes - 1, codes - 1, 0);
  }
}

function huffmanSubTree(st, aPtr, aValue, levelIdx) {
  const l = st.level[levelIdx];       // *aLevel
  const next = levelIdx + 1;          // aLevel++ (by value, seen by both child calls)
  if (l > aValue) {
    const sub0 = huffmanSubTree(st, aPtr, aValue, next);
    aPtr = huffmanSubTree(st, sub0, aValue - (aPtr - sub0) - 1, next);
    const branch0 = ((sub0 - (aPtr - 1)) * 4) & 0xffffffff;   // byte diff
    st.T[st.base + (--aPtr)] = (KBranch1 | (branch0 >>> 0)) >>> 0;
  } else if (l === aValue) {
    const term0 = st.T[st.base + aValue] >>> 0; aValue -= 1;
    aPtr = huffmanSubTree(st, aPtr, aValue, next);
    st.T[st.base + (--aPtr)] = (KBranch1 | (term0 >>> 16)) >>> 0;
  } else {
    const term0 = st.T[st.base + aValue] >>> 0; aValue -= 1;
    const term1 = st.T[st.base + aValue] >>> 0; aValue -= 1;
    st.T[st.base + (--aPtr)] = ((((term1 >>> 16) << 16) >>> 0) | (term0 >>> 16)) >>> 0;
  }
  return aPtr;
}

// ---- CInflater ----
class CInflater {
  constructor(bits) {
    this.iBits = bits;
    // iEncoding: iLitLen[ELitLens] then iDistance[EDistances] laid out as one buffer for Internalize.
    this.enc = new Uint32Array(KDeflationCodes);  // 329
    this.InitL();
    this.iLen = 0;
    this.iOut = new Uint8Array(KDeflateMaxDistance);
    this.iRptr = 0;
    this.avail = 0; this.limit = 0;
  }
  InitL() {
    // Internalize both litlen and distance code lengths as one KDeflationCodes stream.
    InternalizeL(this.iBits, this.enc, 0, KDeflationCodes);
    if (!IsValid(this.enc, 0, ELitLens) || !IsValid(this.enc, ELitLens, EDistances))
      throw new Error('HUFFMAN invalid coding (encoding)');
    // Build the two trees in place.
    Decoding(this.enc, 0, ELitLens, 0);
    Decoding(this.enc, ELitLens, EDistances, KDeflateDistCodeBase);
    this.litBase = 0;
    this.distBase = ELitLens;
  }
  InflateL() {
    const out = this.iOut;
    const end = KDeflateMaxDistance;
    let o = 0;
    let treeBase = this.litBase;
    let fromHistory = false;
    if (this.iLen < 0) return 0;      // EOF
    if (this.iLen > 0) fromHistory = true;

    while (o < end) {
      if (!fromHistory) {
        let val = this.iBits.HuffmanL(this.enc, treeBase) - ELiterals;
        if (val < 0) { out[o++] = val & 0xff; continue; }
        if (val === EEos - ELiterals) { this.iLen = -1; break; }
        let code = val & 0xff;
        if (code >= 8) {
          const xtra = (code >> 2) - 1;
          code -= xtra << 2;
          code <<= xtra;
          code |= this.iBits.ReadL(xtra);
        }
        if (val < KDeflateDistCodeBase - ELiterals) {
          this.iLen = code + KDeflateMinLength;
          treeBase = this.distBase;
          continue;
        }
        // distance code
        this.iRptr = o - (code + 1);
        if (this.iRptr + KDeflateMaxDistance < end) this.iRptr += KDeflateMaxDistance;
      }
      fromHistory = false;
      // useHistory:
      let tfr = this.iLen > (end - o) ? (end - o) : this.iLen;
      this.iLen -= tfr;
      let from = this.iRptr;
      do {
        out[o++] = out[from++];
        if (from === end) from -= KDeflateMaxDistance;
      } while (--tfr !== 0);
      this.iRptr = from;
      treeBase = this.litBase;
    }
    return o;
  }
  ReadL(dest, aLength) {
    let tfr = 0;
    let dpos = 0;
    for (;;) {
      let len = aLength > (this.limit - this.avail) ? (this.limit - this.avail) : aLength;
      if (len) { dest.set(this.iOut.subarray(this.avail, this.avail + len), dpos); dpos += len; }
      aLength -= len; this.avail += len; tfr += len;
      if (aLength === 0) return tfr;
      len = this.InflateL();
      if (len === 0) return tfr;
      this.avail = 0; this.limit = len;
    }
  }
}

function inflate(compressed, destSize) {
  const bits = new TBitInput(compressed);
  const inf = new CInflater(bits);
  const dest = new Uint8Array(destSize);
  const n = inf.ReadL(dest, destSize);
  return { dest, n };
}

// ---- CLI ----
if (require.main === module) {
  const fs = require('fs');
  const [, , inPath, outPath, offStr, sizeStr] = process.argv;
  if (!inPath) { console.error('usage: node syminflate.js <in> <out> [srcOffset] [destSize]'); process.exit(2); }
  const raw = fs.readFileSync(inPath);
  // In a compressed E32 image the uncompressed part is oh->TotalSize() == iCodeOffset (0x64);
  // the deflate stream starts there. iUncompressedSize (0x7C) is the decompressed body length.
  const srcOff = offStr !== undefined ? parseInt(offStr, 10) : raw.readUInt32LE(0x64);
  const want = sizeStr !== undefined ? parseInt(sizeStr, 10) : raw.readUInt32LE(0x7C);
  console.error(`srcOffset=${srcOff} destSize=${want} (0x${want.toString(16)})`);
  const comp = new Uint8Array(raw.buffer, raw.byteOffset + srcOff, raw.length - srcOff);
  const { dest, n } = inflate(comp, want);
  console.error(`inflated ${n} bytes` + (n === want ? '  (matches iUncompressedSize)' : `  (EXPECTED ${want})`));
  if (outPath) {
    // reconstruct the full uncompressed E32 image: uncompressed header + inflated body
    const full = Buffer.concat([raw.subarray(0, srcOff), Buffer.from(dest.buffer, 0, n)]);
    fs.writeFileSync(outPath, full);
    console.error(`wrote full image ${full.length} bytes -> ${outPath} (header ${srcOff} + body ${n})`);
  }
}

module.exports = { inflate, TBitInput, CInflater };
