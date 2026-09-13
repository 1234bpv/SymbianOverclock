// Generates the application icon bitmaps in src/gfx/ from a vector description.
//
//   node tools/mkicon.js
//
// The icon is a speedometer: a light rounded plate, a metallic bezel, a dial
// band running blue -> violet -> red, a needle in the red zone and a flame at
// the hot end. Everything here is drawn from geometry, not scaled from one
// large image, because the three sizes S60 asks for (88, 44 and 22 pixels) need
// different amounts of detail. A faithful downscale of the 88-pixel drawing
// turns the bezel ticks into a grey smear and the flame into a blob; at 44 the
// ticks are therefore dropped, and at 22 the band widens and the needle and
// flame grow, so that the shape still reads.
//
// Output is 24-bit BMP plus a 24-bit mask per size, which is what mifconv and
// bmconv accept. White in the mask means opaque.
const fs = require('fs');
const path = require('path');

const CX = 0.5, CY = 0.5;
const A0 = 225, A1 = -25;                 // dial sweep in degrees, y upwards

const PLATE_T = [0xFB, 0xFD, 0xFF], PLATE_B = [0xE2, 0xEA, 0xF2];
const BEZ_T   = [0xE8, 0xEE, 0xF4], BEZ_B   = [0x6B, 0x78, 0x89];
const TICK    = [0x47, 0x53, 0x5F];
const FACE_T  = [0x28, 0x35, 0x46], FACE_B  = [0x0B, 0x13, 0x26];
const NEEDLE  = [0xEC, 0xF2, 0xF9];
const HUB     = [0xBA, 0xC9, 0xDA];
const FLAME_O = [0xF9, 0x70, 0x0D], FLAME_I = [0xFF, 0xB6, 0x06];

// Stops along the dial, sampled every ten degrees off the reference drawing.
const STOPS = [
    [0.00, [0x2E, 0x72, 0xDD]],
    [0.22, [0x2F, 0x74, 0xE0]],
    [0.55, [0xA0, 0x50, 0xC8]],
    [0.82, [0xFF, 0x3E, 0x39]],
    [1.00, [0xFF, 0x3E, 0x39]],
];

const mix = (a, b, f) => [0, 1, 2].map(k => a[k] + (b[k] - a[k]) * f);
const clamp01 = v => Math.min(1, Math.max(0, v));

function bandColour(t) {
    t = clamp01(t);
    for (let i = 0; i < STOPS.length - 1; ++i) {
        const [a, ca] = STOPS[i], [b, cb] = STOPS[i + 1];
        if (t <= b) return mix(ca, cb, (t - a) / (b - a));
    }
    return STOPS[STOPS.length - 1][1];
}

