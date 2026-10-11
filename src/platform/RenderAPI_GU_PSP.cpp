// PSP renderer: implements platform/RenderAPI.h on the PSP Graphics Engine (sceGu).
//
// The engine speaks an OpenGL 1.x fixed-function dialect (matrix stacks, immediate state,
// display lists, a Tessellator that emits interleaved vertices). The GE is a fixed-function
// pipeline of the same generation, so most calls map directly:
//   - matrices: kept as GL-style stacks on the CPU, uploaded to the GE world/projection slots
//     (view = identity), so GL's modelview == GE world and lighting happens in eye space.
//   - state (blend, alpha test, depth, cull, fog, scissor, mask): shadowed, written lazily.
//   - vertices: converted once into GE format (float UV, ABGR color, float position, optional
//     8-bit normal) by psp/render/PspVertexPack. Quads are sent as indexed triangles through one
//     shared index table, so terrain costs 4 vertices per quad instead of 6.
//   - display lists: recorded as a small command stream (state changes + packed geometry) and
//     replayed against the live matrices; terrain sections and models are display lists.
//   - lightmap (GL texture unit 1): the GE has one texture unit, so the 16x16 lightmap is
//     applied on the CPU while packing vertices (color * lightmap[sky][block]).
//   - textures: RGBA8888 in system RAM, with atlas-like textures mirrored into VRAM (the GE
//     reads VRAM much faster than main memory).
#include "platform/RenderAPI.h"

#include <malloc.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "psp/render/PspGuDevice.h"
#include "psp/render/PspRenderMath.h"
#include "psp/render/PspVertexPack.h"
#include "psp/system/PspSystem.h"

using PspMath::Mat4;

namespace
{
// ------------------------------------------------------------------ constants ---

constexpr int kStackDepth = 32;
constexpr int kMaxTextureSize = 512;
constexpr int kMinTextureSize = 8;

constexpr int kGlTexture1 = 0x84C1;

// ------------------------------------------------------------------- matrices ---

enum MatrixSlot { SlotModelView = 0, SlotProjection = 1, SlotTexture = 2 };

struct MatrixStack
{
    Mat4 m[kStackDepth];
    int top = 0;
    MatrixStack() { for (int i = 0; i < kStackDepth; ++i) m[i] = PspMath::identity(); }
    Mat4 &cur() { return m[top]; }
};

MatrixStack g_stacks[3];
MatrixStack g_texUnit1Stack; // swallowed: the lightmap's texture matrix has no meaning here
int g_matrixMode = SlotModelView;
float g_polyBias = 0.0f; // clip-space depth bias for polygon offset draws
bool g_mvDirty = true, g_projDirty = true, g_texMatrixDirty = true;
int g_activeUnit = 0;

MatrixStack &activeStack()
{
    if (g_matrixMode == SlotTexture && g_activeUnit == 1)
        return g_texUnit1Stack;
    return g_stacks[g_matrixMode];
}

void markMatrixDirty()
{
    if (g_matrixMode == SlotModelView) g_mvDirty = true;
    else if (g_matrixMode == SlotProjection) g_projDirty = true;
    else if (g_activeUnit == 0) g_texMatrixDirty = true;
}

// ---------------------------------------------------------------- GL-ish state ---

struct State
{
    bool tex2d = false;
    bool lightmap = false; // GL texture unit 1 enabled
    bool blend = false;
    bool alphaTest = false;
    bool depthTest = false;
    bool cull = false;
    bool fog = false;
    bool lighting = false;
    bool light[2] = {false, false};
    bool colorMaterial = false;
    bool polygonOffset = false;
    bool depthMask = true;

    RenderCompare depthFunc = RenderCompare::Less;
    RenderCompare alphaFunc = RenderCompare::Always;
    float alphaRef = 0.0f;
    RenderBlendFactor blendSrc = RenderBlendFactor::One;
    RenderBlendFactor blendDst = RenderBlendFactor::Zero;
    RenderFace cullFace = RenderFace::Back;
    bool colorMask[4] = {true, true, true, true};

    float polyFactor = 0.0f, polyUnits = 0.0f;

    std::uint32_t curColor = 0xFFFFFFFFu; // ABGR
    float clearColor[4] = {0, 0, 0, 1};
    float clearDepth = 1.0f;

    float fogStart = 0.0f, fogEnd = 1.0f, fogDensity = 1.0f;
    RenderFogMode fogMode = RenderFogMode::Linear;
    std::uint32_t fogColor = 0;

    float lightDirEye[2][3] = {{0, 1, 0}, {0, 1, 0}};
    float lightDiffuse[2][3] = {{1, 1, 1}, {0, 0, 0}};
    float modelAmbient[3] = {0.2f, 0.2f, 0.2f};

    int viewport[4] = {0, 0, PspGu::kScreenWidth, PspGu::kScreenHeight};

