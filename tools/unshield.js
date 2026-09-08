'use strict';

/**
 * Розпакувальник архівів InstallShield Cabinet (ISc) та Nokia DPC для Node.js.
 * 
 * Підтримує читання data1.hdr / data1.cab / data2.cab, розрізані файли між томами (SPLIT),
 * маскування (obfuscation), zlib deflate стиснення, а також вкладені архіви Nokia Data Package (DPC/LZMA).
 * 
 * Не має зовнішніх залежностей (тільки стандартні модулі Node.js: fs, path, zlib).
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

// Сигнатури форматів
const CAB_SIGNATURE = 0x28635349;  // 'ISc('
const MSCF_SIGNATURE = 0x4643534d; // 'MSCF' (Microsoft Cabinet)

// Прапорці файлів InstallShield (з cabfile.h)
const FILE_SPLIT = 0x01;
const FILE_OBFUSCATED = 0x02;
const FILE_COMPRESSED = 0x04;
const FILE_INVALID = 0x08;

// Прапорці зв'язків
const LINK_NONE = 0;
const LINK_PREV = 1;
const LINK_NEXT = 2;
const LINK_BOTH = 3;

/**
 * Отримання головної версії формату InstallShield.
 */
function getMajorVersion(version) {
  const topByte = (version >>> 24) & 0xff;
  if (topByte === 1) {
    return (version >>> 12) & 0x0f;
  } else if (topByte === 2 || topByte === 4) {
    const lowWord = version & 0xffff;
    return lowWord !== 0 ? Math.floor(lowWord / 100) : 5;
  } else if (topByte === 0) {
    return (version >>> 12) & 0x0f || 5;
  }
  return 5;
}

/**
 * Читання рядка з буфера за зміщенням.
 */
function readNullTerminatedString(buffer, offset, isUtf16 = false) {
  if (offset >= buffer.length) return '';
  if (isUtf16) {
    let end = offset;
    while (end + 1 < buffer.length && (buffer[end] !== 0 || buffer[end + 1] !== 0)) {
      end += 2;
    }
    return buffer.toString('utf16le', offset, end);
  } else {
    let end = offset;
    while (end < buffer.length && buffer[end] !== 0) {
      end++;
    }
    return buffer.toString('utf8', offset, end);
  }
}

/**
 * Пошук файлу в теці без урахування регістру символів.
 */
function findFileCaseInsensitive(dir, targetName) {
  if (!fs.existsSync(dir)) return null;
  const lower = targetName.toLowerCase();
  const entries = fs.readdirSync(dir);
  for (const entry of entries) {
    if (entry.toLowerCase() === lower) {
      return path.join(dir, entry);
    }
  }
  return null;
}

/**
 * Демаскування даних (InstallShield deobfuscation).
 */
function deobfuscateBuffer(buffer, seedObj) {
  let s = seedObj.val;
  for (let i = 0; i < buffer.length; i++) {
    let b = buffer[i] ^ 0xd5;
    // циклічний зсув праворуч на 2 біти (ror8)
    b = ((b >>> 2) | ((b << 6) & 0xff)) & 0xff;
    b = (b - (s % 0x47)) & 0xff;
    buffer[i] = b;
    s++;
  }
  seedObj.val = s;
}

/**
 * Перевірка відповідності імені файлу масці (wildcard: * та ?).
 */
