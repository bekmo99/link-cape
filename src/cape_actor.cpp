#include "cape_actor.hpp"
#include "body_data.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "m_Do/m_Do_mtx.h"

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DShape.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lnkcape {

Tuning gTuning;
void (*gLogWarn)(const char* msg) = nullptr;

s16 Cape_c::sProcName = -1;
ActorHandle Cape_c::sActorHandle = -1;
Cape_c* Cape_c::sInstance = nullptr;

// ---------------------------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------------------------

static inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 operator*(const V3& a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
static inline f32 dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline f32 length(const V3& a) { return std::sqrt(dot(a, a)); }
static inline V3 normalized(const V3& a, const V3& fallback) {
    const f32 l = length(a);
    return l > 1e-4f ? a * (1.0f / l) : fallback;
}

// ---------------------------------------------------------------------------------------------
// Drawing
//
// The cape is drawn with immediate-mode GX from world-space positions that Execute() keeps up to
// date. The GX state set-up is the same as the game's own cloak actor (d_a_mant) uses: one TEV
// stage that multiplies the texture by the scene lighting.
// ---------------------------------------------------------------------------------------------

// The cape's texture. The game side only ever sees this small placeholder (a flat red-brown).
// The real artwork is the texture replacement shipped in textures/ (`tex1_80x144_$_5.png`), which
// the renderer substitutes for it. If texture replacements are disabled, the flat placeholder
// colour is what you get.
constexpr int kTexW = 80;
constexpr int kTexH = 144;
alignas(32) static u8 sTexData[kTexW * kTexH * 2];
static GXTexObj sTexObj;
static bool sTexInit = false;

static void initTexture() {
    if (sTexInit) {
        return;
    }
    // RGB5A3, opaque (top bit set): 5-5-5 colour. Big-endian 16-bit texels.
    const u16 texel = 0x8000 | (24 << 10) | (6 << 5) | 6;
    for (int i = 0; i < kTexW * kTexH; i++) {
        sTexData[i * 2 + 0] = static_cast<u8>(texel >> 8);
        sTexData[i * 2 + 1] = static_cast<u8>(texel & 0xFF);
    }
    GXInitTexObj(&sTexObj, sTexData, kTexW, kTexH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&sTexObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE,
        GX_ANISO_1);
    sTexInit = true;
}

static void submitCloth(const Packet_c& pk, bool inside) {
    const f32 ns = inside ? -1.0f : 1.0f;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, (kRefRows - 1) * (kRefCols - 1) * 6);
    for (int r = 0; r < kRefRows - 1; r++) {
        for (int c = 0; c < kRefCols - 1; c++) {
            const int i00 = r * kRefCols + c;
            const int i01 = i00 + 1;
            const int i10 = i00 + kRefCols;
            const int i11 = i10 + 1;
            const int order[6] = {i00, i10, i11, i00, i11, i01};
            const int flipped[6] = {i00, i11, i10, i00, i01, i11};
            const int* idx = pk.mFlip ? flipped : order;
            for (int k = 0; k < 6; k++) {
                const int i = idx[k];
                const V3& p = pk.mRefPos[i];
                const V3& n = pk.mRefNrm[i];
                GXPosition3f32(p.x, p.y, p.z);
                GXNormal3f32(n.x * ns, n.y * ns, n.z * ns);
                GXTexCoord2f32(static_cast<f32>(i % kRefCols) / (kRefCols - 1),
                    static_cast<f32>(i / kRefCols) / (kRefRows - 1));
            }
        }
    }
    GXEnd();
}

// Catmull-Rom spline through p1..p2 (t in 0..1), p0 and p3 being the neighbours.
static V3 catmullRom(const V3& p0, const V3& p1, const V3& p2, const V3& p3, f32 t) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
               (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

// Builds the smooth, subdivided surface (and its normals) from the simulated grid.
static void refineCloth(Packet_c& pk) {
    static V3 tmp[kRows * kRefCols];
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols - 1; c++) {
            const V3& p0 = pk.mDrawPos[r * kCols + (c > 0 ? c - 1 : 0)];
            const V3& p1 = pk.mDrawPos[r * kCols + c];
            const V3& p2 = pk.mDrawPos[r * kCols + c + 1];
            const V3& p3 = pk.mDrawPos[r * kCols + (c + 2 < kCols ? c + 2 : kCols - 1)];
            for (int sub = 0; sub < kSub; sub++) {
                tmp[r * kRefCols + c * kSub + sub] =
                    catmullRom(p0, p1, p2, p3, static_cast<f32>(sub) / kSub);
            }
        }
        tmp[r * kRefCols + kRefCols - 1] = pk.mDrawPos[r * kCols + kCols - 1];
    }
    for (int c = 0; c < kRefCols; c++) {
        for (int r = 0; r < kRows - 1; r++) {
            const V3& p0 = tmp[(r > 0 ? r - 1 : 0) * kRefCols + c];
            const V3& p1 = tmp[r * kRefCols + c];
            const V3& p2 = tmp[(r + 1) * kRefCols + c];
            const V3& p3 = tmp[(r + 2 < kRows ? r + 2 : kRows - 1) * kRefCols + c];
            for (int sub = 0; sub < kSub; sub++) {
                pk.mRefPos[(r * kSub + sub) * kRefCols + c] =
                    catmullRom(p0, p1, p2, p3, static_cast<f32>(sub) / kSub);
            }
        }
        pk.mRefPos[(kRefRows - 1) * kRefCols + c] = tmp[(kRows - 1) * kRefCols + c];
    }
    for (int r = 0; r < kRefRows; r++) {
        for (int c = 0; c < kRefCols; c++) {
            const int r0 = r > 0 ? r - 1 : r;
            const int r1 = r < kRefRows - 1 ? r + 1 : r;
            const int c0 = c > 0 ? c - 1 : c;
            const int c1 = c < kRefCols - 1 ? c + 1 : c;
            const V3 dx = pk.mRefPos[r * kRefCols + c1] - pk.mRefPos[r * kRefCols + c0];
            const V3 dy = pk.mRefPos[r1 * kRefCols + c] - pk.mRefPos[r0 * kRefCols + c];
            pk.mRefNrm[r * kRefCols + c] = normalized(cross(dy, dx), V3{0.0f, 0.0f, -1.0f});
        }
    }
}