    int lightmapU = 240, lightmapV = 240; // current GL texture-unit-1 coordinate (0..255)
};

State g_s;

// What has actually been sent to the GE for this frame (written lazily).
struct Applied
{
    int tex2d = -1, blend = -1, alphaTest = -1, depthTest = -1, cull = -1, fog = -1, lighting = -1,
        light0 = -1, light1 = -1;
    int depthMask = -1;
    unsigned blendKey = ~0u, alphaKey = ~0u, depthFuncKey = ~0u, pixelMask = ~0u, cullFaceKey = ~0u;
    unsigned fogKey = ~0u, polyKey = ~0u;
    std::uint32_t fogColor = 0;
    float fogNear = -1.0f, fogFar = -1.0f;
    int boundTex = -1;
    int texEpoch = -1;
    float texScaleU = -1.0f, texScaleV = -1.0f, texOffU = 1e9f, texOffV = 1e9f;
    bool lightsDirty = true;
    bool viewportDirty = true;
};

Applied g_a;

void resetApplied()
{
    g_a = Applied();
    g_mvDirty = g_projDirty = g_texMatrixDirty = true;
}

std::uint32_t packColor(float r, float g, float b, float a)
{
    auto c = [](float v) -> std::uint32_t
    {
        const int i = static_cast<int>(v * 255.0f + 0.5f);
        return static_cast<std::uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// -------------------------------------------------------------------- lightmap ---

std::uint32_t g_lightmap[256];
bool g_lightmapValid = false;

// ------------------------------------------------------------------- textures ---

struct Texture
{
    bool used = false;
    bool created = false; // storage allocated
    int w = 0, h = 0;     // image size
    int tw = 0, th = 0;   // power-of-two storage size
    std::uint32_t *pixels = nullptr; // tw*th RGBA, 16-byte aligned
    bool blur = false, clamp = false;
    int vramOffset = -1;
    int epoch = 0;
    bool atlas = false;             // terrain/item atlas: always point-sampled
    int levels = 1;                 // mip levels stored (tile atlases get a chain, see mipOffset)
    std::size_t totalBytes = 0;     // all levels
};

constexpr int kAtlasMipLevels = 1;  // 256 -> 128 -> 64 (tiles 16 -> 8 -> 4 px): stops distant planks/grass shimmering; cutout alpha is re-binarised per level

inline std::size_t mipOffset(const Texture &t, int level)
{
    std::size_t off = 0;
    for (int l = 0; l < level; ++l)
        off += static_cast<std::size_t>(t.tw >> l) * static_cast<std::size_t>(t.th >> l) * 4;
    return off;
}

std::vector<Texture> g_textures(1); // index 0 unused
std::vector<int> g_freeTextureNames;
int g_boundTexture = 0;
int g_texEpochCounter = 1;

int pow2AtLeast(int v)
{
    int p = kMinTextureSize;
    while (p < v) p <<= 1;
    return p;
}

Texture *boundTex()
{
    if (g_boundTexture <= 0 || g_boundTexture >= static_cast<int>(g_textures.size())) return nullptr;
    Texture &t = g_textures[static_cast<std::size_t>(g_boundTexture)];
    return t.used ? &t : nullptr;
}

// Reuse VRAM blocks of equal size freed by deleted textures.
struct VramBlock { int offset; std::size_t bytes; };
std::vector<VramBlock> g_vramFree;

int vramTake(std::size_t bytes)
{
    for (std::size_t i = 0; i < g_vramFree.size(); ++i)
        if (g_vramFree[i].bytes == bytes)
        {
            const int off = g_vramFree[i].offset;
            g_vramFree.erase(g_vramFree.begin() + static_cast<std::ptrdiff_t>(i));
            return off;
        }
    return PspGu::vramAlloc(bytes);
}

void releaseTextureStorage(Texture &t)
{
    if (t.vramOffset >= 0)
        g_vramFree.push_back({t.vramOffset, t.totalBytes});
    t.vramOffset = -1;
    free(t.pixels);
    t.pixels = nullptr;
    t.created = false;
}

void mirrorToVram(Texture &t, int y0, int rows)
{
    if (t.vramOffset < 0 || !t.pixels) return;
    unsigned char *dst = reinterpret_cast<unsigned char *>(0x44000000u + static_cast<unsigned>(t.vramOffset));
    const std::size_t rowBytes = static_cast<std::size_t>(t.tw) * 4;
    std::memcpy(dst + rowBytes * static_cast<std::size_t>(y0),
                reinterpret_cast<unsigned char *>(t.pixels) + rowBytes * static_cast<std::size_t>(y0),
                rowBytes * static_cast<std::size_t>(rows));
}

// Rebuilds the mip chain for the level-0 rectangle (x,y,w,h) with an alpha-weighted box filter
// (transparent texels must not darken the fringes of leaves/glass), and pushes the touched rows of
// every level to VRAM / out of the data cache.
void rebuildMips(Texture &t, int x, int y, int w, int h)
{
    if (t.levels <= 1 || !t.pixels) return;
    unsigned char *base = reinterpret_cast<unsigned char *>(t.pixels);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h; // region in level-(l-1) texels
    for (int l = 1; l < t.levels; ++l)
    {
        const int pw = t.tw >> (l - 1), cw = t.tw >> l, ch = t.th >> l;
        x0 >>= 1; y0 >>= 1; x1 = (x1 + 1) >> 1; y1 = (y1 + 1) >> 1;
        if (x1 > cw) x1 = cw;
        if (y1 > ch) y1 = ch;
        const unsigned char *src = base + mipOffset(t, l - 1);
        unsigned char *dst = base + mipOffset(t, l);
        for (int cy = y0; cy < y1; ++cy)
            for (int cx = x0; cx < x1; ++cx)
            {
                const unsigned char *p0 = src + (static_cast<std::size_t>(cy * 2) * pw + cx * 2) * 4;
                const unsigned char *p1 = p0 + 4;
                const unsigned char *p2 = p0 + static_cast<std::size_t>(pw) * 4;
                const unsigned char *p3 = p2 + 4;
                const unsigned a0 = p0[3], a1 = p1[3], a2 = p2[3], a3 = p3[3];
                const unsigned asum = a0 + a1 + a2 + a3;
                unsigned char *o = dst + (static_cast<std::size_t>(cy) * cw + cx) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    o[c] = asum ? static_cast<unsigned char>((p0[c] * a0 + p1[c] * a1 + p2[c] * a2 + p3[c] * a3) / asum)
                                : static_cast<unsigned char>((p0[c] + p1[c] + p2[c] + p3[c]) / 4);
                }
                {
                    // Uniform alpha (opaque or translucent water/ice/glass) just averages. Cutout edges
                    // (a hole in the 2x2) are re-binarised so mips never become semi-transparent
                    // "glassy" leaf/grass quads.
                    const unsigned mn = std::min(std::min(a0, a1), std::min(a2, a3));
                    const unsigned mx = std::max(std::max(a0, a1), std::max(a2, a3));
                    const unsigned avg = asum / 4;
                    o[3] = static_cast<unsigned char>(mn > 0 ? avg : (avg >= 96 ? mx : 0));
                }
            }
        if (x1 > x0 && y1 > y0)
        {
            const std::size_t rowOff = mipOffset(t, l) + static_cast<std::size_t>(y0) * cw * 4;
            const std::size_t bytes = static_cast<std::size_t>(y1 - y0) * cw * 4;
            sceKernelDcacheWritebackRange(base + rowOff, bytes);
            if (t.vramOffset >= 0)
                std::memcpy(reinterpret_cast<unsigned char *>(0x44000000u + static_cast<unsigned>(t.vramOffset)) + rowOff,
                            base + rowOff, bytes);
        }
    }
}

// ------------------------------------------------------------------- geometry ---

struct PackedGeom
{
    void *verts = nullptr;
    int vertexCount = 0;
    PspPack::Prim prim = PspPack::Prim::Triangles;
    bool lit = false;
    int refs = 0;
    // The mesh carried no per-vertex colour, so GL would colour it with whatever glColor is current
    // when the list is *replayed* (font glyph lists rely on this). `packedColor` is what was baked
    // in at record time; submit() re-colours a copy when the current colour differs.
    bool needsColor = false;
    std::uint32_t packedColor = 0xFFFFFFFFu;
};

std::uint16_t *g_quadIndices = nullptr;

bool ensureQuadIndices()
{
    if (g_quadIndices) return true;
    g_quadIndices = static_cast<std::uint16_t *>(memalign(16, PspPack::kMaxQuadsPerDraw * 6 * sizeof(std::uint16_t)));
    if (!g_quadIndices) return false;
    PspPack::fillQuadIndices(g_quadIndices, PspPack::kMaxQuadsPerDraw);
    sceKernelDcacheWritebackRange(g_quadIndices, PspPack::kMaxQuadsPerDraw * 6 * sizeof(std::uint16_t));
    return true;
}

PspPack::Options packOptions()
{
    PspPack::Options o;
    if (g_s.lightmap && g_lightmapValid)
        o.lightmap = g_lightmap;
    o.lightmapU = g_s.lightmapU;
    o.lightmapV = g_s.lightmapV;
    o.currentColor = g_s.curColor;
    return o;
}

// ----------------------------------------------------------------- display list ---

enum class Op : std::uint8_t
{
    Draw, MatrixMode, LoadIdentity, Push, Pop, Translate, Rotate, Scale, Color4f, Normal3f,
    Bind, Enable, Disable, BlendFunc, AlphaFunc, DepthMask, DepthFunc, CullFace, LightmapCoord,
    ColorMask, ShadeModel
};

struct Cmd
{
    Op op;
    int i0 = 0, i1 = 0;
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    PackedGeom *geom = nullptr;
};

struct DisplayList
{
    std::vector<Cmd> cmds;
    bool defined = false;
};

std::vector<DisplayList> g_lists(1);
int g_recording = 0; // list id being recorded, 0 = none

void geomRelease(PackedGeom *g)
{
    if (!g) return;
    if (--g->refs <= 0)
    {
        free(g->verts);
        delete g;
    }
}

void listClear(DisplayList &l)
{
    for (Cmd &c : l.cmds)
        if (c.op == Op::Draw) geomRelease(c.geom);
    l.cmds.clear();
    l.cmds.shrink_to_fit();
    l.defined = false;
}

inline bool recording() { return g_recording != 0; }

DisplayList &recordingList() { return g_lists[static_cast<std::size_t>(g_recording)]; }

inline void rec(Cmd c) { recordingList().cmds.push_back(c); }

// -------------------------------------------------------------------- GE helpers ---

int geCompare(RenderCompare c)
{
    switch (c)
    {
    case RenderCompare::Never: return GU_NEVER;
    case RenderCompare::Less: return GU_LESS;
    case RenderCompare::Equal: return GU_EQUAL;
    case RenderCompare::LessEqual: return GU_LEQUAL;
    case RenderCompare::Greater: return GU_GREATER;
    case RenderCompare::NotEqual: return GU_NOTEQUAL;
    case RenderCompare::GreaterEqual: return GU_GEQUAL;
    case RenderCompare::Always: return GU_ALWAYS;
    }
    return GU_ALWAYS;
}

// Depth runs "backwards" on the GE with sceGuDepthRange(65535, 0): nearer = larger.
int geDepthCompare(RenderCompare c)
{
    switch (c)
    {
    case RenderCompare::Less: return GU_GREATER;
    case RenderCompare::LessEqual: return GU_GEQUAL;
    case RenderCompare::Greater: return GU_LESS;
    case RenderCompare::GreaterEqual: return GU_LEQUAL;
    default: return geCompare(c);
    }
}

void applyToggle(int &applied, bool want, int geCap)
{
    const int w = want ? 1 : 0;
    if (applied == w) return;
    applied = w;
    if (want) sceGuEnable(geCap);
    else sceGuDisable(geCap);
}

struct BlendSlot { int func; unsigned fix; };

BlendSlot geBlendSrc(RenderBlendFactor f)
{
    switch (f)
    {
    case RenderBlendFactor::Zero: return {GU_FIX, 0x000000};
    case RenderBlendFactor::One: return {GU_FIX, 0xFFFFFF};
    case RenderBlendFactor::DstColor: return {GU_DST_COLOR, 0};
    case RenderBlendFactor::OneMinusDstColor: return {GU_ONE_MINUS_DST_COLOR, 0};
    case RenderBlendFactor::SrcAlpha: return {GU_SRC_ALPHA, 0};
    case RenderBlendFactor::OneMinusSrcAlpha: return {GU_ONE_MINUS_SRC_ALPHA, 0};
    case RenderBlendFactor::DstAlpha: return {GU_DST_ALPHA, 0};
    case RenderBlendFactor::OneMinusDstAlpha: return {GU_ONE_MINUS_DST_ALPHA, 0};
    case RenderBlendFactor::SrcColor: return {GU_SRC_ALPHA, 0};          // not expressible as a source factor
    case RenderBlendFactor::OneMinusSrcColor: return {GU_ONE_MINUS_SRC_ALPHA, 0};
    }
    return {GU_FIX, 0xFFFFFF};
}

BlendSlot geBlendDst(RenderBlendFactor f)
{
    switch (f)
    {
    case RenderBlendFactor::Zero: return {GU_FIX, 0x000000};
    case RenderBlendFactor::One: return {GU_FIX, 0xFFFFFF};
    case RenderBlendFactor::SrcColor: return {GU_SRC_COLOR, 0};
    case RenderBlendFactor::OneMinusSrcColor: return {GU_ONE_MINUS_SRC_COLOR, 0};
    case RenderBlendFactor::SrcAlpha: return {GU_SRC_ALPHA, 0};
    case RenderBlendFactor::OneMinusSrcAlpha: return {GU_ONE_MINUS_SRC_ALPHA, 0};
    case RenderBlendFactor::DstAlpha: return {GU_DST_ALPHA, 0};
    case RenderBlendFactor::OneMinusDstAlpha: return {GU_ONE_MINUS_DST_ALPHA, 0};
    case RenderBlendFactor::DstColor: return {GU_DST_ALPHA, 0};
    case RenderBlendFactor::OneMinusDstColor: return {GU_ONE_MINUS_DST_ALPHA, 0};
    }
    return {GU_FIX, 0};
}

void uploadMatrices()
{
    if (g_mvDirty)
    {
        sceGuSetMatrix(GU_MODEL, reinterpret_cast<const ScePspFMatrix4 *>(g_stacks[SlotModelView].cur().m));
        g_mvDirty = false;
        g_a.lightsDirty = true; // eye-space light direction depends on the modelview at set time only
    }
    if (g_projDirty)
    {
        // Polygon offset: the GE's own depth offset is unreliable with the reversed depth range, so pull
        // the geometry towards the camera in clip space instead (z' = z - bias * w).
        Mat4 pm = g_stacks[SlotProjection].cur();
        if (g_polyBias != 0.0f)
        {
            pm.m[2] -= g_polyBias * pm.m[3];
            pm.m[6] -= g_polyBias * pm.m[7];
            pm.m[10] -= g_polyBias * pm.m[11];
            pm.m[14] -= g_polyBias * pm.m[15];
        }
        sceGuSetMatrix(GU_PROJECTION, reinterpret_cast<const ScePspFMatrix4 *>(pm.m));
        g_projDirty = false;
    }
}

void applyViewport()
{
    if (!g_a.viewportDirty) return;
    g_a.viewportDirty = false;
    const int offX = 2048 - PspGu::kScreenWidth / 2;
    const int offY = 2048 - PspGu::kScreenHeight / 2;
    const int x = g_s.viewport[0], w = g_s.viewport[2], h = g_s.viewport[3];
    const int yTop = PspGu::kScreenHeight - (g_s.viewport[1] + h);
    sceGuOffset(static_cast<unsigned>(offX), static_cast<unsigned>(offY));
    sceGuViewport(offX + x + w / 2, offY + yTop + h / 2, w, h);
    sceGuScissor(std::max(0, x), std::max(0, yTop), std::min(PspGu::kScreenWidth, x + w),
                 std::min(PspGu::kScreenHeight, yTop + h));
}

void applyTexture()
{
    Texture *t = boundTex();
    if (!t || !t->created)
    {
        return;
    }
    if (g_a.boundTex != g_boundTexture || g_a.texEpoch != t->epoch)
    {
        g_a.boundTex = g_boundTexture;
        g_a.texEpoch = t->epoch;
        sceGuTexMode(GU_PSM_8888, t->levels - 1, 0, 0);
        const unsigned char *src = reinterpret_cast<const unsigned char *>(t->pixels);
        if (t->vramOffset >= 0)
            src = reinterpret_cast<const unsigned char *>(0x04000000u + static_cast<unsigned>(t->vramOffset));
        for (int l = 0; l < t->levels; ++l)
            sceGuTexImage(l, t->tw >> l, t->th >> l, t->tw >> l, src + mipOffset(*t, l));
        if (t->levels > 1)
        {
            sceGuTexLevelMode(GU_TEXTURE_AUTO, -0.4f); // slight bias to the sharper level
            sceGuTexFilter(t->blur ? GU_LINEAR_MIPMAP_LINEAR : GU_NEAREST_MIPMAP_NEAREST,
                           t->blur ? GU_LINEAR : GU_NEAREST);
        }
        else
        {
            const bool lin = t->blur && !t->atlas; // atlases are never smoothed: tiles must stay crisp
            sceGuTexFilter(lin ? GU_LINEAR : GU_NEAREST, lin ? GU_LINEAR : GU_NEAREST);
        }
        sceGuTexWrap(t->clamp ? GU_CLAMP : GU_REPEAT, t->clamp ? GU_CLAMP : GU_REPEAT);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        g_a.texScaleU = -1.0f; // scale depends on the texture
    }
    // GL texture matrix (translate/scale only in this game) -> GE texture scale/offset.
    const Mat4 &tm = g_stacks[SlotTexture].cur();
    const float kU = static_cast<float>(t->w) / static_cast<float>(t->tw);
    const float kV = static_cast<float>(t->h) / static_cast<float>(t->th);
    const float su = tm.m[0] * kU, sv = tm.m[5] * kV;
    const float ou = tm.m[12] * kU, ov = tm.m[13] * kV;
    if (su != g_a.texScaleU || sv != g_a.texScaleV)
    {
        g_a.texScaleU = su; g_a.texScaleV = sv;
        sceGuTexScale(su, sv);
    }
    if (ou != g_a.texOffU || ov != g_a.texOffV)
    {
        g_a.texOffU = ou; g_a.texOffV = ov;
        sceGuTexOffset(ou, ov);
    }
}

void applyFog()
{
    applyToggle(g_a.fog, g_s.fog, GU_FOG);
    if (!g_s.fog) return;
    float nearD = g_s.fogStart, farD = g_s.fogEnd;
    if (g_s.fogMode == RenderFogMode::Exp || g_s.fogMode == RenderFogMode::Exp2)
    {
        const float d = std::max(g_s.fogDensity, 0.0001f);
        nearD = 0.0f;
        farD = (g_s.fogMode == RenderFogMode::Exp ? 3.0f : 2.2f) / d;
    }
    if (farD <= nearD + 0.001f) farD = nearD + 0.01f;
    if (nearD != g_a.fogNear || farD != g_a.fogFar || g_s.fogColor != g_a.fogColor)
    {
        g_a.fogNear = nearD; g_a.fogFar = farD; g_a.fogColor = g_s.fogColor;
        sceGuFog(nearD, farD, g_s.fogColor);
    }
}

void applyLighting(bool meshLit)
{
    const bool on = g_s.lighting && meshLit;
    applyToggle(g_a.lighting, on, GU_LIGHTING);
    if (!on) return;
    applyToggle(g_a.light0, g_s.light[0], GU_LIGHT0);
    applyToggle(g_a.light1, g_s.light[1], GU_LIGHT1);
    if (g_a.lightsDirty)
    {
        g_a.lightsDirty = false;
        for (int i = 0; i < 2; ++i)
        {
            ScePspFVector3 dir = {g_s.lightDirEye[i][0], g_s.lightDirEye[i][1], g_s.lightDirEye[i][2]};
            sceGuLight(i, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
            sceGuLightColor(i, GU_DIFFUSE, packColor(g_s.lightDiffuse[i][0], g_s.lightDiffuse[i][1], g_s.lightDiffuse[i][2], 1.0f));
        }
        sceGuAmbient(packColor(g_s.modelAmbient[0], g_s.modelAmbient[1], g_s.modelAmbient[2], 1.0f));
        sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE);
    }
}

void applyState(bool meshLit)
{
    applyViewport();
    applyToggle(g_a.tex2d, g_s.tex2d, GU_TEXTURE_2D);
    applyToggle(g_a.blend, g_s.blend, GU_BLEND);
    applyToggle(g_a.alphaTest, g_s.alphaTest, GU_ALPHA_TEST);
    {
        // Flat 2D UI (orthographic projection, model-view with no rotation): every quad sits at the
        // same z, and the GE's fixed-point depth interpolation then lets some triangles fail the
        // depth test on real hardware (half-black buttons, missing triangles). Painter's order is
        // already correct there, so skip the depth test. Rotated draws (block icons) keep it.
        bool flat2d = false;
        const Mat4 &pr = g_stacks[SlotProjection].cur();
        const Mat4 &mv = g_stacks[SlotModelView].cur();
        if (pr.m[11] == 0.0f)
        {
            const float e = 1e-4f;
            auto z = [e](float v) { return v < e && v > -e; };
            flat2d = z(mv.m[1]) && z(mv.m[2]) && z(mv.m[4]) && z(mv.m[6]) && z(mv.m[8]) && z(mv.m[9]);
        }
        applyToggle(g_a.depthTest, g_s.depthTest && !flat2d, GU_DEPTH_TEST);
    }
    // Face culling stays off on the GE for now: the title log showed every GUI quad (cull enabled by
    // the game, with a y-flipped ortho projection) being rejected, so the front-face mapping needs a
    // calibration pass first. Depth testing keeps the picture correct meanwhile.
    applyToggle(g_a.cull, PspGu::g_honorCull && g_s.cull, GU_CULL_FACE);

    const unsigned bkey = (static_cast<unsigned>(g_s.blendSrc) << 8) | static_cast<unsigned>(g_s.blendDst);
    if (bkey != g_a.blendKey)
    {
        g_a.blendKey = bkey;
        const BlendSlot s = geBlendSrc(g_s.blendSrc), d = geBlendDst(g_s.blendDst);
        sceGuBlendFunc(GU_ADD, s.func, d.func, s.fix, d.fix);
    }
    const unsigned akey = (static_cast<unsigned>(g_s.alphaFunc) << 16) |
                          static_cast<unsigned>(std::min(255.0f, std::max(0.0f, g_s.alphaRef * 255.0f)));
    if (akey != g_a.alphaKey)
    {
        g_a.alphaKey = akey;
        sceGuAlphaFunc(geCompare(g_s.alphaFunc), static_cast<int>(akey & 0xFFFFu), 0xFF);
    }
    const unsigned dkey = static_cast<unsigned>(g_s.depthFunc);
    if (dkey != g_a.depthFuncKey)
    {
        g_a.depthFuncKey = dkey;
        sceGuDepthFunc(geDepthCompare(g_s.depthFunc));
    }
    if (g_a.depthMask != (g_s.depthMask ? 1 : 0))
    {
        g_a.depthMask = g_s.depthMask ? 1 : 0;
        sceGuDepthMask(g_s.depthMask ? 0 : 1);
    }
    unsigned mask = 0;
    if (!g_s.colorMask[0]) mask |= 0x000000FFu;
    if (!g_s.colorMask[1]) mask |= 0x0000FF00u;
    if (!g_s.colorMask[2]) mask |= 0x00FF0000u;
    if (!g_s.colorMask[3]) mask |= 0xFF000000u;
    if (mask != g_a.pixelMask)
    {
        g_a.pixelMask = mask;
        sceGuPixelMask(mask);
    }
    const unsigned ckey = static_cast<unsigned>(g_s.cullFace);
    if (ckey != g_a.cullFaceKey)
    {
        g_a.cullFaceKey = ckey;
        // GL front faces are counter-clockwise; the GE's flipped Y makes that clockwise here.
        sceGuFrontFace(g_s.cullFace == RenderFace::Front ? GU_CCW : GU_CW);
    }
    const unsigned pkey = g_s.polygonOffset ? static_cast<unsigned>(std::fabs(g_s.polyUnits) * 16.0f) + 1u : 0u;
    if (pkey != g_a.polyKey)
    {
        g_a.polyKey = pkey;
        g_polyBias = pkey ? 2.5e-4f * std::fabs(g_s.polyUnits) : 0.0f;
        g_projDirty = true;
    }
    applyFog();
    applyLighting(meshLit);
    if (g_s.tex2d) applyTexture();
    uploadMatrices();
}

// Diagnostic: describes the first few draws after the title screen is up (boot.log).
void traceDraw(const PackedGeom &g)
{
    static int traced = 0;
    if (traced >= 36 || PspGu::stats().frames < 4) return;
    {
        const Mat4 &p0 = g_stacks[SlotProjection].cur();
        if (p0.m[11] != 0.0f) return; // only orthographic (GUI) draws are interesting
    }
    ++traced;
    const float *f = static_cast<const float *>(g.verts);
    const Texture *t = boundTex();
    const Mat4 &pr = g_stacks[SlotProjection].cur();
    const Mat4 &mv = g_stacks[SlotModelView].cur();
    PspSystem::bootTrace("draw#%d prim=%d n=%d lit=%d tex2d=%d bound=%d(%dx%d%s) blend=%d(%d,%d) at=%d dt=%d dm=%d cull=%d\n",
        traced, static_cast<int>(g.prim), g.vertexCount, g.lit, g.lit ? 0 : g_s.tex2d, g_boundTexture,
        t ? t->w : 0, t ? t->h : 0, (t && t->created) ? "" : ",NOT CREATED", g_s.blend,
        static_cast<int>(g_s.blendSrc), static_cast<int>(g_s.blendDst), g_s.alphaTest, g_s.depthTest,
        g_s.depthMask, g_s.cull);
    PspSystem::bootTrace("   v0 uv=%.3f,%.3f col=%08x xyz=%.1f,%.1f,%.1f  proj[0,5,10,12,13,14]=%.4f %.4f %.4f %.1f %.1f %.1f  mv[12,13,14]=%.1f %.1f %.1f\n",
        f[0], f[1], *reinterpret_cast<const unsigned *>(f + 2), f[3], f[4], f[5],
        pr.m[0], pr.m[5], pr.m[10], pr.m[12], pr.m[13], pr.m[14], mv.m[12], mv.m[13], mv.m[14]);
}

void submit(const PackedGeom &g)
{
    if (!g.verts || g.vertexCount <= 0) return;
    // traceDraw(g);
    applyState(g.lit);
    const void *vertsToDraw = g.verts;
    if (g.needsColor && g.packedColor != g_s.curColor && !g_s.lightmap)
    {
        const std::size_t vs = PspPack::vertexBytes(g.lit);
        unsigned char *copy = static_cast<unsigned char *>(PspGu::frameAlloc(static_cast<std::size_t>(g.vertexCount) * vs));
        if (copy)
        {
            std::memcpy(copy, g.verts, static_cast<std::size_t>(g.vertexCount) * vs);
            for (int i = 0; i < g.vertexCount; ++i)
                std::memcpy(copy + static_cast<std::size_t>(i) * vs + 8, &g_s.curColor, sizeof(std::uint32_t));
            vertsToDraw = copy;
        }
    }
    const int vtype = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D | (g.lit ? GU_NORMAL_8BIT : 0);
    const std::size_t vsize = PspPack::vertexBytes(g.lit);
    if (g.prim == PspPack::Prim::QuadsIndexed)
    {
        if (!ensureQuadIndices()) return;
        int quads = g.vertexCount / 4;
        const unsigned char *base = static_cast<const unsigned char *>(vertsToDraw);
        while (quads > 0)
        {
            const int n = std::min(quads, PspPack::kMaxQuadsPerDraw);
            sceGuDrawArray(GU_TRIANGLES, vtype | GU_INDEX_16BIT, n * 6, g_quadIndices, base);
            base += static_cast<std::size_t>(n) * 4 * vsize;
            quads -= n;
        }
        PspGu::countDraw(static_cast<unsigned>(g.vertexCount));
        return;
    }
    int gePrim = GU_TRIANGLES;
    switch (g.prim)
    {
    case PspPack::Prim::Points: gePrim = GU_POINTS; break;
    case PspPack::Prim::Lines: gePrim = GU_LINES; break;
    case PspPack::Prim::LineStrip: gePrim = GU_LINE_STRIP; break;
    case PspPack::Prim::Triangles: gePrim = GU_TRIANGLES; break;
    case PspPack::Prim::TriangleStrip: gePrim = GU_TRIANGLE_STRIP; break;
    case PspPack::Prim::TriangleFan: gePrim = GU_TRIANGLE_FAN; break;
    default: break;
    }
    sceGuDrawArray(gePrim, vtype, g.vertexCount, nullptr, vertsToDraw);
    PspGu::countDraw(static_cast<unsigned>(g.vertexCount));
}

// Packs into the per-frame arena and draws immediately.
bool drawImmediate(const RenderInterleavedMesh &mesh)
{
    const PspPack::Options opt = packOptions();
    const std::size_t bytes = static_cast<std::size_t>(PspPack::maxVertices(mesh)) *
                              PspPack::vertexBytes(mesh.hasNormals);
    void *mem = PspGu::frameAlloc(bytes);
    if (!mem) return false;
    PspPack::Result res;
    if (!PspPack::pack(mesh, opt, mem, res)) return false;
    PackedGeom g;
    g.verts = mem;
    g.vertexCount = res.vertexCount;
    g.prim = res.prim;
    g.lit = res.lit;
    submit(g);
    return true;
}

// Packs into long-lived memory (display lists, persistent meshes).
PackedGeom *packPersistent(const RenderInterleavedMesh &mesh)
{
    const PspPack::Options opt = packOptions();
    const std::size_t bytes = static_cast<std::size_t>(PspPack::maxVertices(mesh)) *
                              PspPack::vertexBytes(mesh.hasNormals);
    void *mem = memalign(16, bytes);
    if (!mem) return nullptr;
    PspPack::Result res;
    if (!PspPack::pack(mesh, opt, mem, res))
    {
        free(mem);
        return nullptr;
    }
    PackedGeom *g = new PackedGeom();
    g->verts = mem;
    g->vertexCount = res.vertexCount;
    g->prim = res.prim;
    g->lit = res.lit;
    g->refs = 1;
    g->needsColor = !mesh.hasColor && !mesh.positionShort;
    g->packedColor = opt.currentColor;
    // Shrink to what was used; the vertex data must also be flushed for the GE.
    const std::size_t used = static_cast<std::size_t>(res.vertexCount) * PspPack::vertexBytes(res.lit);
    if (used + 64 < bytes)
    {
        void *smaller = realloc(mem, used ? used : 16);
        if (smaller && (reinterpret_cast<std::uintptr_t>(smaller) & 15u) == 0) g->verts = smaller;
        else if (smaller) g->verts = smaller; // alignment is checked below
    }
    if (reinterpret_cast<std::uintptr_t>(g->verts) & 15u)
    {
        void *aligned = memalign(16, used ? used : 16);
        if (!aligned) { geomRelease(g); return nullptr; }
        std::memcpy(aligned, g->verts, used);
        free(g->verts);
        g->verts = aligned;
    }
    sceKernelDcacheWritebackRange(g->verts, used);
    return g;
}

// ---------------------------------------------------------------- persistent meshes ---

std::vector<PackedGeom *> g_persistent(1, nullptr);
std::vector<int> g_persistentFree;

void clearAllDisplayListsAndMeshes()
{
    for (DisplayList &l : g_lists) listClear(l);
    for (PackedGeom *&g : g_persistent) { geomRelease(g); g = nullptr; }
}

// ------------------------------------------------------------------- replay ---

void replay(const DisplayList &list);

} // namespace

// ============================================================================= API ===

void renderEnable(RenderCapability capability)
{
    if (recording()) { Cmd c{Op::Enable}; c.i0 = static_cast<int>(capability); rec(c); return; }
    switch (capability)
    {
    case RenderCapability::Texture2D: if (g_activeUnit == 1) g_s.lightmap = true; else g_s.tex2d = true; break;
    case RenderCapability::ColorMaterial: g_s.colorMaterial = true; break;
    case RenderCapability::CullFace: g_s.cull = true; break;
    case RenderCapability::AlphaTest: g_s.alphaTest = true; break;
    case RenderCapability::Blend: g_s.blend = true; break;
    case RenderCapability::DepthTest: g_s.depthTest = true; break;
    case RenderCapability::Fog: g_s.fog = true; break;
    case RenderCapability::Lighting: g_s.lighting = true; break;
    case RenderCapability::Light0: g_s.light[0] = true; g_a.lightsDirty = true; break;
    case RenderCapability::Light1: g_s.light[1] = true; g_a.lightsDirty = true; break;
    case RenderCapability::PolygonOffsetFill: g_s.polygonOffset = true; break;
    default: break;
    }
}

void renderDisable(RenderCapability capability)
{
    if (recording()) { Cmd c{Op::Disable}; c.i0 = static_cast<int>(capability); rec(c); return; }
    switch (capability)
    {
    case RenderCapability::Texture2D: if (g_activeUnit == 1) g_s.lightmap = false; else g_s.tex2d = false; break;
    case RenderCapability::ColorMaterial: g_s.colorMaterial = false; break;
    case RenderCapability::CullFace: g_s.cull = false; break;
    case RenderCapability::AlphaTest: g_s.alphaTest = false; break;
    case RenderCapability::Blend: g_s.blend = false; break;
    case RenderCapability::DepthTest: g_s.depthTest = false; break;
    case RenderCapability::Fog: g_s.fog = false; break;
    case RenderCapability::Lighting: g_s.lighting = false; break;
    case RenderCapability::Light0: g_s.light[0] = false; g_a.lightsDirty = true; break;
    case RenderCapability::Light1: g_s.light[1] = false; g_a.lightsDirty = true; break;
    case RenderCapability::PolygonOffsetFill: g_s.polygonOffset = false; break;
    default: break;
    }
}

void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination)
{
    if (recording()) { Cmd c{Op::BlendFunc}; c.i0 = static_cast<int>(source); c.i1 = static_cast<int>(destination); rec(c); return; }
    g_s.blendSrc = source;
    g_s.blendDst = destination;
}

