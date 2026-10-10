#pragma once
// Converts the engine's interleaved vertex layouts into the PSP Graphics Engine's vertex format.
// Pure C++ (no PSP headers) so it is unit-tested on the host.
//
// Source layouts (RenderInterleavedMesh): position float3 at +0 or int16x3 when positionShort;
// float2 UV at texCoordOffset; RGBA bytes at colorOffset; signed-byte normal at normalOffset;
// packed lightmap brightness (sky<<20 | block<<4) at brightnessOffset.
#include <cstddef>
#include <cstdint>

#include "platform/RenderAPI.h"

namespace PspPack
{
// GE vertex, no normal: texture(2f) color(8888) position(3f). 24 bytes.
struct VertexPlain
{
    float u, v;
    std::uint32_t color;
    float x, y, z;
};

// GE vertex with a signed-byte normal. 28 bytes.
struct VertexLit
{
    float u, v;
    std::uint32_t color;
    std::int8_t nx, ny, nz, pad;
    float x, y, z;
};

enum class Prim
{
    Points,
    Lines,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
    QuadsIndexed // vertices come in groups of 4; draw with the shared quad index table
};

struct Options
{
    // 256 ARGB entries indexed by sky*16 + block. nullptr = no lightmap (vertex colors untouched).
    const std::uint32_t *lightmap = nullptr;
    // Used when the mesh carries no per-vertex brightness: the current lightmap coordinate.
    int lightmapU = 0;
    int lightmapV = 0;
    // Color (ABGR word) used when the mesh has no per-vertex color.
    std::uint32_t currentColor = 0xFFFFFFFFu;
    // Fold the (a,b,c,a,c,d) triangle pairs produced by Tessellator quad expansion back to 4 vertices.
    bool foldQuads = true;
};

struct Result
{
    int vertexCount = 0;
    Prim prim = Prim::Triangles;
    bool lit = false; // VertexLit instead of VertexPlain
};

inline std::size_t vertexBytes(bool lit) { return lit ? sizeof(VertexLit) : sizeof(VertexPlain); }

// Worst case vertices written for `mesh` (for sizing the destination).
inline int maxVertices(const RenderInterleavedMesh &mesh) { return mesh.count + 2; }

// Writes the converted vertices to dst (needs maxVertices(mesh) * vertexBytes(lit) bytes).
// Returns false if the layout cannot be represented.
bool pack(const RenderInterleavedMesh &mesh, const Options &opt, void *dst, Result &out);

// Index table for QuadsIndexed draws: quad q uses vertices 4q..4q+3 as (0,1,2)(0,2,3).
constexpr int kMaxQuadsPerDraw = 10922; // 6 * 10922 = 65532 indices, 16-bit safe
void fillQuadIndices(std::uint16_t *dst, int quadCount);
}