function inRoundedRect(x, y, x0, y0, x1, y1, r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    const cx = Math.min(Math.max(x, x0 + r), x1 - r);
    const cy = Math.min(Math.max(y, y0 + r), y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

function inTriangle(x, y, ax, ay, bx, by, cx, cy) {
    const s = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    const t = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    const u = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (s >= 0 && t >= 0 && u >= 0) || (s <= 0 && t <= 0 && u <= 0);
}

// detail 2: everything, for 88 px. 1: no bezel ticks, for 44 px.
// 0: wider band, bigger needle and flame, no tail, for 22 px.
function makeIcon(detail) {
    const BEZ_OUT  = 0.4375;
    const BEZ_IN   = detail === 0 ? 0.390 : 0.375;
    const B_OUT    = detail === 0 ? 0.375 : 0.350;
    const B_IN     = detail === 0 ? 0.225 : 0.245;
    const HUB_R    = detail === 0 ? 0.090 : 0.078;
    const NEEDLE_W = detail === 0 ? 0.052 : 0.034;
    const NEEDLE_R = detail === 0 ? 0.300 : 0.310;
    const FL_X = 0.718, FL_Y = detail === 0 ? 0.775 : 0.755;
    const FL_H = detail === 0 ? 0.265 : 0.195;

    const na = 43 * Math.PI / 180;
    const tipx = CX + Math.cos(na) * NEEDLE_R, tipy = CY - Math.sin(na) * NEEDLE_R;
    const tailx = CX - Math.cos(na) * HUB_R * 1.7, taily = CY + Math.sin(na) * HUB_R * 1.7;
    const nx = -Math.sin(na) * NEEDLE_W, ny = -Math.cos(na) * NEEDLE_W;

    function inFlame(x, y, scale) {
        const h = FL_H * scale;
        const u = (x - FL_X) / h, v = (FL_Y - y) / h;
        if (v < 0 || v > 1) return false;
        if (inTriangle(u, v, 0.12 * v * v, 1.0, -0.30, 0.40, 0.30, 0.40)) return true;
        const dy = v - 0.30;
        return u * u + dy * dy <= 0.34 * 0.34;
    }

    return function (x, y) {
        const dx = x - CX, dy = CY - y;
        const r = Math.sqrt(dx * dx + dy * dy);
        let ang = Math.atan2(dy, dx) * 180 / Math.PI;
        if (ang < A1 - 30) ang += 360;
        const t = (A0 - ang) / (A0 - A1);
        const inSweep = t >= 0 && t <= 1;

        if (r <= BEZ_IN) {
            if (inFlame(x, y, 1)) return inFlame(x, y, 0.5) ? FLAME_I : FLAME_O;
            if (inTriangle(x, y, tipx, tipy, CX + nx, CY + ny, CX - nx, CY - ny) ||
                (detail > 0 && inTriangle(x, y, tailx, taily, CX + nx, CY + ny, CX - nx, CY - ny)))
                return NEEDLE;
            if (r <= HUB_R) return HUB;
            if (r >= B_IN && r <= B_OUT && inSweep) return bandColour(t);
            return mix(FACE_T, FACE_B, clamp01((y - 0.12) / 0.76));
        }

        if (r <= BEZ_OUT) {
            if (detail === 2 && inSweep) {
                const minor = Math.abs(ang / 9 - Math.round(ang / 9)) * 9;
                const major = Math.abs(ang / 45 - Math.round(ang / 45)) * 45 < 2.6;
                const lim = major ? BEZ_OUT : BEZ_IN + (BEZ_OUT - BEZ_IN) * 0.62;
                if (minor < (major ? 2.4 : 1.3) && r <= lim) return TICK;
            }
            return mix(BEZ_T, BEZ_B, clamp01((y - 0.05) / 0.9));
        }

        if (inRoundedRect(x, y, 0.008, 0.008, 0.992, 0.992, 0.215))
            return mix(PLATE_T, PLATE_B, clamp01((y - 0.02) / 0.96));
        return null;
    };
}

// Renders one size with 4x4 supersampling. The mask keeps the real coverage:
// mifconv takes an EGray256 soft mask, so the rounded plate edge stays smooth
// instead of turning into a staircase.
function render(size, detail) {
    const fn = makeIcon(detail);
    const SS = 4;
    const rgb = Buffer.alloc(size * size * 3);
    const mask = Buffer.alloc(size * size);
    for (let py = 0; py < size; ++py)
        for (let px = 0; px < size; ++px) {
            let r = 0, g = 0, b = 0, hits = 0;
            for (let sy = 0; sy < SS; ++sy)
                for (let sx = 0; sx < SS; ++sx) {
                    const c = fn((px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size);
                    if (c) { r += c[0]; g += c[1]; b += c[2]; ++hits; }
                }
            const o = (py * size + px) * 3;
            if (hits) {
                rgb[o] = Math.round(r / hits);
                rgb[o + 1] = Math.round(g / hits);
                rgb[o + 2] = Math.round(b / hits);
            }
            mask[py * size + px] = Math.round(255 * hits / (SS * SS));
        }
    return { rgb, mask };
}

// 8-bit greyscale BMP with an identity palette. This is the EGray256 soft mask
// mifconv looks for beside the colour bitmap, under the name <file>_mask_soft.bmp.
function writeGrayBmp(file, size, gray) {
    const rowBytes = size, pad = (4 - (rowBytes % 4)) % 4;
    const pixels = Buffer.alloc((rowBytes + pad) * size);
    for (let y = 0; y < size; ++y) {
        const src = (size - 1 - y) * size;
        gray.copy(pixels, y * (rowBytes + pad), src, src + size);
    }
    const palette = Buffer.alloc(256 * 4);
    for (let i = 0; i < 256; ++i)
        palette[i * 4] = palette[i * 4 + 1] = palette[i * 4 + 2] = i;
    const head = Buffer.alloc(54);
    head.write('BM', 0, 'latin1');
    head.writeUInt32LE(54 + palette.length + pixels.length, 2);
    head.writeUInt32LE(54 + palette.length, 10);
    head.writeUInt32LE(40, 14);
    head.writeInt32LE(size, 18);
    head.writeInt32LE(size, 22);
    head.writeUInt16LE(1, 26);
    head.writeUInt16LE(8, 28);
    head.writeUInt32LE(pixels.length, 34);
    head.writeInt32LE(2835, 38);
    head.writeInt32LE(2835, 42);
    head.writeUInt32LE(256, 46);
    head.writeUInt32LE(256, 50);
    fs.writeFileSync(file, Buffer.concat([head, palette, pixels]));
}

// 24-bit BMP, bottom-up, rows padded to four bytes.
function writeBmp(file, size, rgb) {
    const rowBytes = size * 3, pad = (4 - (rowBytes % 4)) % 4;
    const pixels = Buffer.alloc((rowBytes + pad) * size);
    for (let y = 0; y < size; ++y) {
        const src = (size - 1 - y) * rowBytes;
        const dst = y * (rowBytes + pad);
        for (let x = 0; x < size; ++x) {
            pixels[dst + x * 3]     = rgb[src + x * 3 + 2];
            pixels[dst + x * 3 + 1] = rgb[src + x * 3 + 1];
            pixels[dst + x * 3 + 2] = rgb[src + x * 3];
        }
    }
    const head = Buffer.alloc(54);
    head.write('BM', 0, 'latin1');
    head.writeUInt32LE(54 + pixels.length, 2);
    head.writeUInt32LE(54, 10);
    head.writeUInt32LE(40, 14);
    head.writeInt32LE(size, 18);
    head.writeInt32LE(size, 22);
    head.writeUInt16LE(1, 26);
    head.writeUInt16LE(24, 28);
    head.writeUInt32LE(pixels.length, 34);
    head.writeInt32LE(2835, 38);
    head.writeInt32LE(2835, 42);
    fs.writeFileSync(file, Buffer.concat([head, pixels]));
}

const SIZES = [[88, 2], [44, 1], [22, 0]];
const dir = path.join(__dirname, '..', 'src', 'gfx');
fs.mkdirSync(dir, { recursive: true });
for (const [size, detail] of SIZES) {
    const { rgb, mask } = render(size, detail);
    writeBmp(path.join(dir, 'ocgui_' + size + '.bmp'), size, rgb);
    writeGrayBmp(path.join(dir, 'ocgui_' + size + '_mask_soft.bmp'), size, mask);
    console.log('ocgui_' + size + '.bmp + soft mask');
}