void renderDepthMask(bool enabled)
{
    if (recording()) { Cmd c{Op::DepthMask}; c.i0 = enabled; rec(c); return; }
    g_s.depthMask = enabled;
}

void renderDepthFunc(RenderCompare function)
{
    if (recording()) { Cmd c{Op::DepthFunc}; c.i0 = static_cast<int>(function); rec(c); return; }
    g_s.depthFunc = function;
}

void renderAlphaFunc(RenderCompare function, float reference)
{
    if (recording()) { Cmd c{Op::AlphaFunc}; c.i0 = static_cast<int>(function); c.f0 = reference; rec(c); return; }
    g_s.alphaFunc = function;
    g_s.alphaRef = reference;
}

void renderCullFace(RenderFace face)
{
    if (recording()) { Cmd c{Op::CullFace}; c.i0 = static_cast<int>(face); rec(c); return; }
    g_s.cullFace = face;
}

void renderColorMask(bool red, bool green, bool blue, bool alpha)
{
    if (recording()) { Cmd c{Op::ColorMask}; c.i0 = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0); rec(c); return; }
    g_s.colorMask[0] = red; g_s.colorMask[1] = green; g_s.colorMask[2] = blue; g_s.colorMask[3] = alpha;
}

void renderBindTexture(int texture)
{
    if (recording()) { Cmd c{Op::Bind}; c.i0 = texture; rec(c); return; }
    if (g_activeUnit == 1) return; // the lightmap is applied on the CPU
    g_boundTexture = texture;
}

