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

constexpr int kIterations = 8;
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

    mPacket.mVisible = gTuning.enabled && !link->checkWolf();
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
        top[c] = center + side * (gTuning.halfWidth * (1.0f - 2.0f * t));
    }

    const f32 vRest = gTuning.length / (kRows - 1);
    const f32 hRestTop = (2.0f * gTuning.halfWidth) / (kCols - 1);

    // Teleports (stage change, warp, respawn): start over instead of stretching across the map.
    const V3 linkPos = {link->current.pos.x, link->current.pos.y, link->current.pos.z};
    if (!mSimInit || length(linkPos - mLastLinkPos) > 400.0f) {
        resetSim(top, back, vRest);
    }
    mLastLinkPos = linkPos;

    // --- Integrate ---------------------------------------------------------------------------
    const f32 stream = link->speedF > 0.0f ? link->speedF * kRunStream : 0.0f;
    for (int r = 1; r < kRows; r++) {
        const f32 rowT = static_cast<f32>(r) / (kRows - 1);
        for (int c = 0; c < kCols; c++) {
            V3& p = mP[r * kCols + c];
            V3& prev = mPrev[r * kCols + c];
            const V3 vel = (p - prev) * (1.0f - kDrag);
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
            const f32 hRest = hRestTop * widen;
            for (int c = 0; c < kCols; c++) {
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
                if (depth < -1.0f) {
                    p = p + back * (-1.0f - depth);
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
    const V3 mid = gridNormal(kRows / 2, kCols / 2);
    const bool windingInward = dot(mid, back) < 0.0f;
    const f32 sign = windingInward ? -1.0f : 1.0f;
    mPacket.mFlip = windingInward != gTuning.flipFaces;

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