void Packet_c::draw() {
    if (!mVisible || mTevStr == nullptr) {
        return;
    }
    initTexture();

    // Interpolate between the last two ticks when the game draws more often than it simulates;
    // when it draws once per tick, just show the newest state.
    mDrawsThisTick++;
    f32 alpha = 1.0f;
    if (mInterpOn && !mSnap) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double a = (now - mTickTime) / mTickInterval;
        a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
        alpha = static_cast<f32>(a);
    }
    for (int i = 0; i < kCount; i++) {
        mDrawPos[i] = mPosPrev[i] + (mPos[i] - mPosPrev[i]) * alpha;
        mDrawNrm[i] = normalized(mNrmPrev[i] + (mNrm[i] - mNrmPrev[i]) * alpha, mNrm[i]);
    }
    refineCloth(*this);

    j3dSys.reinitGX();
    GXSetNumIndStages(0);
    dKy_setLight_again();
    dKy_GxFog_tevstr_set(mTevStr);
    GXClearVtxDesc();

    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXSetZCompLoc(0);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, 0xff, GX_DF_CLAMP, GX_AF_SPOT);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);

    dKy_Global_amb_set(mTevStr);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

    GXSetTevColor(GX_TEVREG0, GXColor{1, 0, 0, 0});
    GXSetTevKColor(GX_KCOLOR0, GXColor{1, 0, 0, 0});
    GXSetTevKColor(GX_KCOLOR3, GXColor{255, 255, 255, 255});

    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_KONST, GX_CC_TEXC, GX_CC_RASC, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_KONST, GX_CA_TEXA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K3_A);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_GREATER, 0);

    GXLoadTexObj(&sTexObj, GX_TEXMAP0);

    // Positions are world space; the view matrix takes them to eye space.
    Mtx nrmMtx;
    GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
    cMtx_inverseTranspose(j3dSys.getViewMtx(), nrmMtx);
    GXLoadNrmMtxImm(nrmMtx, GX_PNMTX0);

    // Outside of the cape
    GXSetCullMode(GX_CULL_BACK);
    submitCloth(*this, false);

    // Inside of the cape: same triangles, opposite facing, flipped normals
    GXSetCullMode(GX_CULL_FRONT);
    submitCloth(*this, true);

    J3DShape::resetVcdVatCache();
}

// ---------------------------------------------------------------------------------------------
// Simulation
//
// The cape is built around the "Hylian armor" Link model. Its back surface was measured offline
// (tools/gen_body_data.py -> body_data.hpp):
//  * Rows 0..kRigid (collar to waist) are rigid. Each point is stored relative to Link's chest and
//    waist and simply carried along by them, so the top of the cape can never pull away from him.
//  * Below the waist the cloth hangs and flows. Link's back, tunic flap, legs and boots are
//    ~180 small spheres glued to his skeleton, so the cloth can't pass through them.
//
// Every joint frame is rebuilt each tick from the joint *positions* (the same way it was when the
// data was made), so none of this depends on how the game orients its joints internally.
// ---------------------------------------------------------------------------------------------

constexpr int kRigid = 5;  // rows 0..kRigid are rigid (neck to waist)
constexpr int kIterations = 14;
static_assert(kRigid + 1 == 6, "body_data.hpp was generated for 6 rigid rows");
static_assert(kBodySphereCount == 180, "update mSphereSm in cape_actor.hpp");
constexpr f32 kMaxSpeed = 30.0f;    // per-tick velocity clamp, keeps contact from launching the cloth
constexpr f32 kDrag = 0.12f;        // fraction of velocity lost per tick
constexpr f32 kGravity = 0.9f;      // units per tick^2
constexpr f32 kRunStream = 0.007f;  // extra backward push per unit of Link's forward speed
constexpr f32 kWiden = 0.20f;       // how much wider the hem is than the hips
constexpr f32 kMaxSkinDrift = 9.0f;  // how far the skinned pinned points may stray from the simple estimate
constexpr f32 kSkin = 1.0f;         // the cloth rides this far outside the body