void renderSetActiveTextureUnit(int textureUnit) { g_activeUnit = textureUnit == kGlTexture1 ? 1 : 0; }
void renderSetClientActiveTextureUnit(int) {}

void renderSetMultiTextureCoord(int textureUnit, float u, float v)
{
    if (textureUnit != kGlTexture1) return;
    if (recording()) { Cmd c{Op::LightmapCoord}; c.f0 = u; c.f1 = v; rec(c); return; }
    g_s.lightmapU = static_cast<int>(u);
    g_s.lightmapV = static_cast<int>(v);
}

void renderSetLightmapColors(const std::uint32_t *colors, int count)
{
    if (!colors || count < 256) return;
    std::memcpy(g_lightmap, colors, sizeof(g_lightmap));
    g_lightmapValid = true;
}

void renderColor4f(float r, float g, float b, float a)
{
    if (recording()) { Cmd c{Op::Color4f}; c.f0 = r; c.f1 = g; c.f2 = b; c.f3 = a; rec(c); return; }
    g_s.curColor = packColor(r, g, b, a);
}

void renderColor3f(float r, float g, float b) { renderColor4f(r, g, b, 1.0f); }

void renderNormal3f(float, float, float) {}

// ----------------------------------------------------------------------- textures ---

void renderGenerateTextures(int count, int *textures)
{
    for (int i = 0; i < count; ++i)
    {
        int name;
        if (!g_freeTextureNames.empty())
        {
            name = g_freeTextureNames.back();
            g_freeTextureNames.pop_back();
        }
        else
        {
            name = static_cast<int>(g_textures.size());
            g_textures.emplace_back();
        }
        Texture &t = g_textures[static_cast<std::size_t>(name)];
        t = Texture();
        t.used = true;
        textures[i] = name;
    }
}

