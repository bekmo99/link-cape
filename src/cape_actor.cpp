#include "cape_actor.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "m_Do/m_Do_mtx.h"

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DShape.h"

#include <cmath>
#include <cstring>

namespace lnkcape {

Tuning gTuning;

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
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, (kRows - 1) * (kCols - 1) * 6);
    for (int r = 0; r < kRows - 1; r++) {
        for (int c = 0; c < kCols - 1; c++) {
            const int i00 = r * kCols + c;
            const int i01 = i00 + 1;
            const int i10 = i00 + kCols;
            const int i11 = i10 + 1;
            const int order[6] = {i00, i10, i11, i00, i11, i01};
            const int flipped[6] = {i00, i11, i10, i00, i01, i11};
            const int* idx = pk.mFlip ? flipped : order;
            for (int k = 0; k < 6; k++) {
                const int i = idx[k];
                const V3& p = pk.mPos[i];
                const V3& n = pk.mNrm[i];
                GXPosition3f32(p.x, p.y, p.z);
                GXNormal3f32(n.x * ns, n.y * ns, n.z * ns);
                GXTexCoord2f32(
                    static_cast<f32>(i % kCols) / (kCols - 1), static_cast<f32>(i / kCols) / (kRows - 1));
            }
        }
    }
    GXEnd();
}

