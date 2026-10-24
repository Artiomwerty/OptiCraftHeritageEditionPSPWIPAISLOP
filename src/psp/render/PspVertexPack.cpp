#include "psp/render/PspVertexPack.h"

#include <cstring>

namespace PspPack
{
namespace
{
// round(x / 255) for x in [0, 255*255] without a divide (the Allegrex divider takes ~36 cycles and
// this runs three times per vertex for every chunk mesh and every immediate draw).
inline std::uint32_t div255(std::uint32_t x)
{
    x += 128u;
    return (x + (x >> 8)) >> 8;
}

inline std::uint32_t applyLight(std::uint32_t color, std::uint32_t light)
{
    const std::uint32_t r = (color & 0xFFu) * ((light >> 16) & 0xFFu);
    const std::uint32_t g = ((color >> 8) & 0xFFu) * ((light >> 8) & 0xFFu);
    const std::uint32_t b = ((color >> 16) & 0xFFu) * (light & 0xFFu);
    return (color & 0xFF000000u) | div255(r) | (div255(g) << 8) | (div255(b) << 16);
}

inline int lightIndexFromBrightness(std::int32_t brightness)
{
    return ((brightness >> 20) & 15) * 16 + ((brightness >> 4) & 15);
}

inline int lightIndexFromCoord(int u, int v)
{
    int bl = u >> 4, sk = v >> 4;
    bl = bl < 0 ? 0 : (bl > 15 ? 15 : bl);
    sk = sk < 0 ? 0 : (sk > 15 ? 15 : sk);
    return sk * 16 + bl;
}

struct Source
{
    const unsigned char *base;
    int stride;
    const RenderInterleavedMesh *mesh;
};

inline const unsigned char *vptr(const Source &s, int i)
{
    return s.base + static_cast<std::size_t>(i) * static_cast<std::size_t>(s.stride);
}

template <typename Vertex>
inline void write(const Source &s, const Options &opt, int i, Vertex &dst, bool withNormal)
{
    const RenderInterleavedMesh &m = *s.mesh;
    const unsigned char *p = vptr(s, i);

    if (m.positionShort)
    {
        std::int16_t sp[3];
        std::memcpy(sp, p, sizeof(sp));
        dst.x = static_cast<float>(sp[0]);
        dst.y = static_cast<float>(sp[1]);
        dst.z = static_cast<float>(sp[2]);
    }
    else
    {
        float fp[3];
        std::memcpy(fp, p, sizeof(fp));
        dst.x = fp[0];
        dst.y = fp[1];
        dst.z = fp[2];
    }

    if (m.hasTexture)
    {
        float uv[2];
        std::memcpy(uv, p + m.texCoordOffset, sizeof(uv));
        dst.u = uv[0];
        dst.v = uv[1];
    }
    else
    {
        dst.u = 0.0f;
        dst.v = 0.0f;
    }

    std::uint32_t color = opt.currentColor;
    if (m.hasColor)
        std::memcpy(&color, p + m.colorOffset, sizeof(color));

    if (opt.lightmap != nullptr)
    {
        int idx;
        if (m.hasBrightness)
        {
            std::int32_t br;
            std::memcpy(&br, p + m.brightnessOffset, sizeof(br));
            idx = lightIndexFromBrightness(br);
        }
        else
        {
            idx = lightIndexFromCoord(opt.lightmapU, opt.lightmapV);
        }
        color = applyLight(color, opt.lightmap[idx]);
    }
    dst.color = color;

    (void)withNormal;
}

inline void writeNormal(const Source &s, int i, VertexLit &dst)
{
    const RenderInterleavedMesh &m = *s.mesh;
    if (m.hasNormals)
    {
        const unsigned char *p = vptr(s, i) + m.normalOffset;
        dst.nx = static_cast<std::int8_t>(p[0]);
        dst.ny = static_cast<std::int8_t>(p[1]);
        dst.nz = static_cast<std::int8_t>(p[2]);
    }
    else
    {
        dst.nx = 0;
        dst.ny = 127;
        dst.nz = 0;
    }
    dst.pad = 0;
}

inline bool sameVertex(const Source &s, int a, int b)
{
    return std::memcmp(vptr(s, a), vptr(s, b), static_cast<std::size_t>(s.stride)) == 0;
}

// True when `count` vertices are n*6 triangles that all follow the (a,b,c,a,c,d) quad-pair pattern.
bool isFoldableQuadList(const Source &s, int count)
{
    if (count < 6 || count % 6 != 0 || s.stride < 12)
        return false;
    for (int i = 0; i < count; i += 6)
        if (!sameVertex(s, i + 3, i) || !sameVertex(s, i + 4, i + 2))
            return false;
    return true;
}

template <typename Vertex>
bool packT(const Source &s, const Options &opt, void *dstRaw, Result &out, bool lit)
{
    const RenderInterleavedMesh &m = *s.mesh;
    Vertex *dst = static_cast<Vertex *>(dstRaw);
    auto emit = [&](int srcIndex)
    {
        write<Vertex>(s, opt, srcIndex, *dst, lit);
        if constexpr (sizeof(Vertex) == sizeof(VertexLit))
            writeNormal(s, srcIndex, *reinterpret_cast<VertexLit *>(dst));
        ++dst;
    };

    switch (m.primitive)
    {
    case RenderPrimitive::Points: out.prim = Prim::Points; break;
    case RenderPrimitive::Lines: out.prim = Prim::Lines; break;
    case RenderPrimitive::LineLoop:
    case RenderPrimitive::LineStrip: out.prim = Prim::LineStrip; break;
    case RenderPrimitive::Triangles: out.prim = Prim::Triangles; break;
    case RenderPrimitive::TriangleStrip: out.prim = Prim::TriangleStrip; break;
    case RenderPrimitive::TriangleFan: out.prim = Prim::TriangleFan; break;
    case RenderPrimitive::Quads: out.prim = Prim::QuadsIndexed; break;
    default: return false;
    }

    if (out.prim == Prim::Triangles && opt.foldQuads && isFoldableQuadList(s, m.count))
    {
        out.prim = Prim::QuadsIndexed;
        for (int i = 0; i < m.count; i += 6)
        {
            emit(i + 0);
            emit(i + 1);
            emit(i + 2);
            emit(i + 5);
        }
    }
    else
    {
        for (int i = 0; i < m.count; ++i)
            emit(i);
        if (m.primitive == RenderPrimitive::LineLoop && m.count > 0)
            emit(0);
    }
    out.vertexCount = static_cast<int>(dst - static_cast<Vertex *>(dstRaw));
    return true;
}
} // namespace

bool pack(const RenderInterleavedMesh &mesh, const Options &opt, void *dst, Result &out)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0 || dst == nullptr)
        return false;
    Source s{static_cast<const unsigned char *>(mesh.data) + static_cast<std::size_t>(mesh.first) * mesh.stride,
             mesh.stride, &mesh};
    out = Result();
    out.lit = mesh.hasNormals;
    return out.lit ? packT<VertexLit>(s, opt, dst, out, true) : packT<VertexPlain>(s, opt, dst, out, false);
}

void fillQuadIndices(std::uint16_t *dst, int quadCount)
{
    for (int q = 0; q < quadCount; ++q)
    {
        const std::uint16_t b = static_cast<std::uint16_t>(q * 4);
        dst[0] = b;
        dst[1] = static_cast<std::uint16_t>(b + 1);
        dst[2] = static_cast<std::uint16_t>(b + 2);
        dst[3] = b;
        dst[4] = static_cast<std::uint16_t>(b + 2);
        dst[5] = static_cast<std::uint16_t>(b + 3);
        dst += 6;
    }
}
} // namespace PspPack