void renderDeleteTextures(int count, const int *textures)
{
    for (int i = 0; i < count; ++i)
    {
        const int name = textures[i];
        if (name <= 0 || name >= static_cast<int>(g_textures.size())) continue;
        Texture &t = g_textures[static_cast<std::size_t>(name)];
        if (!t.used) continue;
        releaseTextureStorage(t);
        t = Texture();
        g_freeTextureNames.push_back(name);
        if (g_boundTexture == name) g_boundTexture = 0;
        g_a.boundTex = -1;
    }
}

bool renderTextureBeginUpload(int texture, int width, int height, int, bool blur, bool clamp, bool tileAtlas, bool)
{
    if (texture <= 0 || texture >= static_cast<int>(g_textures.size())) return false;
    Texture &t = g_textures[static_cast<std::size_t>(texture)];
    if (!t.used) return false;
    g_boundTexture = texture;

    const int tw = pow2AtLeast(width), th = pow2AtLeast(height);
    if (tw > kMaxTextureSize || th > kMaxTextureSize || width <= 0 || height <= 0) return false;
    const int wantLevels = (tileAtlas && width == tw && height == th && tw >= 64 && th >= 64) ? kAtlasMipLevels : 1;
    if (!t.created || t.tw != tw || t.th != th || t.levels != wantLevels)
    {
        releaseTextureStorage(t);
        t.tw = tw;
        t.th = th;
        t.levels = wantLevels;
        const std::size_t bytes = static_cast<std::size_t>(tw) * th * 4;
        t.totalBytes = mipOffset(t, wantLevels - 1) + (wantLevels > 1 ? static_cast<std::size_t>(tw >> (wantLevels - 1)) * (th >> (wantLevels - 1)) * 4 : bytes);
        if (wantLevels == 1) t.totalBytes = bytes;
        t.pixels = static_cast<std::uint32_t *>(memalign(16, t.totalBytes));
        if (!t.pixels) return false;
        std::memset(t.pixels, 0, t.totalBytes);
        t.created = true;
        // Mirror textures that matter for fill rate into VRAM when there is room.
        if (tileAtlas || bytes <= 64 * 1024)
            t.vramOffset = vramTake(t.totalBytes);
    }
    t.w = width;
    t.h = height;
    t.blur = blur;
    t.clamp = clamp;
    t.atlas = tileAtlas;
    if (width >= 128 || tileAtlas)
        PspSystem::bootTrace("texture upload id=%d %dx%d atlas=%d blur=%d clamp=%d vram=%s free=%uK", texture, width, height, tileAtlas ? 1 : 0, blur ? 1 : 0, clamp ? 1 : 0, t.vramOffset >= 0 ? "yes" : "no", (unsigned)(PspGu::vramFree() / 1024));
    t.epoch = g_texEpochCounter++;
    return true;
}