function matchMask(filename, mask) {
  if (!mask || mask === '*' || mask === '*.*') return true;
  const baseName = path.basename(filename);
  const regexStr = '^' + mask
    .replace(/[-[\]{}()+.,\\^$|#\s]/g, '\\$&')
    .replace(/\*/g, '.*')
    .replace(/\?/g, '.') + '$';
  const re = new RegExp(regexStr, 'i');
  return re.test(filename) || re.test(baseName);
}

/**
 * Потоковий декодер LZMA-1 для розпакування solid-архівів Nokia DPC.
 */
class StreamingLzmaDecoder {
  constructor(propsByte, dictSize) {
    this.pb = Math.floor(propsByte / 45);
    const rem = propsByte % 45;
    this.lp = Math.floor(rem / 9);
    this.lc = rem % 9;
    this.dictSize = Math.max(dictSize, 4096);
    this.ringSize = Math.max(this.dictSize, 0x1000000); // 16 МБ кільцевий буфер
    this.ring = Buffer.alloc(this.ringSize);
    this.ringPos = 0;
    this.totalOut = 0;

    const kNumStates = 12;
    const kNumPosStatesMax = 1 << this.pb;
    this.isMatch = new Uint16Array(kNumStates << 4).fill(1024);
    this.isRep = new Uint16Array(kNumStates).fill(1024);
    this.isRepG0 = new Uint16Array(kNumStates).fill(1024);
    this.isRepG1 = new Uint16Array(kNumStates).fill(1024);
    this.isRepG2 = new Uint16Array(kNumStates).fill(1024);
    this.isRep0Long = new Uint16Array(kNumStates << 4).fill(1024);

    this.posSlotDecoder = new Uint16Array(4 << 6).fill(1024);
    this.posDecoders = new Uint16Array(114 + 16).fill(1024);
    this.posAlignDecoder = new Uint16Array(16).fill(1024);
    this.litProbs = new Uint16Array(0x300 << (this.lc + this.lp)).fill(1024);

    class LenDecoder {
      constructor() {
        this.choice = new Uint16Array(2).fill(1024);
        this.low = new Uint16Array(kNumPosStatesMax << 3).fill(1024);
        this.mid = new Uint16Array(kNumPosStatesMax << 3).fill(1024);
        this.high = new Uint16Array(1 << 8).fill(1024);
      }
      decode(rangeDec, posState) {
        if (rangeDec.decodeBit(this.choice, 0) === 0) {
          return rangeDec.decodeBitTree(this.low, posState << 3, 3);
        }
        if (rangeDec.decodeBit(this.choice, 1) === 0) {
          return 8 + rangeDec.decodeBitTree(this.mid, posState << 3, 3);
        }
        return 16 + rangeDec.decodeBitTree(this.high, 0, 8);
      }
    }

    this.lenDecoder = new LenDecoder();
    this.repLenDecoder = new LenDecoder();
  }

  decodeStream(inBuf, inOffset, files, outputHandler) {
    let inPos = inOffset;
    const inEnd = inBuf.length;

    inPos++; // Перший байт у LZMA завжди 0
    let code = 0 >>> 0;
    for (let i = 0; i < 4; i++) {
      code = ((code << 8) | (inPos < inEnd ? inBuf[inPos++] : 0)) >>> 0;
    }
    let range = 0xffffffff >>> 0;

    const rangeDec = {
      decodeBit: (probs, index) => {
        const prob = probs[index];
        const bound = ((range >>> 11) * prob) >>> 0;
        if (code < bound) {
          range = bound;
          probs[index] = (prob + ((2048 - prob) >>> 5)) | 0;
          if (range < 0x01000000) {
            range = (range << 8) >>> 0;
            code = ((code << 8) | (inPos < inEnd ? inBuf[inPos++] : 0)) >>> 0;
          }
          return 0;
        } else {
          range = (range - bound) >>> 0;
          code = (code - bound) >>> 0;
          probs[index] = (prob - (prob >>> 5)) | 0;
          if (range < 0x01000000) {
            range = (range << 8) >>> 0;
            code = ((code << 8) | (inPos < inEnd ? inBuf[inPos++] : 0)) >>> 0;
          }
          return 1;
        }
      },
      decodeBitTree: (probs, baseIndex, numBits) => {
        let m = 1;
        for (let i = 0; i < numBits; i++) {
          m = (m << 1) | rangeDec.decodeBit(probs, baseIndex + m);
        }
        return m - (1 << numBits);
      },
      decodeReverseBitTree: (probs, baseIndex, numBits) => {
        let m = 1;
        let symbol = 0;
        for (let i = 0; i < numBits; i++) {
          const bit = rangeDec.decodeBit(probs, baseIndex + m);
          m = (m << 1) | bit;
          symbol |= bit << i;
        }
        return symbol;
      },
      decodeDirect: (numBits) => {
        let res = 0;
        for (let i = 0; i < numBits; i++) {
          range = (range >>> 1) >>> 0;
          code = (code - range) >>> 0;
          const bit = (code >>> 31) & 1;
          if (bit === 1) {
            code = (code + range) >>> 0;
          }
          res = (res << 1) | (1 - bit);
          if (range < 0x01000000) {
            range = (range << 8) >>> 0;
            code = ((code << 8) | (inPos < inEnd ? inBuf[inPos++] : 0)) >>> 0;
          }
        }
        return res;
      }
    };

    let state = 0;
    let rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;

    const posStateMask = (1 << this.pb) - 1;
    const literalPosMask = (1 << this.lp) - 1;
    const lc = this.lc;
    const ring = this.ring;
    const ringSize = this.ringSize;

    let fileIdx = 0;
    let fileBytesWritten = 0;
    let curFile = files[fileIdx];

    const CHUNK_SIZE = 65536;
    const chunkBuf = Buffer.alloc(CHUNK_SIZE);
    let chunkPos = 0;

    function flushChunk() {
      if (chunkPos > 0) {
        if (curFile && curFile.needed && outputHandler) {
          outputHandler(curFile, chunkBuf.subarray(0, chunkPos));
        }
        chunkPos = 0;
      }
    }

    let lastNeededFileIdx = -1;
    for (let i = 0; i < files.length; i++) {
      if (files[i].needed) lastNeededFileIdx = i;
    }

    while (fileIdx < files.length) {
      curFile = files[fileIdx];
      if (lastNeededFileIdx !== -1 && fileIdx > lastNeededFileIdx) {
        break;
      }

      const posState = this.totalOut & posStateMask;
      let outByte;

      if (rangeDec.decodeBit(this.isMatch, (state << 4) + posState) === 0) {
        const prevByte = this.totalOut > 0 ? ring[(this.ringPos - 1 + ringSize) % ringSize] : 0;
        const litState = ((this.totalOut & literalPosMask) << lc) + (prevByte >>> (8 - lc));
        const probsOffset = litState * 0x300;

        let symbol = 1;
        if (state < 7) {
          do {
            symbol = (symbol << 1) | rangeDec.decodeBit(this.litProbs, probsOffset + symbol);
          } while (symbol < 0x100);
        } else {
          let matchByte = ring[(this.ringPos - rep0 - 1 + ringSize) % ringSize];
          do {
            const matchBit = (matchByte >>> 7) & 1;
            matchByte <<= 1;
            const bit = rangeDec.decodeBit(this.litProbs, probsOffset + ((1 + matchBit) << 8) + symbol);
            symbol = (symbol << 1) | bit;
            if (matchBit !== bit) {
              while (symbol < 0x100) {
                symbol = (symbol << 1) | rangeDec.decodeBit(this.litProbs, probsOffset + symbol);
              }
              break;
            }
          } while (symbol < 0x100);
        }

        outByte = symbol - 0x100;
        ring[this.ringPos] = outByte;
        this.ringPos = (this.ringPos + 1) % ringSize;
        this.totalOut++;

        if (curFile.needed) {
          chunkBuf[chunkPos++] = outByte;
          if (chunkPos === CHUNK_SIZE) flushChunk();
        }
        fileBytesWritten++;
        if (fileBytesWritten === curFile.size) {
          flushChunk();
          fileIdx++;
          fileBytesWritten = 0;
        }

        if (state < 4) state = 0;
        else if (state < 10) state -= 3;
        else state -= 6;
      } else {
        let len;
        if (rangeDec.decodeBit(this.isRep, state) === 1) {
          if (rangeDec.decodeBit(this.isRepG0, state) === 0) {
            if (rangeDec.decodeBit(this.isRep0Long, (state << 4) + posState) === 0) {
              state = state < 7 ? 9 : 11;
              outByte = ring[(this.ringPos - rep0 - 1 + ringSize) % ringSize];
              ring[this.ringPos] = outByte;
              this.ringPos = (this.ringPos + 1) % ringSize;
              this.totalOut++;

              if (curFile.needed) {
                chunkBuf[chunkPos++] = outByte;
                if (chunkPos === CHUNK_SIZE) flushChunk();
              }
              fileBytesWritten++;
              if (fileBytesWritten === curFile.size) {
                flushChunk();
                fileIdx++;
                fileBytesWritten = 0;
              }
              continue;
            }
          } else {
            let dist;
            if (rangeDec.decodeBit(this.isRepG1, state) === 0) {
              dist = rep1;
            } else {
              if (rangeDec.decodeBit(this.isRepG2, state) === 0) {
                dist = rep2;
              } else {
                dist = rep3;
                rep3 = rep2;
              }
              rep2 = rep1;
            }
            rep1 = rep0;
            rep0 = dist;
          }
          len = this.repLenDecoder.decode(rangeDec, posState) + 2;
          state = state < 7 ? 8 : 11;
        } else {
          rep3 = rep2;
          rep2 = rep1;
          rep1 = rep0;
          len = this.lenDecoder.decode(rangeDec, posState) + 2;
          state = state < 7 ? 7 : 10;

          const posSlot = rangeDec.decodeBitTree(this.posSlotDecoder, (len < 6 ? len - 2 : 3) << 6, 6);
          if (posSlot >= 4) {
            const numDirectBits = (posSlot >>> 1) - 1;
            rep0 = ((2 | (posSlot & 1)) << numDirectBits) >>> 0;
            if (posSlot < 14) {
              rep0 += rangeDec.decodeReverseBitTree(this.posDecoders, rep0 - posSlot - 1, numDirectBits);
            } else {
              rep0 += rangeDec.decodeDirect(numDirectBits - 4) << 4;
              rep0 += rangeDec.decodeReverseBitTree(this.posAlignDecoder, 0, 4);
            }
          } else {
            rep0 = posSlot;
          }
        }

        for (let i = 0; i < len && fileIdx < files.length; i++) {
          curFile = files[fileIdx];
          outByte = ring[(this.ringPos - rep0 - 1 + ringSize) % ringSize];
          ring[this.ringPos] = outByte;
          this.ringPos = (this.ringPos + 1) % ringSize;
          this.totalOut++;

          if (curFile.needed) {
            chunkBuf[chunkPos++] = outByte;
            if (chunkPos === CHUNK_SIZE) flushChunk();
          }
          fileBytesWritten++;
          if (fileBytesWritten === curFile.size) {
            flushChunk();
            fileIdx++;
            fileBytesWritten = 0;
          }
        }
      }
    }
    flushChunk();
  }
}

/**
 * Читач томів архіву InstallShield Cabinet.
 */
class CabinetReader {
  constructor(archiveDir, majorVersion) {
    this.archiveDir = archiveDir;
    this.majorVersion = majorVersion;
    this.openFds = new Map(); // volume -> fd
    this.volHeaders = new Map(); // volume -> header
  }

  close() {
    for (const fd of this.openFds.values()) {
      try { fs.closeSync(fd); } catch (e) {}
    }
    this.openFds.clear();
  }

  getVolumeFd(volume) {
    if (this.openFds.has(volume)) {
      return this.openFds.get(volume);
    }
    const cabName = `data${volume}.cab`;
    const cabPath = findFileCaseInsensitive(this.archiveDir, cabName);
    if (!cabPath) {
      throw new Error(`Том архіву ${cabName} не знайдено в теці ${this.archiveDir}`);
    }
    const fd = fs.openSync(cabPath, 'r');
    this.openFds.set(volume, fd);

    // Зчитуємо заголовок тому
    const commonBuf = Buffer.alloc(20);
    fs.readSync(fd, commonBuf, 0, 20, 0);
    const sig = commonBuf.readUInt32LE(0);
    if (sig !== CAB_SIGNATURE) {
      throw new Error(`Некоректний підпис тома ${cabName}: 0x${sig.toString(16)}`);
    }

    let volHeader;
    if (this.majorVersion <= 5) {
      const v5Buf = Buffer.alloc(40);
      fs.readSync(fd, v5Buf, 0, 40, 20);
      volHeader = {
        dataOffset: v5Buf.readUInt32LE(0),
        firstFileIndex: v5Buf.readUInt32LE(8),
        lastFileIndex: v5Buf.readUInt32LE(12),
        firstFileOffset: v5Buf.readUInt32LE(16),
        firstFileSizeExpanded: v5Buf.readUInt32LE(20),
        firstFileSizeCompressed: v5Buf.readUInt32LE(24),
        lastFileOffset: v5Buf.readUInt32LE(28),
        lastFileSizeExpanded: v5Buf.readUInt32LE(32),
        lastFileSizeCompressed: v5Buf.readUInt32LE(36),
      };
    } else {
      const v6Buf = Buffer.alloc(64);
      fs.readSync(fd, v6Buf, 0, 64, 20);
      volHeader = {
        dataOffset: v6Buf.readUInt32LE(0),
        firstFileIndex: v6Buf.readUInt32LE(8),
        lastFileIndex: v6Buf.readUInt32LE(12),
        firstFileOffset: v6Buf.readUInt32LE(16),
        firstFileSizeExpanded: v6Buf.readUInt32LE(24),
        firstFileSizeCompressed: v6Buf.readUInt32LE(32),
        lastFileOffset: v6Buf.readUInt32LE(40),
        lastFileSizeExpanded: v6Buf.readUInt32LE(48),
        lastFileSizeCompressed: v6Buf.readUInt32LE(56),
      };
    }
    this.volHeaders.set(volume, volHeader);
    return fd;
  }

  /**
   * Створення потокового читача для конкретного дескриптора файлу.
   */
  createStreamReader(fileDesc) {
    let curVolume = fileDesc.volume;
    let curFd = this.getVolumeFd(curVolume);
    let volHeader = this.volHeaders.get(curVolume);

    let curOffset = 0;
    let volumeBytesLeft = 0;

    if (fileDesc.flags & FILE_SPLIT) {
      if (fileDesc.index === volHeader.lastFileIndex && volHeader.lastFileOffset !== 0) {
        curOffset = volHeader.lastFileOffset;
        volumeBytesLeft = (fileDesc.flags & FILE_COMPRESSED) ? volHeader.lastFileSizeCompressed : volHeader.lastFileSizeExpanded;
      } else if (fileDesc.index === volHeader.firstFileIndex) {
        curOffset = volHeader.firstFileOffset;
        volumeBytesLeft = (fileDesc.flags & FILE_COMPRESSED) ? volHeader.firstFileSizeCompressed : volHeader.firstFileSizeExpanded;
      } else {
        curOffset = fileDesc.dataOffset;
        volumeBytesLeft = (fileDesc.flags & FILE_COMPRESSED) ? fileDesc.compressedSize : fileDesc.expandedSize;
      }
    } else {
      curOffset = fileDesc.dataOffset;
      volumeBytesLeft = (fileDesc.flags & FILE_COMPRESSED) ? fileDesc.compressedSize : fileDesc.expandedSize;
    }

    const seedObj = { val: 0 };
    const self = this;

    return {
      read(destBuf, count) {
        let bytesRemaining = count;
        let destPos = 0;

        while (bytesRemaining > 0) {
          const bytesToRead = Math.min(bytesRemaining, volumeBytesLeft);
          if (bytesToRead <= 0) {
            throw new Error(`Неможливо прочитати дані для файлу ${fileDesc.name}: bytesToRead <= 0`);
          }

          fs.readSync(curFd, destBuf, destPos, bytesToRead, curOffset);
          curOffset += bytesToRead;
          volumeBytesLeft -= bytesToRead;
          destPos += bytesToRead;
          bytesRemaining -= bytesToRead;

          if (bytesRemaining > 0) {
            // Переходимо до наступного тому
            curVolume++;
            curFd = self.getVolumeFd(curVolume);
            volHeader = self.volHeaders.get(curVolume);
            curOffset = volHeader.firstFileOffset;
            volumeBytesLeft = (fileDesc.flags & FILE_COMPRESSED) ? volHeader.firstFileSizeCompressed : volHeader.firstFileSizeExpanded;
          }
        }

        if (fileDesc.flags & FILE_OBFUSCATED) {
          deobfuscateBuffer(destBuf.subarray(0, count), seedObj);
        }
      }
    };
  }
}

/**
 * Парсер заголовка InstallShield (data1.hdr).
 */
class InstallShieldCabinet {
  constructor(disk1Path) {
    this.disk1Path = path.resolve(disk1Path);
    let hdrFile = null;

    if (fs.statSync(this.disk1Path).isDirectory()) {
      this.baseDir = this.disk1Path;
      hdrFile = findFileCaseInsensitive(this.baseDir, 'data1.hdr');
      if (!hdrFile) {
        hdrFile = findFileCaseInsensitive(this.baseDir, 'data1.cab');
      }
    } else {
      hdrFile = this.disk1Path;
      this.baseDir = path.dirname(this.disk1Path);
    }

    if (!hdrFile || !fs.existsSync(hdrFile)) {
      throw new Error(`Файл заголовка data1.hdr не знайдено в ${this.disk1Path}`);
    }

    this.hdrPath = hdrFile;
    this.hdrData = fs.readFileSync(this.hdrPath);
    this.parseHeader();
  }

  parseHeader() {
    const hdr = this.hdrData;
    if (hdr.length < 20) {
      throw new Error('Файл заголовка занадто малий');
    }

    const sig = hdr.readUInt32LE(0);
    if (sig === MSCF_SIGNATURE) {
      throw new Error('Виявлено заголовок Microsoft Cabinet (MSCF). Використовуйте cabextract.');
    }
    if (sig !== CAB_SIGNATURE) {
      throw new Error(`Невідома сигнатура архіву: 0x${sig.toString(16)} (очікувалося ISc/0x28635349)`);
    }

    this.versionRaw = hdr.readUInt32LE(4);
    this.majorVersion = getMajorVersion(this.versionRaw);
    this.cabDescriptorOffset = hdr.readUInt32LE(12);
    this.cabDescriptorSize = hdr.readUInt32LE(16);

    if (this.cabDescriptorSize === 0) {
      throw new Error('Описовий блок CAB descriptor відсутній у заголовку');
    }

    // Читання CabDescriptor
    const descOff = this.cabDescriptorOffset;
    let p = descOff + 0x0c;
    this.fileTableOffset = hdr.readUInt32LE(p); p += 4;
    p += 4;
    this.fileTableSize = hdr.readUInt32LE(p); p += 4;
    this.fileTableSize2 = hdr.readUInt32LE(p); p += 4;
    this.directoryCount = hdr.readUInt32LE(p); p += 4;
    p += 8;
    this.fileCount = hdr.readUInt32LE(p); p += 4;
    this.fileTableOffset2 = hdr.readUInt32LE(p); p += 4;

    const baseStringOffset = descOff + this.fileTableOffset;
    this.baseStringOffset = baseStringOffset;
    const isUtf16 = this.majorVersion >= 17;

    // Таблиця директорій
    this.directories = [];
    for (let i = 0; i < this.directoryCount; i++) {
      const dirStrOff = hdr.readUInt32LE(baseStringOffset + i * 4);
      this.directories.push(readNullTerminatedString(hdr, baseStringOffset + dirStrOff, isUtf16));
    }

    // Читання дескрипторів файлів
    this.files = [];
    if (this.majorVersion <= 5) {
      const fileTablePtr = baseStringOffset + this.directoryCount * 4;
      for (let i = 0; i < this.fileCount; i++) {
        const entryRelOff = hdr.readUInt32LE(fileTablePtr + i * 4);
        const ep = baseStringOffset + entryRelOff;

        const nameOffset = hdr.readUInt32LE(ep);
        const dirIndex = hdr.readUInt16LE(ep + 4);
        const flags = hdr.readUInt16LE(ep + 8);
        const expandedSize = hdr.readUInt32LE(ep + 10);
        const compressedSize = hdr.readUInt32LE(ep + 14);
        const dataOffset = hdr.readUInt32LE(ep + 38);

        const name = readNullTerminatedString(hdr, baseStringOffset + nameOffset, isUtf16);
        const dir = (dirIndex < this.directories.length) ? this.directories[dirIndex] : '';

        this.files.push({
          index: i,
          name,
          dir,
          dirIndex,
          flags,
          expandedSize,
          compressedSize,
          dataOffset,
          volume: 1,
          linkFlags: LINK_NONE,
          linkPrev: 0,
          linkNext: 0
        });
      }
    } else {
      // v6..v13+ (кожен дескриптор 0x57 = 87 байтів)
      for (let i = 0; i < this.fileCount; i++) {
        const ep = baseStringOffset + this.fileTableOffset2 + i * 0x57;
        const flags = hdr.readUInt16LE(ep);
        const expandedSize = Number(hdr.readBigUInt64LE(ep + 2));
        const compressedSize = Number(hdr.readBigUInt64LE(ep + 10));
        const dataOffset = Number(hdr.readBigUInt64LE(ep + 18));
        const nameOffset = hdr.readUInt32LE(ep + 0x3a);
        const dirIndex = hdr.readUInt16LE(ep + 0x3e);
        const linkPrev = hdr.readUInt32LE(ep + 0x4c);
        const linkNext = hdr.readUInt32LE(ep + 0x50);
        const linkFlags = hdr.readUInt8(ep + 0x54);
        const volume = hdr.readUInt16LE(ep + 0x55);

        const name = readNullTerminatedString(hdr, baseStringOffset + nameOffset, isUtf16);
        const dir = (dirIndex < this.directories.length) ? this.directories[dirIndex] : '';

        this.files.push({
          index: i,
          name,
          dir,
          dirIndex,
          flags,
          expandedSize,
          compressedSize,
          dataOffset,
          volume,
          linkFlags,
          linkPrev,
          linkNext
        });
      }
    }
  }

  /**
   * Отримання дійсних файлів (відфільтровані невалідні та нульові заглушки).
   */
  getValidFiles() {
    return this.files.filter(f => !(f.flags & FILE_INVALID) && f.dataOffset !== 0 && f.expandedSize > 0);
  }

  /**
   * Розпакування одного файлу InstallShield у буфер або запис у файл.
   */
  extractFileToBuffer(fileDesc, reader) {
    const streamReader = reader.createStreamReader(fileDesc);
    const outBuf = Buffer.alloc(fileDesc.expandedSize);
    let outPos = 0;

    if (fileDesc.flags & FILE_COMPRESSED) {
      let compressedBytesLeft = fileDesc.compressedSize;
      const hBuf = Buffer.alloc(2);

      while (compressedBytesLeft > 0) {
        streamReader.read(hBuf, 2);
        const chunkSize = hBuf.readUInt16LE(0);
        if (chunkSize === 0) {
          throw new Error(`Розмір стисненого чанка 0 для файлу ${fileDesc.name}`);
        }

        const compBuf = Buffer.alloc(chunkSize);
        streamReader.read(compBuf, chunkSize);
        compressedBytesLeft -= (2 + chunkSize);

        const decompChunk = zlib.inflateRawSync(compBuf);
        decompChunk.copy(outBuf, outPos);
        outPos += decompChunk.length;
      }
    } else {
      streamReader.read(outBuf, fileDesc.expandedSize);
      outPos = fileDesc.expandedSize;
    }

    return outBuf.subarray(0, outPos);
  }

  /**
   * Повний список усіх файлів (включаючи вміст контейнерів Nokia DPC).
   */
  getAllEntries(reader) {
    const validFiles = this.getValidFiles();
    const result = [];

    for (const f of validFiles) {
      if (f.name.toLowerCase().endsWith('.dpc') || f.name.toLowerCase() === 'compressed_files.dpc') {
        // Читаємо DPC заголовок
        const dpcData = this.extractFileToBuffer(f, reader);
        const dpcEntries = parseDpcHeader(dpcData);
        for (const de of dpcEntries.files) {
          result.push({
            name: de.name,
            dir: f.dir,
            expandedSize: de.size,
            compressedSize: Math.round(f.compressedSize * (de.size / dpcEntries.totalSize)),
            volume: `data${f.volume}`,
            isDpc: true,
            dpcParent: f,
            dpcInfo: dpcEntries
          });
        }
      } else {
        result.push({
          name: f.name,
          dir: f.dir,
          expandedSize: f.expandedSize,
          compressedSize: f.compressedSize,
          volume: `data${f.volume}`,
          isDpc: false,
          rawDesc: f
        });
      }
    }
    return result;
  }
}

/**
 * Читання списку файлів із заголовка Nokia DPC.
 */
function parseDpcHeader(dpcBuf) {
  let p = 0;
  const propsByte = dpcBuf[p++];
  const dictSize = dpcBuf.readUInt32LE(p); p += 4;
  const numFiles = dpcBuf.readUInt32LE(p); p += 4;

  const files = [];
  let totalSize = 0;
  for (let i = 0; i < numFiles; i++) {
    const nameLen = dpcBuf.readUInt32LE(p); p += 4;
    const name = dpcBuf.toString('utf8', p, p + nameLen); p += nameLen;
    const extra = dpcBuf.readUInt32LE(p); p += 4;
    const size = Number(dpcBuf.readBigUInt64LE(p)); p += 8;
    files.push({ name, extra, size });
    totalSize += size;
  }

  return {
    propsByte,
    dictSize,
    files,
    totalSize,
    lzmaOffset: p,
    rawBuffer: dpcBuf
  };
}

/**
 * Команда: node tools/unshield.js list <шлях до теки Disk1>
 */
function cmdList(disk1Path) {
  const cab = new InstallShieldCabinet(disk1Path);
  const reader = new CabinetReader(cab.baseDir, cab.majorVersion);

  try {
    const entries = cab.getAllEntries(reader);

    console.log(`Архів InstallShield Cabinet (версія ISc ${cab.majorVersion}, заголовок 0x${cab.versionRaw.toString(16)})`);
    console.log(`Знайдено ${entries.length} файлів:\n`);

    const colNameW = 45;
    const colExpW = 16;
    const colCompW = 16;
    const colVolW = 8;

    const headerLine = 'Ім\'я'.padEnd(colNameW) +
      'Розпакований'.padStart(colExpW) +
      'Стиснений'.padStart(colCompW) +
      '  Том'.padEnd(colVolW);
    console.log(headerLine);
    console.log('-'.repeat(headerLine.length));

    for (const e of entries) {
      const displayName = e.dir ? path.join(e.dir, e.name) : e.name;
      const expStr = e.expandedSize.toLocaleString('uk-UA');
      const compStr = e.compressedSize ? e.compressedSize.toLocaleString('uk-UA') : '-';
      console.log(
        displayName.padEnd(colNameW) +
        expStr.padStart(colExpW) +
        compStr.padStart(colCompW) +
        '  ' + e.volume.padEnd(colVolW)
      );
    }

    console.log('-'.repeat(headerLine.length));
    console.log(`Всього файлів: ${entries.length}`);
  } finally {
    reader.close();
  }
}

/**
 * Команда: node tools/unshield.js extract <шлях до теки Disk1> <тека призначення> [маска]
 */
function cmdExtract(disk1Path, destDir, mask) {
  const cab = new InstallShieldCabinet(disk1Path);
  const reader = new CabinetReader(cab.baseDir, cab.majorVersion);

  if (!fs.existsSync(destDir)) {
    fs.mkdirSync(destDir, { recursive: true });
  }

  let extractedCount = 0;

  try {
    const validFiles = cab.getValidFiles();
    const dpcCache = new Map();

    for (const f of validFiles) {
      if (f.name.toLowerCase().endsWith('.dpc') || f.name.toLowerCase() === 'compressed_files.dpc') {
        // Перевіряємо, чи є всередині DPC файли під нашу маску
        let dpcInfo = dpcCache.get(f.name);
        if (!dpcInfo) {
          const dpcData = cab.extractFileToBuffer(f, reader);
          dpcInfo = parseDpcHeader(dpcData);
          dpcCache.set(f.name, dpcInfo);
        }

        const matchingFiles = dpcInfo.files.map(df => ({
          ...df,
          needed: matchMask(df.name, mask)
        }));

        const hasNeeded = matchingFiles.some(mf => mf.needed);
        if (hasNeeded) {
          const lzmaDec = new StreamingLzmaDecoder(dpcInfo.propsByte, dpcInfo.dictSize);
          const openOutFds = new Map();

          lzmaDec.decodeStream(dpcInfo.rawBuffer, dpcInfo.lzmaOffset, matchingFiles, (curFile, chunk) => {
            let outFd = openOutFds.get(curFile.name);
            if (!outFd) {
              const outPath = path.join(destDir, curFile.name);
              console.log(`Розпакування: ${curFile.name} (${curFile.size.toLocaleString('uk-UA')} байтів)...`);
              outFd = fs.openSync(outPath, 'w');
              openOutFds.set(curFile.name, outFd);
            }
            fs.writeSync(outFd, chunk, 0, chunk.length);
          });

          for (const fd of openOutFds.values()) {
            fs.closeSync(fd);
            extractedCount++;
          }
        }
      } else {
        // Звичайний файл ISc
        if (matchMask(f.name, mask)) {
          console.log(`Розпакування: ${f.name} (${f.expandedSize.toLocaleString('uk-UA')} байтів)...`);
          const outBuf = cab.extractFileToBuffer(f, reader);
          const outSubdir = f.dir ? path.join(destDir, f.dir) : destDir;
          if (!fs.existsSync(outSubdir)) {
            fs.mkdirSync(outSubdir, { recursive: true });
          }
          const outPath = path.join(outSubdir, f.name);
          fs.writeFileSync(outPath, outBuf);
          extractedCount++;
        }
      }
    }

    console.log(`\nГотово! Успішно розпаковано ${extractedCount} файл(ів) у ${destDir}`);
  } finally {
    reader.close();
  }
}

/**
 * Головна точка входу CLI.
 */
function main() {
  const args = process.argv.slice(2);
  if (args.length < 2) {
    console.log('Використання:');
    console.log('  node tools/unshield.js list <шлях до теки Disk1>');
    console.log('  node tools/unshield.js extract <шлях до теки Disk1> <тека призначення> [маска]');
    process.exit(1);
  }

  const action = args[0].toLowerCase();
  const disk1Path = args[1];

  try {
    if (action === 'list' || action === 'l') {
      cmdList(disk1Path);
    } else if (action === 'extract' || action === 'x') {
      if (args.length < 3) {
        console.error('Помилка: не вказано теку призначення для розпакування.');
        process.exit(1);
      }
      const destDir = args[2];
      const mask = args[3] || null;
      cmdExtract(disk1Path, destDir, mask);
    } else {
      console.error(`Невідома дія: ${action}`);
      console.error('Дозволені дії: list, extract');
      process.exit(1);
    }
  } catch (err) {
    console.error(`Помилка: ${err.message}`);
    process.exit(1);
  }
}

if (require.main === module) {
  main();
}

module.exports = {
  InstallShieldCabinet,
  CabinetReader,
  StreamingLzmaDecoder,
  matchMask
};
