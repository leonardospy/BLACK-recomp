#pragma once
// Small vector math for the VR models: column vectors, matrices by rows (m[row][column]), quaternions x y z w.
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vr::math {

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normal(V3 a) {
    float l = Length(a);
    return l > 1e-8f ? a * (1 / l) : V3{0, 0, 0};
}

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Q operator*(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q Conj(Q a) { return {-a.x, -a.y, -a.z, a.w}; }
inline Q AxisAngle(V3 axis, float a) {
    V3 n = Normal(axis);
    float s = std::sin(a / 2);
    return {n.x * s, n.y * s, n.z * s, std::cos(a / 2)};
}
inline V3 Rotate(Q q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = Cross(u, v) * 2;
    return v + t * q.w + Cross(u, t);
}
// the shortest turn taking direction a to direction b
inline Q Between(V3 a, V3 b) {
    a = Normal(a);
    b = Normal(b);
    float d = Dot(a, b);
    if (d < -0.9999f) {
        V3 axis = Cross({1, 0, 0}, a);
        if (Length(axis) < 1e-4f) axis = Cross({0, 1, 0}, a);
        return AxisAngle(axis, 3.14159265f);
    }
    V3 c = Cross(a, b);
    Q q{c.x, c.y, c.z, 1 + d};
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

struct M4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};
inline M4 operator*(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = 0;
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    return r;
}
inline M4 FromColumnMajor(const float* f) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) r.m[row][c] = f[c * 4 + row];
    return r;
}
// rotation, then scale, then the position
inline M4 Compose(V3 p, Q q, float s = 1) {
    M4 r;
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float rot[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                       {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                       {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = rot[i][j] * s;
    r.m[0][3] = p.x;
    r.m[1][3] = p.y;
    r.m[2][3] = p.z;
    return r;
}
inline V3 Position(const M4& a) { return {a.m[0][3], a.m[1][3], a.m[2][3]}; }
inline V3 Apply(const M4& a, V3 v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3],
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3],
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3]};
}
// the rotation of a matrix without scale or shear
inline Q Rotation(const M4& a) {
    V3 c0 = Normal({a.m[0][0], a.m[1][0], a.m[2][0]});
    V3 c1 = Normal({a.m[0][1], a.m[1][1], a.m[2][1]});
    V3 c2 = Normal({a.m[0][2], a.m[1][2], a.m[2][2]});
    float t = c0.x + c1.y + c2.z;
    Q q;
    if (t > 0) {
        float s = std::sqrt(t + 1) * 2;
        q = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, s / 4};
    } else if (c0.x > c1.y && c0.x > c2.z) {
        float s = std::sqrt(1 + c0.x - c1.y - c2.z) * 2;
        q = {s / 4, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
    } else if (c1.y > c2.z) {
        float s = std::sqrt(1 + c1.y - c0.x - c2.z) * 2;
        q = {(c1.x + c0.y) / s, s / 4, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
    } else {
        float s = std::sqrt(1 + c2.z - c0.x - c1.y) * 2;
        q = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, s / 4, (c0.y - c1.x) / s};
    }
    return q;
}
inline float Scale(const M4& a) { return Length({a.m[0][0], a.m[1][0], a.m[2][0]}); }
inline M4 RigidInverse(const M4& a) {
    // rotation times uniform scale, then a position
    float s = Scale(a);
    M4 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i] / (s * s);
    V3 p = Position(a);
    for (int i = 0; i < 3; i++) r.m[i][3] = -(r.m[i][0] * p.x + r.m[i][1] * p.y + r.m[i][2] * p.z);
    return r;
}

}  // namespace vr::math