bool renderTextureIsValid(int texture)
{
    return texture > 0 && texture < static_cast<int>(g_textures.size()) &&
           g_textures[static_cast<std::size_t>(texture)].created;
}

// Tall grass / fern sprites are high-contrast per-pixel noise; without mipmaps they shimmer badly on a
// 480x272 screen. Pull each texel 30% toward the tile's mean colour (alpha untouched) so they are calm.
void softenGrassTile(Texture &t, int tileIndex)
{
    const int ts = t.tw / 16;
    if (ts < 4) return;
    const int x0 = (tileIndex % 16) * ts, y0 = (tileIndex / 16) * ts;
    unsigned sr = 0, sg = 0, sb = 0, n = 0;
    for (int y = 0; y < ts; ++y)
        for (int x = 0; x < ts; ++x)
        {
            const std::uint32_t c = t.pixels[static_cast<std::size_t>(y0 + y) * t.tw + x0 + x];
            if ((c >> 24) == 0) continue;
            sr += c & 255; sg += (c >> 8) & 255; sb += (c >> 16) & 255; ++n;
        }
    if (!n) return;
    const int mr = sr / n, mg = sg / n, mb = sb / n;
    for (int y = 0; y < ts; ++y)
        for (int x = 0; x < ts; ++x)
        {
            std::uint32_t &c = t.pixels[static_cast<std::size_t>(y0 + y) * t.tw + x0 + x];
            if ((c >> 24) == 0) continue;
            const int r = (c & 255), g = (c >> 8) & 255, b = (c >> 16) & 255;
            const int nr = r + (mr - r) * 3 / 10, ng = g + (mg - g) * 3 / 10, nb = b + (mb - b) * 3 / 10;
            c = (c & 0xFF000000u) | (static_cast<std::uint32_t>(nb) << 16) | (static_cast<std::uint32_t>(ng) << 8) | static_cast<std::uint32_t>(nr);
        }
}

void renderTextureImageRgba(int level, int width, int height, const void *pixels)
{
    if (level != 0) return; // no mipmaps yet
    Texture *t = boundTex();
    if (!t || !t->created || !pixels) return;
    const int w = std::min(width, t->tw), h = std::min(height, t->th);
    const unsigned char *src = static_cast<const unsigned char *>(pixels);
    for (int y = 0; y < h; ++y)
        std::memcpy(reinterpret_cast<unsigned char *>(t->pixels) + static_cast<std::size_t>(y) * t->tw * 4,
                    src + static_cast<std::size_t>(y) * width * 4, static_cast<std::size_t>(w) * 4);
    if (t->atlas && t->tw == 256 && t->th == 256)
    {
        softenGrassTile(*t, 39); // tall grass
        softenGrassTile(*t, 56); // fern
    }
    sceKernelDcacheWritebackRange(t->pixels, static_cast<std::size_t>(t->tw) * t->th * 4);
    mirrorToVram(*t, 0, t->th);
    rebuildMips(*t, 0, 0, t->tw, t->th);
    sceGuTexFlush();
    ++t->epoch;
}

void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void *pixels)
{
    if (level != 0) return;
    Texture *t = boundTex();
    if (!t || !t->created || !pixels) return;
    if (x < 0 || y < 0 || x + width > t->tw || y + height > t->th) return;
    const unsigned char *src = static_cast<const unsigned char *>(pixels);
    for (int row = 0; row < height; ++row)
        std::memcpy(reinterpret_cast<unsigned char *>(t->pixels) + (static_cast<std::size_t>(y + row) * t->tw + x) * 4,
                    src + static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4);
    sceKernelDcacheWritebackRange(reinterpret_cast<unsigned char *>(t->pixels) + static_cast<std::size_t>(y) * t->tw * 4,
                                  static_cast<std::size_t>(height) * t->tw * 4);
    mirrorToVram(*t, y, height);
    rebuildMips(*t, x, y, width, height);
    g_a.boundTex = -1; // force a texture-cache flush on the next draw
    sceGuTexFlush();
}

void renderTextureParameters(bool blur, bool, bool clamp)
{
    Texture *t = boundTex();
    if (!t) return;
    t->blur = blur;
    t->clamp = clamp;
    ++t->epoch;
}

#if PLATFORM_TEXTURE_QUALITY_CONTROLS
void renderApplyTextureQuality(bool, int, bool, int) {}
#endif
int renderGetMaxAnisotropy() { return 1; }
int renderGetMaxSamples() { return 0; }

void renderResetResources()
{
    clearAllDisplayListsAndMeshes();
    for (std::size_t i = 1; i < g_textures.size(); ++i)
    {
        if (g_textures[i].used) releaseTextureStorage(g_textures[i]);
        g_textures[i] = Texture();
    }
    g_textures.resize(1);
    g_freeTextureNames.clear();
    g_vramFree.clear();
    PspGu::vramReset();
    g_boundTexture = 0;
    g_a.boundTex = -1;
}

// ---------------------------------------------------------------------------- fog ---

