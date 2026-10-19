#pragma once

#include "mods/svc/actor.h"

// Base actor class definitions
#include "f_op/f_op_actor.h"
#include "SSystem/SComponent/c_phase.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"

#define LNKCAPE_NAME "lnkcape"

namespace lnkcape {

// Cloth grid: kCols strands across, kRows nodes down each strand.
constexpr int kCols = 9;
constexpr int kRows = 12;
constexpr int kCount = kCols * kRows;

struct V3 {
    f32 x, y, z;
};

// Live tuning values. mod.cpp refreshes these from the mod's config vars every frame.
struct Tuning {
    bool enabled = true;
    bool flipFaces = false;  // swap which side of the cloth counts as "outside" (see README)
    f32 anchorUp = 10.0f;    // anchor height above Link's spine joint (backbone2)
    f32 anchorBack = 14.0f;  // anchor distance behind the spine joint
    f32 halfWidth = 22.0f;   // half the distance between the two shoulder anchors
    f32 length = 110.0f;     // cape length
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
    void resetSim(const V3* top, const V3& back, f32 vRest);

    Packet_c mPacket;
    V3 mP[kCount];
    V3 mPrev[kCount];
    V3 mLastLinkPos;
    bool mSimInit;
};

}  // namespace lnkcape