static void solveDistance(V3& a, V3& b, f32 rest, bool pinA, bool pinB) {
    if (pinA && pinB) {
        return;
    }
    const V3 delta = b - a;
    const f32 d = length(delta);
    if (d < 1e-5f) {
        return;
    }
    const f32 diff = (d - rest) / d;
    if (pinA) {
        b = b - delta * diff;
    } else if (pinB) {
        a = a + delta * diff;
    } else {
        a = a + delta * (diff * 0.5f);
        b = b - delta * (diff * 0.5f);
    }
}

static V3 jointPos(daAlink_c* link, int j) {
    MtxP m = link->mpLinkModel->getAnmMtx(j);
    return {m[0][3], m[1][3], m[2][3]};
}

struct Frame {
    V3 x, y, z;
};

constexpr int kNumJoints = 35;
constexpr int kNumGroups = 9;

// Same construction as tools/gen_body_data.py: Y from two joints, X from the shoulder / hip line.
static Frame makeFrame(const V3& yRaw, const V3& side, f32 handed) {
    Frame f;
    f.y = normalized(yRaw, V3{0.0f, 1.0f, 0.0f});
    f.x = normalized(side - f.y * dot(side, f.y), V3{1.0f, 0.0f, 0.0f});
    f.z = cross(f.x, f.y) * handed;
    return f;
}

static void buildFrames(const V3* P, f32 handed, Frame* fr) {
    const V3 hip = normalized(P[17] - P[22], V3{1.0f, 0.0f, 0.0f});
    fr[0] = makeFrame(P[3] - P[2], hip, handed);       // chest
    fr[1] = makeFrame(P[2] - P[1], hip, handed);       // spine
    fr[2] = makeFrame(P[2] - P[16], hip, handed);      // waist
    fr[3] = makeFrame(P[18] - P[19], hip, handed);     // left thigh
    fr[4] = makeFrame(P[23] - P[24], hip, handed);     // right thigh
    fr[5] = makeFrame(P[19] - P[20], hip, handed);     // left shin / boot
    fr[6] = makeFrame(P[24] - P[25], hip, handed);     // right shin / boot
    fr[7] = makeFrame(P[31] - P[32], hip, handed);     // left rear skirt flap
    fr[8] = makeFrame(P[33] - P[34], hip, handed);     // right rear skirt flap
}

static V3 fromLocal(const V3& origin, const Frame& f, const float* l) {
    return origin + f.x * l[0] + f.y * l[1] + f.z * l[2];
}

struct Sphere {
    V3 c;
    f32 r;
};

// Pushes p out of the sphere by moving it BACKWARD (along `back`, a unit vector) until it clears it,
// rather than out through the nearest surface: the cape always hangs behind Link, and a point that
// starts inside his thick tunic flap would otherwise be shoved the short way, into his body.
// Returns true if it moved.
static bool collideSphere(V3& p, const Sphere& s, const V3& back) {
    const V3 d = p - s.c;
    const f32 dist2 = dot(d, d);
    if (dist2 >= s.r * s.r) {
        return false;
    }
    const f32 b = dot(d, back);
    const f32 disc = b * b - (dist2 - s.r * s.r);
    const f32 t = -b + std::sqrt(disc > 0.0f ? disc : 0.0f);
    p = p + back * t;
    return true;
}

// Oriented boxes for the gear on Link's back (sheath, sword, shield): the joint bounding boxes of
// those models, placed with their live joint matrices.
struct Box {
    V3 origin;
    V3 axis[3];   // unit axes in world space
    f32 scale[3]; // world length of one local unit along each axis
    V3 mn, mx;    // local-space bounds
    f32 r;
};

constexpr int kMaxBoxes = 12;

static void addModelBoxes(J3DModel* model, f32 pad, Box* out, int& n) {
    if (model == nullptr || model->getModelData() == nullptr) {
        return;
    }
    J3DModelData* data = model->getModelData();
    const int joints = data->getJointNum();
    for (int j = 0; j < joints && j < 4 && n < kMaxBoxes; j++) {
        J3DJoint* joint = data->getJointNodePointer(j);
        if (joint == nullptr) {
            continue;
        }
        MtxP m = model->getAnmMtx(j);
        Box b;
        b.origin = {m[0][3], m[1][3], m[2][3]};
        for (int k = 0; k < 3; k++) {
            const V3 col = {m[0][k], m[1][k], m[2][k]};
            b.scale[k] = length(col);
            b.axis[k] = normalized(col, V3{k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f});
        }
        const Vec* mn = joint->getMin();
        const Vec* mx = joint->getMax();
        b.mn = {mn->x, mn->y, mn->z};
        b.mx = {mx->x, mx->y, mx->z};
        // Joints with no geometry (or an unset box) get a small default box instead.
        if (mx->x - mn->x < 1.0f && mx->y - mn->y < 1.0f && mx->z - mn->z < 1.0f) {
            b.mn = {-6.0f, -6.0f, -6.0f};
            b.mx = {6.0f, 6.0f, 6.0f};
        }
        b.r = pad;
        out[n++] = b;
    }
}