void renderFogf(RenderFogParameter parameter, float value)
{
    switch (parameter)
    {
    case RenderFogParameter::Density: g_s.fogDensity = value; break;
    case RenderFogParameter::Start: g_s.fogStart = value; break;
    case RenderFogParameter::End: g_s.fogEnd = value; break;
    default: break;
    }
}

void renderFogi(RenderFogParameter parameter, RenderFogMode value)
{
    if (parameter == RenderFogParameter::Mode) g_s.fogMode = value;
}

void renderFogColor(const float *values)
{
    if (!values) return;
    g_s.fogColor = packColor(values[0], values[1], values[2], 1.0f);
}

void renderFogHint(RenderHintMode) {}

// ------------------------------------------------------------------------ lighting ---

void renderLightfv(int lightIndex, RenderLightParameter parameter, const float *values)
{
    if (lightIndex < 0 || lightIndex > 1 || !values) return;
    if (parameter == RenderLightParameter::Position)
    {
        PspMath::transformDirection(g_stacks[SlotModelView].cur(), values, g_s.lightDirEye[lightIndex]);
        g_a.lightsDirty = true;
    }
    else if (parameter == RenderLightParameter::Diffuse)
    {
        g_s.lightDiffuse[lightIndex][0] = values[0];
        g_s.lightDiffuse[lightIndex][1] = values[1];
        g_s.lightDiffuse[lightIndex][2] = values[2];
        g_a.lightsDirty = true;
    }
}

void renderLightModelAmbient(const float *values)
{
    if (!values) return;
    g_s.modelAmbient[0] = values[0]; g_s.modelAmbient[1] = values[1]; g_s.modelAmbient[2] = values[2];
    g_a.lightsDirty = true;
}

void renderColorMaterial(RenderFace, RenderColorMaterialMode) {}

void renderShadeModel(RenderShadeModel model)
{
    sceGuShadeModel(model == RenderShadeModel::Flat ? GU_FLAT : GU_SMOOTH);
}

// ------------------------------------------------------------------- frame control ---

void renderClear(unsigned int mask)
{
    int flags = 0;
    if (mask & RenderClearMask::Color) flags |= GU_COLOR_BUFFER_BIT;
    if (mask & RenderClearMask::Depth) flags |= GU_DEPTH_BUFFER_BIT;
    if (!flags) return;
    sceGuClearColor(packColor(g_s.clearColor[0], g_s.clearColor[1], g_s.clearColor[2], g_s.clearColor[3]));
    sceGuClearDepth(static_cast<unsigned>((1.0f - g_s.clearDepth) * 65535.0f));
    sceGuClear(flags | (flags == (GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT) ? GU_FAST_CLEAR_BIT : 0));
    // sceGuClear leaves the pipeline in an indeterminate state; re-send everything on the next draw.
    resetApplied();
}

void renderFinishGpu() {}
void renderSubmitFrame() {}

void renderClearColor(float r, float g, float b, float a)
{
    g_s.clearColor[0] = r; g_s.clearColor[1] = g; g_s.clearColor[2] = b; g_s.clearColor[3] = a;
}

void renderClearDepth(double depth) { g_s.clearDepth = static_cast<float>(depth); }

void renderPolygonOffset(float factor, float units)
{
    g_s.polyFactor = factor;
    g_s.polyUnits = units;
}

void renderLineWidth(float) {}

void renderViewport(int x, int y, int width, int height)
{
    g_s.viewport[0] = x; g_s.viewport[1] = y; g_s.viewport[2] = width; g_s.viewport[3] = height;
    g_a.viewportDirty = true;
}

void renderGetViewport(int *values)
{
    if (!values) return;
    for (int i = 0; i < 4; ++i) values[i] = g_s.viewport[i];
}

void renderGetMatrix(RenderMatrixQuery query, float *values)
{
    if (!values) return;
    const int slot = query == RenderMatrixQuery::Projection ? SlotProjection :
                     query == RenderMatrixQuery::Texture ? SlotTexture : SlotModelView;
    std::memcpy(values, g_stacks[slot].cur().m, sizeof(float) * 16);
}

const unsigned char *renderGetString(RenderStringQuery query)
{
    switch (query)
    {
    case RenderStringQuery::Vendor: return reinterpret_cast<const unsigned char *>("Sony");
    case RenderStringQuery::Renderer: return reinterpret_cast<const unsigned char *>("PSP Graphics Engine (sceGu)");
    case RenderStringQuery::Version: return reinterpret_cast<const unsigned char *>("OptiCraft PSP");
    case RenderStringQuery::Extensions: return reinterpret_cast<const unsigned char *>("");
    }
    return reinterpret_cast<const unsigned char *>("");
}

bool renderSupportsFeature(RenderFeature)
{
    return false; // no mipmaps yet, no occlusion queries, no AF, no MSAA, no fancy fog
}

unsigned int renderGetError() { return 0; }

// ------------------------------------------------------------------------ matrices ---

void renderMatrixMode(RenderMatrixMode mode)
{
    if (recording()) { Cmd c{Op::MatrixMode}; c.i0 = static_cast<int>(mode); rec(c); return; }
    g_matrixMode = mode == RenderMatrixMode::Projection ? SlotProjection :
                   mode == RenderMatrixMode::Texture ? SlotTexture : SlotModelView;
}

void renderLoadIdentity()
{
    if (recording()) { rec(Cmd{Op::LoadIdentity}); return; }
    activeStack().cur() = PspMath::identity();
    markMatrixDirty();
}

void renderPushMatrix()
{
    if (recording()) { rec(Cmd{Op::Push}); return; }
    MatrixStack &s = activeStack();
    if (s.top + 1 < kStackDepth)
    {
        s.m[s.top + 1] = s.m[s.top];
        ++s.top;
    }
}

void renderPopMatrix()
{
    if (recording()) { rec(Cmd{Op::Pop}); return; }
    MatrixStack &s = activeStack();
    if (s.top > 0) --s.top;
    markMatrixDirty();
}

void renderTranslate(float x, float y, float z)
{
    if (recording()) { Cmd c{Op::Translate}; c.f0 = x; c.f1 = y; c.f2 = z; rec(c); return; }
    Mat4 &m = activeStack().cur();
    m = PspMath::multiply(m, PspMath::translation(x, y, z));
    markMatrixDirty();
}

void renderRotate(float angle, float x, float y, float z)
{
    if (recording()) { Cmd c{Op::Rotate}; c.f0 = angle; c.f1 = x; c.f2 = y; c.f3 = z; rec(c); return; }
    Mat4 &m = activeStack().cur();
    m = PspMath::multiply(m, PspMath::rotation(angle, x, y, z));
    markMatrixDirty();
}

void renderScale(float x, float y, float z)
{
    if (recording()) { Cmd c{Op::Scale}; c.f0 = x; c.f1 = y; c.f2 = z; rec(c); return; }
    Mat4 &m = activeStack().cur();
    m = PspMath::multiply(m, PspMath::scaling(x, y, z));
    markMatrixDirty();
}

