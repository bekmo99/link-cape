#pragma once

#include "mods/svc/actor.h"

// Base actor class definitions
#include "f_op/f_op_actor.h"
#include "SSystem/SComponent/c_phase.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"

#define LNKCAPE_NAME "lnkcape"

namespace lnkcape {

// Cloth grid: kCols strands across, kRows nodes down each strand.
constexpr int kCols = 13;
constexpr int kRows = 16;
constexpr int kCount = kCols * kRows;

struct V3 {
    f32 x, y, z;
};

// Live tuning values. mod.cpp refreshes these from the mod's config vars every frame.
struct Tuning {
    bool enabled = true;
    bool heroOnly = true;    // only show the cape while Link wears the Hero's Clothes
    bool flipFaces = false;  // swap which side of the cloth counts as "outside" (see README)
    f32 anchorUp = 10.0f;    // anchor height above Link's spine joint (backbone2)
    f32 anchorBack = 14.0f;  // anchor distance behind the spine joint
    f32 halfWidth = 22.0f;   // half the distance between the two shoulder anchors
    f32 length = 110.0f;     // cape length
    f32 wrap = 14.0f;        // how far the top corners curl forward around the shoulders
    f32 stiffness = 40.0f;   // 0..100, how strongly the lower cape is pulled back to its resting shape
    f32 padding = 2.0f;      // extra clearance added to every body collider (gear, bulky outfits)
};
extern Tuning gTuning;

// What the GX pipeline draws: world-space positions and normals, copied from the sim each tick.
class Packet_c : public J3DPacket {
public:
    void draw() override;

    dKy_tevstr_c* mTevStr = nullptr;
    bool mVisible = false;
    bool mFlip = false;
    V3 mPos[kCount];
    V3 mNrm[kCount];

    // Frame interpolation: the sim ticks at 30 Hz but the game may draw more often. We keep the
    // previous tick's result too and blend between the two by wall-clock time (see draw()).
    V3 mPosPrev[kCount];
    V3 mNrmPrev[kCount];
    V3 mDrawPos[kCount];
    V3 mDrawNrm[kCount];
    double mTickTime = 0.0;      // seconds, when the latest tick finished
    double mTickInterval = 1.0 / 30.0;
    int mDrawsThisTick = 0;
    int mDrawsLastTick = 0;
    bool mSnap = true;           // next tick: don't interpolate from stale data
};

class Cape_c : public fopAc_ac_c {
public:
    virtual ~Cape_c();
    cPhs_Step create();
    int Delete();
    int Execute();
    int Draw();

    // Registered by mod.cpp
    static s16 sProcName;
    static ActorHandle sActorHandle;
    static const ActorProfileDesc sProfile;
    // The one live cape (there is only ever one Link), or nullptr
    static Cape_c* sInstance;

private:
    void resetSim(const V3* rest);

    Packet_c mPacket;
    V3 mP[kCount];
    V3 mPrev[kCount];
    V3 mLastLinkPos;
    bool mSimInit;
    f32 mStream = 0.0f;  // smoothed run-speed push
};

}  // namespace lnkcape