// Gear on Link's back goes OVER the cape: a cloth point inside the box is pushed out through the
// face that looks toward Link's body, so the cloth ends up between his back and the gear.
static bool collideBoxInward(V3& p, const Box& b, const V3& back, V3& nOut) {
    const V3 rel = p - b.origin;
    f32 l[3];
    for (int k = 0; k < 3; k++) {
        const f32 s = b.scale[k] > 1e-4f ? b.scale[k] : 1.0f;
        l[k] = dot(rel, b.axis[k]) / s;
    }
    const f32 lo[3] = {b.mn.x, b.mn.y, b.mn.z};
    const f32 hi[3] = {b.mx.x, b.mx.y, b.mx.z};
    for (int k = 0; k < 3; k++) {
        if (l[k] <= lo[k] || l[k] >= hi[k]) {
            return false;
        }
    }
    int bestK = 0;
    f32 bestDir = 1.0f;
    f32 bestDot = 1e30f;
    for (int k = 0; k < 3; k++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const f32 d = dot(b.axis[k] * static_cast<f32>(sgn), back);
            if (d < bestDot) {
                bestDot = d;
                bestK = k;
                bestDir = static_cast<f32>(sgn);
            }
        }
    }
    const f32 s = b.scale[bestK] > 1e-4f ? b.scale[bestK] : 1.0f;
    const f32 dist = (bestDir > 0.0f ? hi[bestK] - l[bestK] : l[bestK] - lo[bestK]) * s;
    nOut = b.axis[bestK] * bestDir;
    p = p + nOut * (dist + 0.5f);
    return true;
}

Cape_c::~Cape_c() {}

void Cape_c::resetSim(const V3* rest) {
    for (int i = 0; i < kCount; i++) {
        mP[i] = rest[i];
        mPrev[i] = rest[i];
    }
    mSimInit = true;
}

cPhs_Step Cape_c::create() {
    // The game never runs actor constructors for us; do it by hand (see the custom_actor_demo).
    fopAcM_ct(this, Cape_c);

    sInstance = this;
    mSimInit = false;
    mLastLinkPos = {0.0f, 0.0f, 0.0f};
    mPacket.mVisible = false;
    current.pos = home.pos;  // overwritten every tick once we have a parent
    scale.set(1.0f, 1.0f, 1.0f);
    return cPhs_COMPLEATE_e;
}

int Cape_c::Delete() {
    if (sInstance == this) {
        sInstance = nullptr;
    }
    this->~Cape_c();
    return 1;
}

