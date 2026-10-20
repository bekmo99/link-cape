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
constexpr int kPinnedRows = 6;  // rows carried rigidly by his body (the simulation's kRigid + 1)

// The cloth is drawn subdivided (Catmull-Rom through the simulated points) so the surface is smooth.
constexpr int kSub = 3;
constexpr int kRefCols = (kCols - 1) * kSub + 1;
constexpr int kRefRows = (kRows - 1) * kSub + 1;

struct V3 {
    f32 x, y, z;
};

// Live tuning values. mod.cpp refreshes these from the mod's config vars every frame.
struct Tuning {
    bool enabled = true;
    bool heroOnly = true;    // only show the cape while Link wears the Hero's Clothes
    bool flipFaces = false;  // swap which side of the cloth counts as "outside" (see README)
    f32 length = 110.0f;     // cape length, collar to hem
    f32 stiffness = 40.0f;   // 0..100, how strongly the hanging cape is pulled back to its resting shape
};
extern Tuning gTuning;

// Set by mod.cpp so the actor can write to the game's log.
extern void (*gLogWarn)(const char* msg);

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
    V3 mRefPos[kRefCols * kRefRows];
    V3 mRefNrm[kRefCols * kRefRows];
    double mTickTime = 0.0;      // seconds, when the latest tick finished
    double mTickInterval = 1.0 / 30.0;
    int mDrawsThisTick = 0;
    int mDrawsLastTick = 0;
    double mDrawEma = 1.0;       // average number of draws per tick
    bool mInterpOn = false;      // blend between ticks (only when the game draws more than once per tick)
    bool mSnap = true;           // next tick: don't interpolate from stale data

    // The pinned rows are stored relative to his chest and waist joints, so at draw time they can be
    // placed with the game's own frame-interpolated joint matrices. That keeps the top of the cape
    // exactly on his back between ticks (blending cloth positions by wall-clock time could never
    // match the body's own interpolation, and the cape lagged behind / rubber-banded).
    const f32 (*mJointMtx[2])[4] = {nullptr, nullptr};
    V3 mLocChest[kPinnedRows * kCols];
    V3 mLocWaist[kPinnedRows * kCols];
    f32 mRowWaist[kPinnedRows] = {};  // how much each pinned row follows the waist (rest: chest)
    bool mAttachValid = false;
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
    V3 mSphereSm[180];   // body collision spheres, smoothed over time (kBodySphereCount)
    bool mSphereInit = false;
    int mLogCooldown = 0;
    // Which column of the spine / chest / waist joint matrices is "side" and "up", worked out once
    // while he stands still (index 0 = spine, 1 = chest, 2 = waist). Lets the pinned top follow the
    // real orientation of those joints, twist included.
    bool mAxValid[3] = {false, false, false};
    int mAxUp[3] = {0, 0, 0};
    int mAxSide[3] = {0, 0, 0};
    f32 mAxUpSign[3] = {1.0f, 1.0f, 1.0f};
    f32 mAxSideSign[3] = {1.0f, 1.0f, 1.0f};
    int mAxBad[3] = {0, 0, 0};  // consecutive ticks the joint matrix disagreed with the joint positions
};

}  // namespace lnkcape
