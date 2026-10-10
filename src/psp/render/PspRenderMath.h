#pragma once
// Small column-major 4x4 matrix helpers with OpenGL semantics (m[col*4 + row]).
// Plain C++ so it can be unit-tested on the host; the PSP renderer feeds the result to the GE.
#include <cmath>
#include <cstring>

namespace PspMath
{
struct Mat4
{
    float m[16];
};

inline Mat4 identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

// r = a * b (apply b first, then a), same as OpenGL's glMultMatrix(b) on current matrix a.
inline Mat4 multiply(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Mat4 translation(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline Mat4 scaling(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
}

// glRotatef(angleDegrees, x, y, z)
inline Mat4 rotation(float angleDegrees, float x, float y, float z)
{
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-8f)
        return identity();
    x /= len;
    y /= len;
    z /= len;
    const float rad = angleDegrees * 0.017453292519943295f;
    const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    Mat4 r = identity();
    r.m[0] = t * x * x + c;
    r.m[1] = t * x * y + s * z;
    r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z;
    r.m[5] = t * y * y + c;
    r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y;
    r.m[9] = t * y * z - s * x;
    r.m[10] = t * z * z + c;
    return r;
}

// glFrustum
inline Mat4 frustum(double l, double r, double b, double t, double n, double f)
{
    Mat4 m{};
    m.m[0] = static_cast<float>(2.0 * n / (r - l));
    m.m[5] = static_cast<float>(2.0 * n / (t - b));
    m.m[8] = static_cast<float>((r + l) / (r - l));
    m.m[9] = static_cast<float>((t + b) / (t - b));
    m.m[10] = static_cast<float>(-(f + n) / (f - n));
    m.m[11] = -1.0f;
    m.m[14] = static_cast<float>(-2.0 * f * n / (f - n));
    return m;
}

// glOrtho
inline Mat4 ortho(double l, double r, double b, double t, double n, double f)
{
    Mat4 m = identity();
    m.m[0] = static_cast<float>(2.0 / (r - l));
    m.m[5] = static_cast<float>(2.0 / (t - b));
    m.m[10] = static_cast<float>(-2.0 / (f - n));
    m.m[12] = static_cast<float>(-(r + l) / (r - l));
    m.m[13] = static_cast<float>(-(t + b) / (t - b));
    m.m[14] = static_cast<float>(-(f + n) / (f - n));
    return m;
}

inline bool isIdentity(const Mat4 &a)
{
    static const Mat4 id = identity();
    return std::memcmp(a.m, id.m, sizeof(a.m)) == 0;
}

// Transforms a direction (w = 0) by the upper-left 3x3 of a and normalises it.
inline void transformDirection(const Mat4 &a, const float in[3], float out[3])
{
    out[0] = a.m[0] * in[0] + a.m[4] * in[1] + a.m[8] * in[2];
    out[1] = a.m[1] * in[0] + a.m[5] * in[1] + a.m[9] * in[2];
    out[2] = a.m[2] * in[0] + a.m[6] * in[1] + a.m[10] * in[2];
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-8f)
    {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}
}