int Cape_c::Execute() {
    // The cape lives and dies with Link (stage changes, game over, ...).
    fopAc_ac_c* parent = fopAcM_SearchByID(parentActorID);
    if (parent == nullptr || fopAcM_GetName(parent) != fpcNm_ALINK_e) {
        fopAcM_delete(this);
        return 1;
    }
    daAlink_c* link = static_cast<daAlink_c*>(parent);
    // The cape is built for one specific model, so make sure we really are looking at its skeleton.
    if (link->mpLinkModel == nullptr || link->mpLinkModel->getModelData() == nullptr ||
        link->mpLinkModel->getModelData()->getJointNum() < kNumJoints)
    {
        mPacket.mVisible = false;
        return 1;
    }

    const bool heroClothes = dComIfGs_getSelectEquipClothes() == dItemNo_WEAR_KOKIRI_e;
    mPacket.mVisible = gTuning.enabled && !link->checkWolf() &&
                       (!gTuning.heroOnly || heroClothes);
    current.pos = link->current.pos;  // used for the lighting lookup

    // --- Link's pose -------------------------------------------------------------------------
    const f32 angle = link->shape_angle.y * (3.14159265f / 32768.0f);
    const V3 forward = {std::sin(angle), 0.0f, std::cos(angle)};
    const V3 back = forward * -1.0f;
    const V3 up = {0.0f, 1.0f, 0.0f};

    V3 P[kNumJoints];
    for (int j = 0; j < kNumJoints; j++) {
        P[j] = jointPos(link, j);
    }
    // Make sure "forward" in the frames really is forward in the game's world.
    f32 handed = 1.0f;
    {
        const V3 chestSide = normalized(P[17] - P[22], V3{1.0f, 0.0f, 0.0f});
        const V3 chestUp = normalized(P[3] - P[2], up);
        const V3 chestX = normalized(chestSide - chestUp * dot(chestSide, chestUp), chestSide);
        if (dot(cross(chestX, chestUp), forward) < 0.0f) {
            handed = -1.0f;
        }
    }
    Frame fr[kNumGroups];
    buildFrames(P, handed, fr);

    // Spine, chest and waist take their orientation straight from the game's joint matrices, so the
    // pinned part of the cape follows their twist too. Which matrix column is which axis is not
    // documented, so it is worked out once, while he is standing upright and still, and only if the
    // skeleton looks like the real thing (right after spawning the matrices can still be empty, and
    // calibrating then gave a cape stuck sideways). Every tick the result is checked against the
    // joint positions; if it disagrees for a while it is thrown away and measured again.
    {
        const V3 hipSide = normalized(P[17] - P[22], V3{1.0f, 0.0f, 0.0f});
        const int joints[3] = {1, 2, 16};
        const V3 refUp[3] = {P[2] - P[1], P[3] - P[2], P[2] - P[16]};
        const int groups[3] = {1, 0, 2};
        const f32 lenNeck = length(P[3] - P[2]);
        const f32 lenSpine = length(P[2] - P[16]);
        const f32 lenHips = length(P[17] - P[22]);
        const bool plausible = lenNeck > 14.0f && lenNeck < 26.0f && lenSpine > 15.0f &&
                               lenSpine < 28.0f && lenHips > 8.0f && lenHips < 18.0f &&
                               P[2].y > P[16].y;
        const bool upright = plausible && dot(normalized(P[3] - P[2], up), up) > 0.9f &&
                             dot(normalized(P[2] - P[16], up), up) > 0.85f && link->speedF < 0.5f;
        for (int a = 0; a < 3; a++) {
            MtxP m = link->mpLinkModel->getAnmMtx(joints[a]);
            V3 col[3];
            for (int k = 0; k < 3; k++) {
                col[k] = normalized(V3{m[0][k], m[1][k], m[2][k]}, V3{0.0f, 1.0f, 0.0f});
            }
            const V3 ru = normalized(refUp[a], up);
            if (!mAxValid[a] && upright) {
                int iu = 0;
                for (int k = 1; k < 3; k++) {
                    if (std::fabs(dot(col[k], ru)) > std::fabs(dot(col[iu], ru))) {
                        iu = k;
                    }
                }
                int is = iu == 0 ? 1 : 0;
                for (int k = 0; k < 3; k++) {
                    if (k != iu && std::fabs(dot(col[k], hipSide)) > std::fabs(dot(col[is], hipSide))) {
                        is = k;
                    }
                }
                const int ifw = 3 - iu - is;
                if (std::fabs(dot(col[iu], ru)) > 0.8f && std::fabs(dot(col[is], hipSide)) > 0.8f &&
                    std::fabs(dot(col[ifw], forward)) > 0.7f)
                {
                    mAxUp[a] = iu;
                    mAxSide[a] = is;
                    mAxUpSign[a] = dot(col[iu], ru) < 0.0f ? -1.0f : 1.0f;
                    mAxSideSign[a] = dot(col[is], hipSide) < 0.0f ? -1.0f : 1.0f;
                    mAxValid[a] = true;
                    mAxBad[a] = 0;
                }
            }
            if (mAxValid[a]) {
                Frame f;
                f.y = col[mAxUp[a]] * mAxUpSign[a];
                f.x = col[mAxSide[a]] * mAxSideSign[a];
                f.z = cross(f.x, f.y) * handed;
                if (dot(f.y, ru) > 0.5f && dot(f.x, hipSide) > 0.2f) {
                    fr[groups[a]] = f;
                    mAxBad[a] = 0;
                } else if (++mAxBad[a] > 20) {
                    mAxValid[a] = false;  // measured wrongly: start over
                }
            }
        }
    }

    // Body spheres in world space
    // The spheres are low-pass filtered over time: the tunic flap and legs swing quickly, and
    // letting the cloth chase every twitch made the hem warp.
    Sphere spheres[kBodySphereCount];
    for (int i = 0; i < kBodySphereCount; i++) {
        const BodySphere& b = kBodySpheres[i];
        const V3 target = fromLocal(P[b.joint], fr[b.group], b.l);
        if (!mSphereInit) {
            mSphereSm[i] = target;
        } else {
            mSphereSm[i] = mSphereSm[i] + (target - mSphereSm[i]) * 0.6f;
        }
        spheres[i].c = mSphereSm[i];
        spheres[i].r = b.r + kSkin;
    }
    mSphereInit = true;

    // Gear stowed on his back (shield, sheath, sword) goes OVER the cape.
    Box allBoxes[kMaxBoxes];
    int allCount = 0;
    if (link->checkShieldGet()) {
        addModelBoxes(link->mShieldModel, 0.0f, allBoxes, allCount);
    }
    if (link->checkSwordGet()) {
        addModelBoxes(link->mSheathModel, 0.0f, allBoxes, allCount);
        addModelBoxes(link->mSwordModel, 0.0f, allBoxes, allCount);
    }
    Box boxes[kMaxBoxes];
    int boxCount = 0;
    for (int i = 0; i < allCount; i++) {
        const Box& b = allBoxes[i];
        V3 mid = b.origin;
        for (int k = 0; k < 3; k++) {
            const f32 m = ((k == 0 ? b.mn.x + b.mx.x : (k == 1 ? b.mn.y + b.mx.y : b.mn.z + b.mx.z)) * 0.5f);
            mid = mid + b.axis[k] * (m * b.scale[k]);
        }
        const V3 off = mid - P[2];
        if (dot(off, back) > 2.0f && length(off) < 60.0f) {
            boxes[boxCount++] = b;
        }
    }

    auto resolveBody = [&](V3& p, int passes) {
        V3 n;
        for (int pass = 0; pass < passes; pass++) {
            for (int k = 0; k < boxCount; k++) {
                collideBoxInward(p, boxes[k], back, n);
            }
            for (int k = 0; k < kBodySphereCount; k++) {
                collideSphere(p, spheres[k], back);
            }
        }
    };

    // --- Rigid part: carried by the chest and waist ------------------------------------------
    V3 rest[kCount];
    {
        // A second, simple estimate of where each pinned point should be, that cannot go badly
        // wrong: along the spine's own line, then sideways / backward by his yaw. The skinned
        // position (which follows the real joint rotations) is trusted only within a few units of
        // it, so a mis-measured joint axis can never throw part of the cape out into a spike.
        const V3 side = {std::cos(angle), 0.0f, -std::sin(angle)};
        const V3 dirChest = normalized(P[3] - P[2], up);
        const V3 dirWaist = normalized(P[2] - P[16], up);
        f32 worst = 0.0f;
        for (int r = 0; r <= kRigid; r++) {
            for (int c = 0; c < kCols; c++) {
                const RigidPoint& rp = kRigidPoints[r * kCols + c];
                const f32 yb = rp.chest[1] + 124.0f;  // bind height (chest local = bind - chest origin)
                const V3 axisPoint = yb >= 124.0f ? P[2] + dirChest * (yb - 124.0f)
                                                  : P[16] + dirWaist * (yb - 103.0f);
                const V3 safe = axisPoint + side * rp.chest[0] + forward * rp.chest[2];
                // Linear blend skinning with the chest, spine and waist, like the body mesh itself.
                V3 skinned = fromLocal(P[2], fr[0], rp.chest) * rp.w[0] +
                             fromLocal(P[1], fr[1], rp.spine) * rp.w[1] +
                             fromLocal(P[16], fr[2], rp.waist) * rp.w[2];
                const V3 diff = skinned - safe;
                const f32 dl = length(diff);
                if (dl > worst) {
                    worst = dl;
                }
                if (dl > kMaxSkinDrift) {
                    skinned = safe + diff * (kMaxSkinDrift / dl);
                }
                rest[r * kCols + c] = skinned;
            }
        }
        if (worst > 14.0f && gLogWarn != nullptr && mLogCooldown <= 0) {
            char msg[200];
            std::snprintf(msg, sizeof(msg),
                "pinned cape point %.1f units from the spine estimate (handed=%.0f, axes valid %d%d%d, "
                "up %d%d%d, side %d%d%d); clamped",
                static_cast<double>(worst), static_cast<double>(handed), mAxValid[0], mAxValid[1],
                mAxValid[2], mAxUp[0], mAxUp[1], mAxUp[2], mAxSide[0], mAxSide[1], mAxSide[2]);
            gLogWarn(msg);
            mLogCooldown = 300;
        }
        if (mLogCooldown > 0) {
            mLogCooldown--;
        }
    }

    // --- Where the hanging part would rest ---------------------------------------------------
    // Straight down from the waist row, pushed out of his body where it is in the way. The
    // constraint lengths come from this shape, so the bumps of his tunic and boots don't stretch
    // the cloth, and the cloth is gently pulled toward it (see Stiffness).
    const f32 hipLen = 37.0f;  // neck-to-waist span of the rigid part
    const f32 vRestLow = (gTuning.length > hipLen + 20.0f ? gTuning.length - hipLen : 20.0f) /
                         (kRows - 1 - kRigid);
    const f32 groundY = link->current.pos.y + 2.0f;
    const V3 mid = rest[kRigid * kCols + kCols / 2];
    for (int r = kRigid + 1; r < kRows; r++) {
        const f32 k = static_cast<f32>(r - kRigid);
        const f32 widen = 1.0f + kWiden * k / (kRows - 1 - kRigid);
        // How far behind the waist row the cloth hangs at this height (it drapes over his tunic
        // flap, then falls straight down from the flap's lowest edge).
        f32 yb = (kHipY - vRestLow * k) / kEnvStep;
        yb = yb < 0.0f ? 0.0f : (yb > kEnvRows - 1.001f ? kEnvRows - 1.001f : yb);
        const int y0 = static_cast<int>(yb);
        const f32 fy = yb - static_cast<f32>(y0);
        for (int c = 0; c < kCols; c++) {
            const V3& h = rest[kRigid * kCols + c];
            const f32 depth = kEnvDepth[y0][c] * (1.0f - fy) + kEnvDepth[y0 + 1][c] * fy;
            V3 p = h + (h - mid) * (widen - 1.0f) + up * (-vRestLow * k) + back * depth;
            resolveBody(p, 2);
            if (p.y < groundY) {
                p.y = groundY;
            }
            rest[r * kCols + c] = p;
        }
    }

    // Teleports (stage change, warp, respawn): start over instead of stretching across the map.
    const V3 linkPos = {link->current.pos.x, link->current.pos.y, link->current.pos.z};
    if (!mSimInit || length(linkPos - mLastLinkPos) > 400.0f) {
        resetSim(rest);
        mPacket.mSnap = true;
        mSphereInit = false;
    }
    mLastLinkPos = linkPos;

    // --- Integrate the hanging part ----------------------------------------------------------
    const f32 streamTarget = link->speedF > 0.0f ? link->speedF * kRunStream : 0.0f;
    mStream += (streamTarget - mStream) * 0.2f;  // smoothed so starting / stopping doesn't snap it
    const f32 stream = mStream;
    const f32 spring = 0.003f * gTuning.stiffness;

    for (int i = 0; i < (kRigid + 1) * kCols; i++) {
        mP[i] = rest[i];
        mPrev[i] = rest[i];
    }
    for (int r = kRigid + 1; r < kRows; r++) {
        const f32 rowT = static_cast<f32>(r - kRigid) / (kRows - 1 - kRigid);
        for (int c = 0; c < kCols; c++) {
            const int i = r * kCols + c;
            V3& p = mP[i];
            V3& prev = mPrev[i];
            V3 vel = (p - prev) * (1.0f - kDrag);
            const f32 speed = length(vel);
            if (speed > kMaxSpeed) {
                vel = vel * (kMaxSpeed / speed);
            }
            prev = p;
            // The hem is looser than the top of the hanging part: it is pulled back to the resting
            // shape less and streams out more, so the bottom flows.
            const f32 loose = rowT * rowT;
            p = p + vel + V3{0.0f, -kGravity, 0.0f} + back * (stream * (0.3f + 1.2f * loose));
            p = p + (rest[i] - p) * (spring * (1.0f - 0.85f * loose));
        }
    }

    // --- Constraints and collision -----------------------------------------------------------
    for (int it = 0; it < kIterations; it++) {
        for (int i = 0; i < (kRigid + 1) * kCols; i++) {
            mP[i] = rest[i];
        }
        for (int r = 0; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                const int i = r * kCols + c;
                V3& p = mP[i];
                const bool pinned = r <= kRigid;
                if (r + 1 < kRows) {
                    const int j = i + kCols;
                    solveDistance(p, mP[j], length(rest[j] - rest[i]), pinned, false);
                }
                if (c + 1 < kCols) {
                    solveDistance(p, mP[i + 1], length(rest[i + 1] - rest[i]), pinned, pinned);
                }
                if (r + 1 < kRows && c + 1 < kCols) {
                    solveDistance(p, mP[i + kCols + 1], length(rest[i + kCols + 1] - rest[i]),
                        pinned, false);
                }
                if (r + 2 < kRows) {
                    solveDistance(p, mP[i + 2 * kCols], length(rest[i + 2 * kCols] - rest[i]),
                        pinned, false);
                }
            }
        }
        const f32 push = it >= kIterations - 3 ? 1.0f : 0.6f;  // soft at first, exact at the end
        for (int r = kRigid + 1; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                V3& p = mP[r * kCols + c];
                // Hanging cloth never rises above the row it hangs from: no floating up while he
                // runs or when the legs kick it.
                {
                    const f32 maxY = mP[(r - 1) * kCols + c].y - 0.35f * vRestLow;
                    if (p.y > maxY) {
                        p.y = maxY;
                    }
                }
                const V3 before = p;
                V3 hitN = {0.0f, 0.0f, 0.0f};
                bool hit = false;
                V3 n;
                for (int k = 0; k < boxCount; k++) {
                    if (collideBoxInward(p, boxes[k], back, n)) {
                        hit = true;
                        hitN = hitN + n;
                    }
                }
                const V3 preSphere = p;
                for (int k = 0; k < kBodySphereCount; k++) {
                    if (collideSphere(p, spheres[k], back)) {
                        hit = true;
                        hitN = hitN + back;
                    }
                }
                p = preSphere + (p - preSphere) * push;
                if (hit) {
                    // Remove the velocity going into the body and add a little friction.
                    V3& pv = mPrev[r * kCols + c];
                    V3 v = before - pv;
                    const V3 nn = normalized(hitN, V3{0.0f, 0.0f, -1.0f});
                    const f32 vn = dot(v, nn);
                    if (vn < 0.0f) {
                        v = v - nn * vn;
                    }
                    pv = p - v * 0.85f;
                }
                if (p.y < groundY) {
                    p.y = groundY;
                }
            }
        }
    }
    for (int i = 0; i < (kRigid + 1) * kCols; i++) {
        mP[i] = rest[i];
    }

    // --- Smooth out spikes ---------------------------------------------------------------------
    // A point shoved far out by a swinging flap or leg used to stick out as a sharp point. Relax
    // how far each point is from its resting spot toward its neighbours', then push the cloth back
    // out of the body once more so the smoothing can't sink it in.
    {
        V3 dev[kCount];
        for (int pass = 0; pass < 2; pass++) {
            for (int r = kRigid + 1; r < kRows; r++) {
                for (int c = 0; c < kCols; c++) {
                    const int i = r * kCols + c;
                    dev[i] = mP[i] - rest[i];
                }
            }
            for (int r = kRigid + 1; r < kRows; r++) {
                for (int c = 0; c < kCols; c++) {
                    const int i = r * kCols + c;
                    const V3& l = dev[r * kCols + (c > 0 ? c - 1 : c)];
                    const V3& rr = dev[r * kCols + (c < kCols - 1 ? c + 1 : c)];
                    const V3& u = dev[(r > kRigid + 1 ? r - 1 : r) * kCols + c];
                    const V3& d = dev[(r < kRows - 1 ? r + 1 : r) * kCols + c];
                    const V3 avg = (l + rr + u + d) * 0.25f;
                    mP[i] = rest[i] + dev[i] * 0.5f + avg * 0.5f;
                }
            }
        }
        for (int r = kRigid + 1; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                V3& p = mP[r * kCols + c];
                for (int k = 0; k < kBodySphereCount; k++) {
                    collideSphere(p, spheres[k], back);
                }
                if (p.y < groundY) {
                    p.y = groundY;
                }
            }
        }
    }

    // --- Keep the hanging cloth sane -----------------------------------------------------------
    // A leash on how far it may stray from its resting shape (tight near the waist, loose at the
    // hem), and a hard limit on stretching, so quick moves can't distort or stretch it.
    for (int r = kRigid + 1; r < kRows; r++) {
        const f32 rowT = static_cast<f32>(r - kRigid) / (kRows - 1 - kRigid);
        const f32 maxDev = 5.0f + 22.0f * rowT * rowT;
        for (int c = 0; c < kCols; c++) {
            const int i = r * kCols + c;
            V3& p = mP[i];
            const V3 d = p - rest[i];
            const f32 dl = length(d);
            if (dl > maxDev) {
                p = rest[i] + d * (maxDev / dl);
            }
            const V3& above = mP[i - kCols];
            const V3 v = p - above;
            const f32 maxLen = length(rest[i] - rest[i - kCols]) * 1.04f;
            const f32 vl = length(v);
            if (vl > maxLen) {
                p = above + v * (maxLen / vl);
            }
        }
    }

    // --- Normals and winding for drawing -----------------------------------------------------
    // Normal = dy x dx (down x right) as seen from outside; pick the sign so it faces away from
    // Link, and remember whether our triangle winding needs flipping to match.
    auto gridNormal = [&](int r, int c) {
        const int r0 = r > 0 ? r - 1 : r;
        const int r1 = r < kRows - 1 ? r + 1 : r;
        const int c0 = c > 0 ? c - 1 : c;
        const int c1 = c < kCols - 1 ? c + 1 : c;
        const V3 dx = mP[r * kCols + c1] - mP[r * kCols + c0];
        const V3 dy = mP[r1 * kCols + c] - mP[r0 * kCols + c];
        return cross(dy, dx);
    };
    // With the grid laid out left-to-right, top-to-bottom, dy x dx points straight away from
    // Link's back, so the outside / winding choice is fixed (deciding it from the live cloth made
    // it flicker whenever the cape swung edge-on).
    const f32 sign = 1.0f;
    mPacket.mFlip = gTuning.flipFaces;

    const double nowSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!mPacket.mSnap) {
        std::memcpy(mPacket.mPosPrev, mPacket.mPos, sizeof(mPacket.mPos));
        std::memcpy(mPacket.mNrmPrev, mPacket.mNrm, sizeof(mPacket.mNrm));
        const double dt = nowSec - mPacket.mTickTime;
        if (dt > 0.005 && dt < 0.2) {
            mPacket.mTickInterval = mPacket.mTickInterval * 0.7 + dt * 0.3;
        }
    }
    mPacket.mTickTime = nowSec;
    mPacket.mDrawsLastTick = mPacket.mDrawsThisTick;
    // Decide whether to blend between ticks from a smoothed draws-per-tick count, with hysteresis.
    // (Switching on a single tick's count made the cape flip between "ahead" and "behind" and bounce.)
    mPacket.mDrawEma = mPacket.mDrawEma * 0.8 + static_cast<double>(mPacket.mDrawsThisTick) * 0.2;
    if (!mPacket.mInterpOn && mPacket.mDrawEma > 1.7) {
        mPacket.mInterpOn = true;
    } else if (mPacket.mInterpOn && mPacket.mDrawEma < 1.25) {
        mPacket.mInterpOn = false;
    }
    mPacket.mDrawsThisTick = 0;

    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            const int i = r * kCols + c;
            // Light smoothing of what is drawn, for the hanging part only.
            const bool smooth = r > kRigid && !mPacket.mSnap;
            mPacket.mPos[i] = smooth ? mPacket.mPos[i] + (mP[i] - mPacket.mPos[i]) * 0.65f : mP[i];
            mPacket.mNrm[i] = normalized(gridNormal(r, c) * sign, back);
        }
    }
    if (mPacket.mSnap) {
        std::memcpy(mPacket.mPosPrev, mPacket.mPos, sizeof(mPacket.mPos));
        std::memcpy(mPacket.mNrmPrev, mPacket.mNrm, sizeof(mPacket.mNrm));
        mPacket.mSnap = false;
    }
    return 1;
}