void renderScaleDouble(double x, double y, double z)
{
    renderScale(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void renderFrustum(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    Mat4 &m = activeStack().cur();
    m = PspMath::multiply(m, PspMath::frustum(left, right, bottom, top, nearValue, farValue));
    markMatrixDirty();
}

void renderOrtho(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    Mat4 &m = activeStack().cur();
    m = PspMath::multiply(m, PspMath::ortho(left, right, bottom, top, nearValue, farValue));
    markMatrixDirty();
}

// -------------------------------------------------------------------- display lists ---

int renderGenerateDisplayLists(int count)
{
    const int first = static_cast<int>(g_lists.size());
    g_lists.resize(g_lists.size() + static_cast<std::size_t>(count));
    return first;
}

void renderDeleteDisplayLists(int first, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const std::size_t id = static_cast<std::size_t>(first + i);
        if (id > 0 && id < g_lists.size()) listClear(g_lists[id]);
    }
}

void renderBeginDisplayList(int list)
{
    if (list <= 0 || list >= static_cast<int>(g_lists.size())) return;
    listClear(g_lists[static_cast<std::size_t>(list)]);
    g_lists[static_cast<std::size_t>(list)].defined = true;
    g_recording = list;
}

void renderEndDisplayList() { g_recording = 0; }

void renderCallDisplayList(int list)
{
    if (list <= 0 || list >= static_cast<int>(g_lists.size())) return;
    const DisplayList &l = g_lists[static_cast<std::size_t>(list)];
    if (recording())
    {
        // Nested call: replay now so its effect is part of this frame, not recorded.
    }
    replay(l);
}

void renderCallDisplayLists(int count, const int *lists)
{
    for (int i = 0; i < count; ++i) renderCallDisplayList(lists[i]);
}

namespace
{
void replay(const DisplayList &list)
{
    const int savedRecording = g_recording;
    g_recording = 0;
    for (const Cmd &c : list.cmds)
    {
        switch (c.op)
        {
        case Op::Draw: if (c.geom) submit(*c.geom); break;
        case Op::MatrixMode: renderMatrixMode(static_cast<RenderMatrixMode>(c.i0)); break;
        case Op::LoadIdentity: renderLoadIdentity(); break;
        case Op::Push: renderPushMatrix(); break;
        case Op::Pop: renderPopMatrix(); break;
        case Op::Translate: renderTranslate(c.f0, c.f1, c.f2); break;
        case Op::Rotate: renderRotate(c.f0, c.f1, c.f2, c.f3); break;
        case Op::Scale: renderScale(c.f0, c.f1, c.f2); break;
        case Op::Color4f: renderColor4f(c.f0, c.f1, c.f2, c.f3); break;
        case Op::Normal3f: break;
        case Op::Bind: renderBindTexture(c.i0); break;
        case Op::Enable: renderEnable(static_cast<RenderCapability>(c.i0)); break;
        case Op::Disable: renderDisable(static_cast<RenderCapability>(c.i0)); break;
        case Op::BlendFunc: renderBlendFunc(static_cast<RenderBlendFactor>(c.i0), static_cast<RenderBlendFactor>(c.i1)); break;
        case Op::AlphaFunc: renderAlphaFunc(static_cast<RenderCompare>(c.i0), c.f0); break;
        case Op::DepthMask: renderDepthMask(c.i0 != 0); break;
        case Op::DepthFunc: renderDepthFunc(static_cast<RenderCompare>(c.i0)); break;
        case Op::CullFace: renderCullFace(static_cast<RenderFace>(c.i0)); break;
        case Op::LightmapCoord:
            g_s.lightmapU = static_cast<int>(c.f0);
            g_s.lightmapV = static_cast<int>(c.f1);
            break;
        case Op::ColorMask: renderColorMask(c.i0 & 1, c.i0 & 2, c.i0 & 4, c.i0 & 8); break;
        case Op::ShadeModel: break;
        }
    }
    g_recording = savedRecording;
}
} // namespace

// ----------------------------------------------------------------- occlusion queries ---

void renderGenerateOcclusionQueries(int count, int *queries)
{
    for (int i = 0; i < count; ++i) queries[i] = 0;
}
void renderBeginOcclusionQuery(int) {}
void renderEndOcclusionQuery() {}
bool renderOcclusionQueryResultAvailable(int) { return true; }
unsigned int renderOcclusionQueryResult(int) { return 1; }

// ------------------------------------------------------------------------- drawing ---

bool renderDrawInterleaved(const RenderInterleavedMesh &mesh)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0) return false;
    if (recording())
    {
        PackedGeom *g = packPersistent(mesh);
        if (!g) return false;
        Cmd c{Op::Draw};
        c.geom = g;
        rec(c);
        return true;
    }
    return drawImmediate(mesh);
}

bool renderCaptureInterleaved(const RenderInterleavedMesh &mesh, RenderCapturedMesh &out, bool append)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
        return false;
    if (!append)
        out.clear();
    if (!out.empty() && (out.stride != mesh.stride || out.primitive != mesh.primitive ||
        out.positionShort != mesh.positionShort ||
        out.hasTexture != mesh.hasTexture || (mesh.hasTexture && out.texCoordOffset != mesh.texCoordOffset) ||
        out.hasColor != mesh.hasColor || (mesh.hasColor && out.colorOffset != mesh.colorOffset) ||
        out.hasNormals != mesh.hasNormals || (mesh.hasNormals && out.normalOffset != mesh.normalOffset) ||
        out.hasBrightness != mesh.hasBrightness || (mesh.hasBrightness && out.brightnessOffset != mesh.brightnessOffset)))
        return false;
    if (out.empty())
    {
        out.stride = mesh.stride; out.primitive = mesh.primitive; out.positionShort = mesh.positionShort;
        out.hasTexture = mesh.hasTexture; out.texCoordOffset = mesh.texCoordOffset;
        out.hasColor = mesh.hasColor; out.colorOffset = mesh.colorOffset;
        out.hasNormals = mesh.hasNormals; out.normalOffset = mesh.normalOffset;
        out.hasBrightness = mesh.hasBrightness; out.brightnessOffset = mesh.brightnessOffset;
    }
    const unsigned char *src = static_cast<const unsigned char *>(mesh.data) + static_cast<std::size_t>(mesh.first) * mesh.stride;
    const std::size_t bytes = static_cast<std::size_t>(mesh.count) * mesh.stride;
    const std::size_t old = out.raw.size();
    out.raw.resize(old + (bytes + 3u) / 4u);
    std::memcpy(reinterpret_cast<unsigned char *>(out.raw.data()) + old * 4u, src, bytes);
    out.vertexCount += mesh.count;
    return true;
}

bool renderDrawCaptured(const RenderCapturedMesh &mesh)
{
    if (mesh.empty()) return false;
    RenderInterleavedMesh view;
    view.data = mesh.raw.data(); view.stride = mesh.stride; view.count = mesh.vertexCount;
    view.primitive = mesh.primitive; view.positionShort = mesh.positionShort;
    view.hasTexture = mesh.hasTexture; view.texCoordOffset = mesh.texCoordOffset;
    view.hasColor = mesh.hasColor; view.colorOffset = mesh.colorOffset;
    view.hasNormals = mesh.hasNormals; view.normalOffset = mesh.normalOffset;
    view.hasBrightness = mesh.hasBrightness; view.brightnessOffset = mesh.brightnessOffset;
    return renderDrawInterleaved(view);
}

#if PLATFORM_PERSISTENT_RENDER_MESH || PLATFORM_MODEL_PERSISTENT_MESH
int renderCreatePersistentMesh()
{
    if (!g_persistentFree.empty())
    {
        const int h = g_persistentFree.back();
        g_persistentFree.pop_back();
        return h;
    }
    g_persistent.push_back(nullptr);
    return static_cast<int>(g_persistent.size()) - 1;
}

void renderDestroyPersistentMesh(int handle)
{
    if (handle <= 0 || handle >= static_cast<int>(g_persistent.size())) return;
    geomRelease(g_persistent[static_cast<std::size_t>(handle)]);
    g_persistent[static_cast<std::size_t>(handle)] = nullptr;
    g_persistentFree.push_back(handle);
}

bool renderCompilePersistentMesh(int handle, const RenderInterleavedMesh &mesh)
{
    if (handle <= 0 || handle >= static_cast<int>(g_persistent.size())) return false;
    PackedGeom *g = packPersistent(mesh);
    if (!g) return false;
    geomRelease(g_persistent[static_cast<std::size_t>(handle)]);
    g_persistent[static_cast<std::size_t>(handle)] = g;
    return true;
}

bool renderDrawPersistentMesh(int handle)
{
    if (handle <= 0 || handle >= static_cast<int>(g_persistent.size())) return false;
    PackedGeom *g = g_persistent[static_cast<std::size_t>(handle)];
    if (!g) return false;
    if (recording())
    {
        ++g->refs;
        Cmd c{Op::Draw};
        c.geom = g;
        rec(c);
        return true;
    }
    submit(*g);
    return true;
}
#endif

#if PLATFORM_FRAMEBUFFER_READBACK
bool renderReadPixelsRgb(int, int, int, int, void *) { return false; }
#endif

bool renderCopyFramebufferToBoundTexture(int, int, int, int) { return false; }

void renderSetLegacyPresentationGamma(bool) {}

// Lets code that talks to sceGu directly (the debug heartbeat) force a full state re-send.
void pspRenderInvalidateState() { resetApplied(); }

void pspTraceBoundTexture(const char *tag)
{
    Texture *t = boundTex();
    if (!t) { PspSystem::bootTrace("%s: no texture bound (id %d)", tag, g_boundTexture); return; }
    PspSystem::bootTrace("%s: tex id=%d %dx%d store=%dx%d clamp=%d blur=%d created=%d px0=%08x px1=%08x",
                         tag, g_boundTexture, t->w, t->h, t->tw, t->th, t->clamp, t->blur, t->created,
                         t->pixels ? t->pixels[0] : 0u, t->pixels ? t->pixels[t->tw * (t->h / 2) + t->w / 2] : 0u);
}
