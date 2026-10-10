// Host tests for the pure parts of the PSP renderer: matrix helpers and vertex packing.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "psp/render/PspRenderMath.h"
#include "psp/render/PspVertexPack.h"

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

struct V { float x, y, z, u, v; std::uint32_t color; std::int32_t normal; std::int32_t brightness; };

static RenderInterleavedMesh meshOf(const std::vector<V> &v, RenderPrimitive prim)
{
    RenderInterleavedMesh m;
    m.data = v.data();
    m.stride = 32;
    m.count = static_cast<int>(v.size());
    m.primitive = prim;
    m.hasTexture = true; m.texCoordOffset = 12;
    m.hasColor = true; m.colorOffset = 20;
    m.hasBrightness = true; m.brightnessOffset = 28;
    return m;
}

int main()
{
    using namespace PspMath;
    // translate then scale: point (1,1,1) -> scale first, then translate
    Mat4 m = multiply(translation(10, 0, 0), scaling(2, 2, 2));
    assert(near(m.m[0], 2) && near(m.m[12], 10));
    // rotate 90deg about Y maps x axis to -z (right-handed GL)
    Mat4 r = rotation(90, 0, 1, 0);
    assert(near(r.m[0], 0) && near(r.m[2], -1));
    // ortho(0,480,272,0,-1,1): x=480 -> ndc 1, y=0 -> ndc 1
    Mat4 o = ortho(0, 480, 272, 0, -1, 1);
    assert(near(o.m[0] * 480 + o.m[12], 1.0f) && near(o.m[5] * 0 + o.m[13], 1.0f));
    assert(isIdentity(identity()) && !isIdentity(m));

    // Quad fold: two quads as (a,b,c,a,c,d)
    std::vector<V> t;
    for (int q = 0; q < 2; ++q)
    {
        V a{0.f + q, 0, 0, 0, 0, 0xFF0000FFu, 0, 0x00F000F0}, b{1.f + q, 0, 0, 1, 0, 0xFF0000FFu, 0, 0},
          c{1.f + q, 1, 0, 1, 1, 0xFF0000FFu, 0, 0}, d{0.f + q, 1, 0, 0, 1, 0xFF0000FFu, 0, 0};
        t.push_back(a); t.push_back(b); t.push_back(c); t.push_back(a); t.push_back(c); t.push_back(d);
    }
    RenderInterleavedMesh mesh = meshOf(t, RenderPrimitive::Triangles);
    std::vector<unsigned char> out(PspPack::maxVertices(mesh) * sizeof(PspPack::VertexLit));
    PspPack::Options opt;
    PspPack::Result res;
    assert(PspPack::pack(mesh, opt, out.data(), res));
    assert(res.prim == PspPack::Prim::QuadsIndexed && res.vertexCount == 8 && !res.lit);
    const PspPack::VertexPlain *vp = reinterpret_cast<const PspPack::VertexPlain *>(out.data());
    assert(near(vp[3].x, 0) && near(vp[3].y, 1));   // 4th vertex of quad 0 is "d"
    assert(near(vp[4].x, 1));                        // quad 1 starts at x=1

    // Not foldable -> plain triangles
    std::vector<V> t2 = t; t2[3].x = 5.0f;
    mesh = meshOf(t2, RenderPrimitive::Triangles);
    assert(PspPack::pack(mesh, opt, out.data(), res));
    assert(res.prim == PspPack::Prim::Triangles && res.vertexCount == 12);

    // Lightmap: sky 15, block 0 entry is half bright red-only
    std::uint32_t lm[256];
    for (int i = 0; i < 256; ++i) lm[i] = 0xFF000000u | (0x80u << 16) | (0x40u << 8) | 0x20u;
    opt.lightmap = lm;
    mesh = meshOf(t, RenderPrimitive::Triangles);
    assert(PspPack::pack(mesh, opt, out.data(), res));
    // color word is ABGR: R lowest byte. 0xFF0000FF -> R=255,G=0,B=0 -> R*0x80/255=0x80
    assert((vp[0].color & 0xFF) == 0x80 && ((vp[0].color >> 24) == 0xFF));

    // Line loop closes
    std::vector<V> l = {V{0,0,0,0,0,0xFFFFFFFFu,0,0}, V{1,0,0,0,0,0xFFFFFFFFu,0,0}, V{1,1,0,0,0,0xFFFFFFFFu,0,0}};
    mesh = meshOf(l, RenderPrimitive::LineLoop);
    opt.lightmap = nullptr;
    assert(PspPack::pack(mesh, opt, out.data(), res) && res.prim == PspPack::Prim::LineStrip && res.vertexCount == 4);

    // Quad indices
    std::uint16_t idx[12];
    PspPack::fillQuadIndices(idx, 2);
    assert(idx[0] == 0 && idx[2] == 2 && idx[5] == 3 && idx[6] == 4 && idx[11] == 7);

    // Short positions are passed through unscaled
    struct SV { std::int16_t x, y, z, pad; float u, v; std::int32_t color; std::int32_t br; };
    std::vector<SV> sv = {{3, 4, 5, 0, 0.5f, 0.25f, (std::int32_t)0xFFFFFFFFu, 0}};
    RenderInterleavedMesh sm;
    sm.data = sv.data(); sm.stride = 24; sm.count = 1; sm.primitive = RenderPrimitive::Points;
    sm.positionShort = true; sm.hasTexture = true; sm.texCoordOffset = 8; sm.hasColor = true; sm.colorOffset = 16;
    assert(PspPack::pack(sm, opt, out.data(), res));
    assert(near(vp[0].x, 3) && near(vp[0].z, 5) && near(vp[0].v, 0.25f));

    std::puts("PspRenderTests: all passed");
    return 0;
}