int Cape_c::Draw() {
    if (!mPacket.mVisible) {
        return 1;
    }
    g_env_light.settingTevStruct(10, &current.pos, &tevStr);  // 10 = human Link lighting
    mPacket.mTevStr = &tevStr;
    j3dSys.getDrawBuffer(0)->entryImm(&mPacket, 0);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Actor profile
// ---------------------------------------------------------------------------------------------

static int capeCreate(void* i_this) { return static_cast<Cape_c*>(i_this)->create(); }
static int capeDelete(void* i_this) { return static_cast<Cape_c*>(i_this)->Delete(); }
static int capeExecute(void* i_this) { return static_cast<Cape_c*>(i_this)->Execute(); }
static int capeDraw(void* i_this) { return static_cast<Cape_c*>(i_this)->Draw(); }
static int capeIsDelete(void*) { return 1; }

const ActorProfileDesc Cape_c::sProfile = {
    .name = LNKCAPE_NAME,
    .priority_group = 7,  // after Link (5), so his joint matrices are final for this tick
    .process_size = sizeof(Cape_c),
    .draw_priority = fpcDwPi_ALINK_e,
    .status = fopAcStts_UNK_0x40000_e | fopAcStts_UNK_0x4000_e,
    .group = fopAc_ACTOR_e,
    .cull_type = fopAc_CULLBOX_0_e,
    .create_function = capeCreate,
    .delete_function = capeDelete,
    .execute_function = capeExecute,
    .is_delete_function = capeIsDelete,
    .draw_function = capeDraw,
};

}  // namespace lnkcape
