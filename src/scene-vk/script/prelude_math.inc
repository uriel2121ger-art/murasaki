R"JS(
'use strict';
// The vector and matrix classes of the SceneScript reference (docs.wallpaperengine.io, scenescript/reference/class/Vec2..Mat4)
// and the WE modules' math. Angles are degrees everywhere a script sees them. Matrices are column-major (m[col * n + row]).
(function (global) {
  const EPS = 1e-6;
  const D2R = Math.PI / 180, R2D = 180 / Math.PI;
  function num(v, d) { return typeof v === 'number' && isFinite(v) ? v : d; }
  function fmt(n) { return String(+n.toFixed(6)); }
  function nums(s) { return String(s).trim().split(/\s+/).map(Number); }
  const isNum = (v) => typeof v === 'number';

  // components of a Number or a vector as an array of n numbers (a missing component is 0, a Number fills them all)
  function comps(v, n) {
    if (isNum(v)) return new Array(n).fill(v);
    const a = [num(v.x, 0), num(v.y, 0), num(v.z, 0), num(v.w, 0)];
    return a.slice(0, n);
  }
  const smooth = (lo, hi, x) => { const t = Math.min(1, Math.max(0, (x - lo) / (hi - lo))); return t * t * (3 - 2 * t); };

  // One generator for the componentwise parts of Vec2, Vec3 and Vec4.
  function defineVector(cls, keys) {
    const n = keys.length;
    const make = (a) => new cls(...a);
    const arr = (o) => keys.map((k) => o[k]);
    const P = cls.prototype;
    const map1 = (f) => function () { return make(arr(this).map(f)); };
    const map2 = (f, nm) => function (o) { const b = comps(o, n); return make(arr(this).map((x, i) => f(x, b[i]))); };
    P.copy = function () { return make(arr(this)); };
    P.equals = function (o) { return o != null && arr(this).every((x, i) => Math.abs(x - num(o[keys[i]], NaN)) <= EPS); };
    P.isFinite = function () { return arr(this).every(Number.isFinite); };
    P.lengthSqr = function () { return arr(this).reduce((s, x) => s + x * x, 0); };
    P.length = function () { return Math.sqrt(this.lengthSqr()); };
    P.distanceSqr = function (o) { return this.subtract(o).lengthSqr(); };
    P.distance = function (o) { return Math.sqrt(this.distanceSqr(o)); };
    P.normalize = function () { const l = this.length(); return l > 0 ? make(arr(this).map((x) => x / l)) : make(new Array(n).fill(0)); };
    P.negate = map1((x) => -x);
    P.add = map2((x, y) => x + y);
    P.subtract = map2((x, y) => x - y);
    P.multiply = map2((x, y) => x * y);
    P.divide = map2((x, y) => x / y);
    P.dot = function (o) { const b = comps(o, n); return arr(this).reduce((s, x, i) => s + x * b[i], 0); };
    P.reflect = function (nrm) { const d = 2 * this.dot(nrm); const b = comps(nrm, n); return make(arr(this).map((x, i) => x - d * b[i])); };
    P.project = function (o) { const d = o.dot(o); const s = d > 0 ? this.dot(o) / d : 0; const b = comps(o, n); return make(b.map((x) => x * s)); };
    P.mix = function (o, t) { const b = comps(o, n), tt = comps(t, n); return make(arr(this).map((x, i) => x + (b[i] - x) * tt[i])); };
    P.min = map2(Math.min);
    P.max = map2(Math.max);
    P.clamp = function (lo, hi) { const l = comps(lo, n), h = comps(hi, n); return make(arr(this).map((x, i) => Math.min(h[i], Math.max(l[i], x)))); };
    P.abs = map1(Math.abs);
    P.sign = map1(Math.sign);
    P.round = map1(Math.round);
    P.floor = map1(Math.floor);
    P.ceil = map1(Math.ceil);
    P.fract = map1((x) => x - Math.floor(x));
    P.mod = map2((x, y) => x - y * Math.floor(x / y));
    P.step = function (edge) { const e = comps(edge, n); return make(arr(this).map((x, i) => (x < e[i] ? 0 : 1))); };
    P.smoothStep = function (lo, hi) { const l = comps(lo, n), h = comps(hi, n); return make(arr(this).map((x, i) => smooth(l[i], h[i], x))); };
    P.toString = function () { return arr(this).join(' '); };
    P.toJSON = function () { return arr(this); };
    for (let i = 0; i < n; ++i) {
      const k = keys[i];
      const alias = ['r', 'g', 'b', 'a'][i];
      Object.defineProperty(P, alias, { get() { return this[k]; }, set(v) { this[k] = v; }, enumerable: false });
    }
  }

  class Vec2 {
    constructor(x, y) {
      if (x !== null && typeof x === 'object') { this.x = num(x.x, 0); this.y = num(x.y, 0); }
      else if (typeof x === 'string') { const p = nums(x); this.x = num(p[0], 0); this.y = num(p[1], 0); }
      else if (x === undefined) { this.x = 0; this.y = 0; }
      else { this.x = num(x, 0); this.y = y === undefined ? num(x, 0) : num(y, 0); }
    }
    perpendicular() { return new Vec2(-this.y, this.x); }
    // the direction of the vector in degrees, counter-clockwise from +x
    angle() { return Math.atan2(this.y, this.x) * R2D; }
    // the signed angle from this vector to the other, in degrees (positive counter-clockwise); 0 when either has no length
    angleBetween(o) {
      return this.length() * o.length() > 0 ? Math.atan2(this.x * o.y - this.y * o.x, this.x * o.x + this.y * o.y) * R2D : 0;
    }
    rotate(angle) {
      const c = Math.cos(num(angle, 0) * D2R), s = Math.sin(num(angle, 0) * D2R);
      return new Vec2(this.x * c - this.y * s, this.x * s + this.y * c);
    }
  }
  class Vec3 {
    constructor(x, y, z) {
      if (x !== null && typeof x === 'object') { this.x = num(x.x, 0); this.y = num(x.y, 0); this.z = num(x.z, x instanceof Vec2 ? num(y, 0) : 0); }
      else if (typeof x === 'string') { const p = nums(x); this.x = num(p[0], 0); this.y = num(p[1], 0); this.z = num(p[2], 0); }
      else if (x === undefined) { this.x = this.y = this.z = 0; }
      else if (y === undefined && z === undefined) { this.x = this.y = this.z = num(x, 0); }
      else { this.x = num(x, 0); this.y = num(y, 0); this.z = num(z, 0); }
    }
    get xy() { return new Vec2(this.x, this.y); } set xy(v) { this.x = v.x; this.y = v.y; }
    static fromSpherical(r, theta, phi) {
      const t = theta * D2R, p = phi * D2R;
      return new Vec3(r * Math.sin(t) * Math.cos(p), r * Math.cos(t), r * Math.sin(t) * Math.sin(p));
    }
    toSpherical() {
      const r = this.length();
      return new Vec3(r, r > 0 ? Math.acos(Math.min(1, Math.max(-1, this.y / r))) * R2D : 0, Math.atan2(this.z, this.x) * R2D);
    }
    cross(o) { return new Vec3(this.y * o.z - this.z * o.y, this.z * o.x - this.x * o.z, this.x * o.y - this.y * o.x); }
    refract(nrm, eta) {
      const d = nrm.dot(this), k = 1 - eta * eta * (1 - d * d);
      return k < 0 ? new Vec3(0, 0, 0) : this.multiply(eta).subtract(nrm.multiply(eta * d + Math.sqrt(k)));
    }
    angleBetween(o) {
      const l = this.length() * o.length();
      return l > 0 ? Math.acos(Math.min(1, Math.max(-1, this.dot(o) / l))) * R2D : 0;
    }
  }
  class Vec4 {
    constructor(x, y, z, w) {
      if (x !== null && typeof x === 'object') {
        if (x instanceof Vec3) { this.x = x.x; this.y = x.y; this.z = x.z; this.w = num(y, 0); }
        else if (x instanceof Vec2) { this.x = x.x; this.y = x.y; this.z = num(y, 0); this.w = num(z, 0); }
        else { this.x = num(x.x, 0); this.y = num(x.y, 0); this.z = num(x.z, 0); this.w = num(x.w, 0); }
      } else if (typeof x === 'string') { const p = nums(x); this.x = num(p[0], 0); this.y = num(p[1], 0); this.z = num(p[2], 0); this.w = num(p[3], 0); }
      else if (x === undefined) { this.x = this.y = this.z = this.w = 0; }
      else if (y === undefined) { this.x = this.y = this.z = this.w = num(x, 0); }
      else { this.x = num(x, 0); this.y = num(y, 0); this.z = num(z, 0); this.w = num(w, 0); }
    }
  }
  defineVector(Vec2, ['x', 'y']);
  defineVector(Vec3, ['x', 'y', 'z']);
  defineVector(Vec4, ['x', 'y', 'z', 'w']);

  // ---- matrices (column-major) ----
  function matmul(a, b, n) {
    const r = new Array(n * n).fill(0);
    for (let c = 0; c < n; ++c) for (let k = 0; k < n; ++k) { const bk = b[c * n + k]; for (let rr = 0; rr < n; ++rr) r[c * n + rr] += a[k * n + rr] * bk; }
    return r;
  }
  function transposeOf(a, n) { const r = new Array(n * n); for (let c = 0; c < n; ++c) for (let rr = 0; rr < n; ++rr) r[rr * n + c] = a[c * n + rr]; return r; }
  function det3(a) {
    return a[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (a[1] * a[8] - a[7] * a[2]) + a[6] * (a[1] * a[5] - a[4] * a[2]);
  }
  function inv3(a) {
    const d = det3(a);
    if (d === 0) return a.slice();
    const i = 1 / d;
    return [
      (a[4] * a[8] - a[7] * a[5]) * i, (a[7] * a[2] - a[1] * a[8]) * i, (a[1] * a[5] - a[4] * a[2]) * i,
      (a[6] * a[5] - a[3] * a[8]) * i, (a[0] * a[8] - a[6] * a[2]) * i, (a[3] * a[2] - a[0] * a[5]) * i,
      (a[3] * a[7] - a[6] * a[4]) * i, (a[6] * a[1] - a[0] * a[7]) * i, (a[0] * a[4] - a[3] * a[1]) * i,
    ];
  }
  function det4(m) {
    const [a00, a01, a02, a03, a10, a11, a12, a13, a20, a21, a22, a23, a30, a31, a32, a33] = m;
    const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11,
      b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12, b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30,
      b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    return b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  }
  function inv4(m) {
    const [a00, a01, a02, a03, a10, a11, a12, a13, a20, a21, a22, a23, a30, a31, a32, a33] = m;
    const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11,
      b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12, b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30,
      b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    const d = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (d === 0) return m.slice();
    const i = 1 / d;
    return [
      (a11 * b11 - a12 * b10 + a13 * b09) * i, (a02 * b10 - a01 * b11 - a03 * b09) * i, (a31 * b05 - a32 * b04 + a33 * b03) * i, (a22 * b04 - a21 * b05 - a23 * b03) * i,
      (a12 * b08 - a10 * b11 - a13 * b07) * i, (a00 * b11 - a02 * b08 + a03 * b07) * i, (a32 * b02 - a30 * b05 - a33 * b01) * i, (a20 * b05 - a22 * b02 + a23 * b01) * i,
      (a10 * b10 - a11 * b08 + a13 * b06) * i, (a01 * b08 - a00 * b10 - a03 * b06) * i, (a30 * b04 - a31 * b02 + a33 * b00) * i, (a21 * b02 - a20 * b04 - a23 * b00) * i,
      (a11 * b07 - a10 * b09 - a12 * b06) * i, (a00 * b09 - a01 * b07 + a02 * b06) * i, (a31 * b01 - a30 * b03 - a32 * b00) * i, (a20 * b03 - a21 * b01 + a22 * b00) * i,
    ];
  }

  class Mat3 {
    constructor(m) { this.m = m && m.length === 9 ? m.slice() : [1, 0, 0, 0, 1, 0, 0, 0, 1]; }
    static identity() { return new Mat3(); }
    static fromTranslation(v) { return new Mat3([1, 0, 0, 0, 1, 0, v.x, v.y, 1]); }
    static fromScale(v) { const s = comps(v, 2); return new Mat3([s[0], 0, 0, 0, s[1], 0, 0, 0, 1]); }
    static fromRotation(a) { const c = Math.cos(a * D2R), s = Math.sin(a * D2R); return new Mat3([c, s, 0, -s, c, 0, 0, 0, 1]); }
    static fromBasis(right, up) { return new Mat3([right.x, right.y, 0, up.x, up.y, 0, 0, 0, 1]); }
    static fromMat4(m4) { const a = m4.m; return new Mat3([a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10]]); }
    static compose(t, r, s) { return Mat3.fromTranslation(t).multiply(Mat3.fromRotation(r)).multiply(Mat3.fromScale(s)); }
    translation(p) { if (p === undefined) return new Vec2(this.m[6], this.m[7]); this.m[6] = p.x; this.m[7] = p.y; }
    angle() { return Math.atan2(this.m[1], this.m[0]) * R2D; }
    add(o) { return new Mat3(this.m.map((x, i) => x + o.m[i])); }
    subtract(o) { return new Mat3(this.m.map((x, i) => x - o.m[i])); }
    multiply(v) {
      if (isNum(v)) return new Mat3(this.m.map((x) => x * v));
      if (v instanceof Mat3) return new Mat3(matmul(this.m, v.m, 3));
      const a = this.m;
      return new Vec3(a[0] * v.x + a[3] * v.y + a[6] * v.z, a[1] * v.x + a[4] * v.y + a[7] * v.z, a[2] * v.x + a[5] * v.y + a[8] * v.z);
    }
    translate(v) { return this.multiply(Mat3.fromTranslation(v)); }
    rotate(a) { return this.multiply(Mat3.fromRotation(a)); }
    scale(v) { return this.multiply(Mat3.fromScale(v)); }
    transformPoint(v) { const a = this.m; return new Vec2(a[0] * v.x + a[3] * v.y + a[6], a[1] * v.x + a[4] * v.y + a[7]); }
    transformDirection(v) { const a = this.m; return new Vec2(a[0] * v.x + a[3] * v.y, a[1] * v.x + a[4] * v.y); }
    transpose() { return new Mat3(transposeOf(this.m, 3)); }
    determinant() { return det3(this.m); }
    inverse() { return new Mat3(inv3(this.m)); }
    decompose() {
      const a = this.m;
      const sx = Math.hypot(a[0], a[1]), sy = Math.hypot(a[3], a[4]) * (this.determinant() < 0 ? -1 : 1);
      return { translation: new Vec2(a[6], a[7]), rotation: Math.atan2(a[1], a[0]) * R2D, scale: new Vec2(sx, sy) };
    }
    copy() { return new Mat3(this.m); }
    equals(o) { return o instanceof Mat3 && this.m.every((x, i) => Math.abs(x - o.m[i]) <= EPS); }
    toString() { return this.m.map(fmt).join(' '); }
  }

  class Mat4 {
    constructor(m) { this.m = m && m.length === 16 ? m.slice() : [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }
    static identity() { return new Mat4(); }
    static fromTranslation(v) { return new Mat4([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, v.x, v.y, num(v.z, 0), 1]); }
    static fromScale(v) { const s = comps(v, 3); return new Mat4([s[0], 0, 0, 0, 0, s[1], 0, 0, 0, 0, s[2], 0, 0, 0, 0, 1]); }
    static fromRotation(angle, axis) {
      const a = axis.normalize(), c = Math.cos(angle * D2R), s = Math.sin(angle * D2R), t = 1 - c, x = a.x, y = a.y, z = a.z;
      return new Mat4([t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0, t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
                       t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0, 0, 0, 0, 1]);
    }
    static fromEuler(x, y, z) {
      const e = isNum(x) ? [x, y, z] : [x.x, x.y, x.z];
      const rx = Mat4.fromRotation(e[0], new Vec3(1, 0, 0)), ry = Mat4.fromRotation(e[1], new Vec3(0, 1, 0)), rz = Mat4.fromRotation(e[2], new Vec3(0, 0, 1));
      return rz.multiply(ry).multiply(rx); // the order the scene's own angles use: x, then y, then z
    }
    static fromBasis(r, u, f) { return new Mat4([r.x, r.y, r.z, 0, u.x, u.y, u.z, 0, f.x, f.y, f.z, 0, 0, 0, 0, 1]); }
    static lookAt(eye, center, up) {
      const f = center.subtract(eye).normalize(), s = f.cross(up).normalize(), u = s.cross(f);
      return new Mat4([s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -s.dot(eye), -u.dot(eye), f.dot(eye), 1]);
    }
    static compose(t, r, s) { return Mat4.fromTranslation(t).multiply(Mat4.fromEuler(r)).multiply(Mat4.fromScale(s)); }
    translation(p) { if (p === undefined) return new Vec3(this.m[12], this.m[13], this.m[14]); this.m[12] = p.x; this.m[13] = p.y; this.m[14] = num(p.z, this.m[14]); }
    right() { return new Vec3(this.m[0], this.m[1], this.m[2]); }
    up() { return new Vec3(this.m[4], this.m[5], this.m[6]); }
    forward() { return new Vec3(this.m[8], this.m[9], this.m[10]); }
    add(o) { return new Mat4(this.m.map((x, i) => x + o.m[i])); }
    subtract(o) { return new Mat4(this.m.map((x, i) => x - o.m[i])); }
    multiply(v) {
      if (isNum(v)) return new Mat4(this.m.map((x) => x * v));
      if (v instanceof Mat4) return new Mat4(matmul(this.m, v.m, 4));
      const a = this.m;
      return new Vec4(a[0] * v.x + a[4] * v.y + a[8] * v.z + a[12] * v.w, a[1] * v.x + a[5] * v.y + a[9] * v.z + a[13] * v.w,
                      a[2] * v.x + a[6] * v.y + a[10] * v.z + a[14] * v.w, a[3] * v.x + a[7] * v.y + a[11] * v.z + a[15] * v.w);
    }
    translate(v) { return this.multiply(Mat4.fromTranslation(v)); }
    rotate(angle, axis) { return this.multiply(Mat4.fromRotation(angle, axis)); }
    scale(v) { return this.multiply(Mat4.fromScale(v)); }
    transformPoint(v) { const r = this.multiply(new Vec4(v.x, v.y, v.z, 1)); return new Vec3(r.x, r.y, r.z); }
    transformDirection(v) { const r = this.multiply(new Vec4(v.x, v.y, v.z, 0)); return new Vec3(r.x, r.y, r.z); }
    transpose() { return new Mat4(transposeOf(this.m, 4)); }
    inverse() { return new Mat4(inv4(this.m)); }
    determinant() { return det4(this.m); }
    extractEuler() {
      // the inverse of fromEuler (R = Rz * Ry * Rx), on the matrix with its scale taken out
      const a = this.m;
      const sx = Math.hypot(a[0], a[1], a[2]) || 1, sy = Math.hypot(a[4], a[5], a[6]) || 1, sz = Math.hypot(a[8], a[9], a[10]) || 1;
      const r00 = a[0] / sx, r10 = a[1] / sx, r20 = a[2] / sx, r21 = a[6] / sy, r22 = a[10] / sz, r11 = a[5] / sy, r12 = a[9] / sz;
      const y = Math.asin(Math.max(-1, Math.min(1, -r20)));
      if (Math.abs(r20) < 0.999999) return new Vec3(Math.atan2(r21, r22) * R2D, y * R2D, Math.atan2(r10, r00) * R2D);
      return new Vec3(Math.atan2(-r12, r11) * R2D, y * R2D, 0);
    }
    normalMatrix() { return Mat3.fromMat4(this).inverse().transpose(); }
    decompose() {
      const a = this.m;
      const sx = Math.hypot(a[0], a[1], a[2]), sy = Math.hypot(a[4], a[5], a[6]), sz = Math.hypot(a[8], a[9], a[10]) * (this.determinant() < 0 ? -1 : 1);
      const rot = new Mat4([a[0] / sx, a[1] / sx, a[2] / sx, 0, a[4] / sy, a[5] / sy, a[6] / sy, 0, a[8] / sz, a[9] / sz, a[10] / sz, 0, 0, 0, 0, 1]);
      return { translation: new Vec3(a[12], a[13], a[14]), rotation: rot.extractEuler(), scale: new Vec3(sx, sy, sz) };
    }
    copy() { return new Mat4(this.m); }
    equals(o) { return o instanceof Mat4 && this.m.every((x, i) => Math.abs(x - o.m[i]) <= EPS); }
    toString() { return this.m.map(fmt).join(' '); }
  }

  // ---- the modules ----
  // deg2rad and rad2deg are numbers in the reference; they stay callable too (older scripts of this engine called them)
  const factor = (k) => { const f = (d) => d * k; f.valueOf = () => k; f.toString = () => String(k); return f; };
  const WEMath = {
    PI: Math.PI,
    deg2rad: factor(D2R),
    rad2deg: factor(R2D),
    mix(a, b, t) { return a + (b - a) * t; },
    clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); },
    smoothStep(lo, hi, v) { return smooth(lo, hi, v); },
  };
  const WEColor = {
    rgb2hsv(c) {
      const r = c.x, g = c.y, b = c.z, mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
      let h = 0;
      if (d > 0) { h = mx === r ? ((g - b) / d) % 6 : mx === g ? (b - r) / d + 2 : (r - g) / d + 4; h /= 6; if (h < 0) h += 1; }
      return new Vec3(h, mx === 0 ? 0 : d / mx, mx);
    },
    hsv2rgb(c) {
      const h = c.x * 6, s = c.y, v = c.z, i = Math.floor(h), f = h - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
      switch (((i % 6) + 6) % 6) {
        case 0: return new Vec3(v, t, p); case 1: return new Vec3(q, v, p); case 2: return new Vec3(p, v, t);
        case 3: return new Vec3(p, q, v); case 4: return new Vec3(t, p, v); default: return new Vec3(v, p, q);
      }
    },
    normalizeColor(c) { return new Vec3(c.x / 255, c.y / 255, c.z / 255); },
    expandColor(c) { return new Vec3(c.x * 255, c.y * 255, c.z * 255); },
  };
  const WEVector = {
    angleVector2(deg) { const r = deg * D2R; return new Vec2(Math.cos(r), Math.sin(r)); },
    vectorAngle2(v) { return Math.atan2(v.y, v.x) * R2D; },
  };
  global.Vec2 = Vec2; global.Vec3 = Vec3; global.Vec4 = Vec4; global.Mat3 = Mat3; global.Mat4 = Mat4;
  global.WEMath = WEMath; global.WEColor = WEColor; global.WEVector = WEVector;
})(globalThis);
)JS"