void Packet_c::draw() {
    if (!mVisible || mTevStr == nullptr) {
        return;
    }
    initTexture();

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
// ---------------------------------------------------------------------------------------------

constexpr int kIterations = 10;
constexpr f32 kMaxSpeed = 30.0f;  // per-tick velocity clamp, keeps contact from launching the cloth
constexpr f32 kDrag = 0.06f;      // fraction of velocity lost per tick
constexpr f32 kGravity = 1.4f;    // units per tick^2
constexpr f32 kRunStream = 0.025f;  // extra backward push per unit of Link's forward speed
constexpr f32 kWiden = 0.35f;     // how much wider the hem is than the shoulders

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

// ---------------------------------------------------------------------------------------------
// Body colliders: capsules fitted to Link's skeleton. Joint numbers are Link's human skeleton
// (al.bmd): 1 backbone1, 2 backbone2, 3 neck, 7/8/9 armL1/armL2/handL, 12/13/14 armR1/armR2/handR,
// 16 waist, 18/19/20 legL1/legL2/footL, 23/24/25 legR1/legR2/footR.
// ---------------------------------------------------------------------------------------------

struct Capsule {
    V3 a, b;
    f32 r;
};

constexpr int kMaxCaps = 16;

static V3 jointPos(daAlink_c* link, int j) {
    MtxP m = link->mpLinkModel->getAnmMtx(j);
    return {m[0][3], m[1][3], m[2][3]};
}

static int buildColliders(daAlink_c* link, f32 pad, Capsule* out) {
    int n = 0;
    auto add = [&](int ja, int jb, f32 r) {
        out[n++] = {jointPos(link, ja), jointPos(link, jb), r + pad};
    };
    add(1, 3, 12.0f);                 // torso: lower spine up to the neck
    add(16, 16, 15.0f);               // hips
    add(7, 7, 9.0f);                  // shoulders (upper arm roots)
    add(12, 12, 9.0f);
    add(7, 8, 7.0f);                  // upper arms
    add(12, 13, 7.0f);
    add(8, 9, 6.0f);                  // forearms
    add(13, 14, 6.0f);
    add(18, 19, 11.0f);               // thighs
    add(23, 24, 11.0f);
    add(19, 20, 8.0f);                // shins
    add(24, 25, 8.0f);
    add(20, 21, 7.0f);                // feet
    add(25, 26, 7.0f);
    return n;
}

// Pushes p out of the capsule. Returns true if it moved, with the outward normal in n.
static bool collideCapsule(V3& p, const Capsule& c, V3& n) {
    const V3 ab = c.b - c.a;
    const f32 len2 = dot(ab, ab);
    f32 t = len2 > 1e-6f ? dot(p - c.a, ab) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const V3 q = c.a + ab * t;
    const V3 d = p - q;
    const f32 dist = length(d);
    if (dist >= c.r) {
        return false;
    }
    n = dist > 1e-4f ? d * (1.0f / dist) : V3{0.0f, 0.0f, -1.0f};
    p = q + n * c.r;
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

static bool collideBox(V3& p, const Box& b, V3& nOut) {
    const V3 rel = p - b.origin;
    f32 l[3];
    f32 lo[3];
    f32 hi[3];
    f32 rad[3];
    for (int k = 0; k < 3; k++) {
        const f32 s = b.scale[k] > 1e-4f ? b.scale[k] : 1.0f;
        l[k] = dot(rel, b.axis[k]) / s;
        rad[k] = b.r / s;
        lo[k] = (k == 0 ? b.mn.x : (k == 1 ? b.mn.y : b.mn.z)) - rad[k];
        hi[k] = (k == 0 ? b.mx.x : (k == 1 ? b.mx.y : b.mx.z)) + rad[k];
    }
    if (l[0] <= lo[0] || l[0] >= hi[0] || l[1] <= lo[1] || l[1] >= hi[1] || l[2] <= lo[2] ||
        l[2] >= hi[2])
    {
        return false;
    }
    // Inside the padded box: leave through the nearest face.
    int axisK = 0;
    f32 best = 1e30f;
    f32 dir = 1.0f;
    for (int k = 0; k < 3; k++) {
        const f32 s = b.scale[k] > 1e-4f ? b.scale[k] : 1.0f;
        const f32 dLo = (l[k] - lo[k]) * s;
        const f32 dHi = (hi[k] - l[k]) * s;
        if (dLo < best) {
            best = dLo;
            axisK = k;
            dir = -1.0f;
        }
        if (dHi < best) {
            best = dHi;
            axisK = k;
            dir = 1.0f;
        }
    }
    nOut = b.axis[axisK] * dir;
    p = p + nOut * best;
    return true;
}

Cape_c::~Cape_c() {}

void Cape_c::resetSim(const V3* top, const V3& back, f32 vRest) {
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            // hang straight down, leaning very slightly backward so it starts off the body
            V3 p = top[c];
            p.y -= vRest * r;
            p = p + back * (vRest * r * 0.08f);
            mP[r * kCols + c] = p;
            mPrev[r * kCols + c] = p;
        }
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
    if (link->mpLinkModel == nullptr) {
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
    const V3 side = {std::cos(angle), 0.0f, -std::sin(angle)};

    // backbone2 (the upper spine) is joint 2 of Link's skeleton; the matrix is world space.
    MtxP spine = link->mpLinkModel->getAnmMtx(2);
    const V3 spinePos = {spine[0][3], spine[1][3], spine[2][3]};
    const V3 center = spinePos + back * gTuning.anchorBack + V3{0.0f, gTuning.anchorUp, 0.0f};

    V3 top[kCols];
    for (int c = 0; c < kCols; c++) {
        const f32 t = static_cast<f32>(c) / (kCols - 1);          // 0 = left, 1 = right
        const f32 u = 1.0f - 2.0f * t;                            // +1 = left end, -1 = right end
        // The ends curl forward and down so the cape wraps over the shoulders.
        top[c] = center + side * (gTuning.halfWidth * u) + back * (-gTuning.wrap * u * u) +
                 V3{0.0f, -gTuning.wrap * 0.5f * u * u, 0.0f};
    }
    f32 topRest[kCols];
    for (int c = 0; c < kCols; c++) {
        const int c1 = c + 1 < kCols ? c + 1 : c - 1;
        topRest[c] = length(top[c1] - top[c]);
    }

    Capsule caps[kMaxCaps];
    const int capCount = buildColliders(link, gTuning.padding, caps);

    Box boxes[kMaxBoxes];
    int boxCount = 0;
    if (link->checkShieldGet()) {
        addModelBoxes(link->mShieldModel, gTuning.padding, boxes, boxCount);
    }
    if (link->checkSwordGet()) {
        addModelBoxes(link->mSheathModel, gTuning.padding, boxes, boxCount);
        addModelBoxes(link->mSwordModel, gTuning.padding, boxes, boxCount);
    }

    const f32 vRest = gTuning.length / (kRows - 1);

    // Teleports (stage change, warp, respawn): start over instead of stretching across the map.
    const V3 linkPos = {link->current.pos.x, link->current.pos.y, link->current.pos.z};
    if (!mSimInit || length(linkPos - mLastLinkPos) > 400.0f) {
        resetSim(top, back, vRest);
    }
    mLastLinkPos = linkPos;

    // --- Integrate ---------------------------------------------------------------------------
    // Smoothed so starting / stopping doesn't snap the cape backward.
    const f32 streamTarget = link->speedF > 0.0f ? link->speedF * kRunStream : 0.0f;
    mStream += (streamTarget - mStream) * 0.2f;
    const f32 stream = mStream;
    for (int r = 1; r < kRows; r++) {
        const f32 rowT = static_cast<f32>(r) / (kRows - 1);
        for (int c = 0; c < kCols; c++) {
            V3& p = mP[r * kCols + c];
            V3& prev = mPrev[r * kCols + c];
            V3 vel = (p - prev) * (1.0f - kDrag);
            const f32 speed = length(vel);
            if (speed > kMaxSpeed) {
                vel = vel * (kMaxSpeed / speed);
            }
            prev = p;
            p = p + vel + V3{0.0f, -kGravity, 0.0f} + back * (stream * rowT);
        }
    }

    // --- Constraints and collision -----------------------------------------------------------
    const f32 groundY = link->current.pos.y + 2.0f;
    for (int it = 0; it < kIterations; it++) {
        for (int c = 0; c < kCols; c++) {
            mP[c] = top[c];
        }
        for (int r = 0; r < kRows; r++) {
            const f32 widen = 1.0f + kWiden * static_cast<f32>(r) / (kRows - 1);
            for (int c = 0; c < kCols; c++) {
                const f32 hRest = topRest[c] * widen;
                V3& p = mP[r * kCols + c];
                if (r + 1 < kRows) {
                    solveDistance(p, mP[(r + 1) * kCols + c], vRest, r == 0, false);
                }
                if (c + 1 < kCols) {
                    solveDistance(p, mP[r * kCols + c + 1], hRest, r == 0, r == 0);
                }
                if (r + 1 < kRows && c + 1 < kCols) {
                    // shear: keeps the cloth from collapsing into a rope
                    const f32 diag = std::sqrt(hRest * hRest + vRest * vRest);
                    solveDistance(p, mP[(r + 1) * kCols + c + 1], diag, r == 0, false);
                }
            }
        }
        for (int r = 1; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                V3& p = mP[r * kCols + c];
                // Stay behind the plane the cape hangs from, so it doesn't pass through his back.
                const f32 depth = dot(p - center, back);
                if (depth < -gTuning.wrap - 1.0f) {
                    p = p + back * (-gTuning.wrap - 1.0f - depth);
                }
                const V3 before = p;
                V3 hitN = {0.0f, 0.0f, 0.0f};
                bool hit = false;
                V3 n;
                // Rows 0-1 hang from the (pinned) anchors, which may sit inside the padded body
                // shapes; colliding them would only fight the pins and make the top shake.
                if (r >= 2) {
                    for (int k = 0; k < capCount; k++) {
                        if (collideCapsule(p, caps[k], n)) {
                            hit = true;
                            hitN = hitN + n;
                        }
                    }
                    for (int k = 0; k < boxCount; k++) {
                        if (collideBox(p, boxes[k], n)) {
                            hit = true;
                            hitN = hitN + n;
                        }
                    }
                }
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
    for (int c = 0; c < kCols; c++) {
        mP[c] = top[c];
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

    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            const int i = r * kCols + c;
            mPacket.mPos[i] = mP[i];
            mPacket.mNrm[i] = normalized(gridNormal(r, c) * sign, back);
        }
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
