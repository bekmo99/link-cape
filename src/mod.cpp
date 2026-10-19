#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

// Game includes
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

#include "cape_actor.hpp"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

using lnkcape::Cape_c;
using lnkcape::gTuning;

// ---------------------------------------------------------------------------------------------
// Settings (saved in config.json, editable from the mod's panel in the Mods window)
// ---------------------------------------------------------------------------------------------

static ConfigVarHandle sShowVar = 0;
static ConfigVarHandle sFlipVar = 0;
static ConfigVarHandle sHeroVar = 0;
static ConfigVarHandle sLengthVar = 0;
static ConfigVarHandle sStiffVar = 0;

static ModResult registerBool(const char* name, bool def, ConfigVarHandle* out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = def;
    return svc_config->register_var(mod_ctx, &desc, out);
}

static ModResult registerInt(const char* name, int64_t def, ConfigVarHandle* out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    return svc_config->register_var(mod_ctx, &desc, out);
}

static void refreshTuning() {
    bool b = false;
    int64_t i = 0;
    if (svc_config->get_bool(mod_ctx, sShowVar, &b) == MOD_OK) {
        gTuning.enabled = b;
    }
    if (svc_config->get_bool(mod_ctx, sHeroVar, &b) == MOD_OK) {
        gTuning.heroOnly = b;
    }
    if (svc_config->get_bool(mod_ctx, sFlipVar, &b) == MOD_OK) {
        gTuning.flipFaces = b;
    }
    if (svc_config->get_int(mod_ctx, sLengthVar, &i) == MOD_OK) {
        gTuning.length = static_cast<f32>(i);
    }
    if (svc_config->get_int(mod_ctx, sStiffVar, &i) == MOD_OK) {
        gTuning.stiffness = static_cast<f32>(i);
    }
}

static void addNumber(UiElementHandle panel, const char* label, const char* help,
    ConfigVarHandle var, int64_t min, int64_t max, const char* suffix) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    control.min = min;
    control.max = max;
    control.step = 1;
    control.suffix = suffix;
    svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
}

static void addToggle(UiElementHandle panel, const char* label, const char* help,
    ConfigVarHandle var) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
}

static ModResult buildPanel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Cape");
    addToggle(panel, "Show cape", "Turns Link's cape on or off.", sShowVar);
    addToggle(panel, "Hero's Clothes only",
        "Only draw the cape while Link is wearing the Hero's Clothes (not the Ordon clothes, Zora "
        "armor or Magic Armor).",
        sHeroVar);
    addNumber(panel, "Length", "How long the cape is, from the collar to the hem.", sLengthVar, 40,
        140, " units");
    addNumber(panel, "Stiffness",
        "How stiff the cloth below the waist is. Higher hugs his body more and sways less.",
        sStiffVar, 0, 100, "");

    svc_ui->pane_add_section(mod_ctx, panel, "Troubleshooting");
    addToggle(panel, "Flip faces",
        "Turn this on if the cape looks dark or only shows its inside.", sFlipVar);
    return MOD_OK;
}

// ---------------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------------

static int sSpawnCooldown = 0;

// Link is recreated on every stage load, and the cape deletes itself when its parent goes away, so
// all we need to do is make sure there is one whenever Link exists.
static void ensureCape() {
    if (Cape_c::sInstance != nullptr) {
        return;
    }
    if (sSpawnCooldown > 0) {
        sSpawnCooldown--;
        return;
    }
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) {
        return;
    }

    ActorSpawnParams params = {};
    params.parameters = 0;
    params.argument = 0;
    params.room_num = static_cast<int8_t>(fopAcM_GetRoomNo(player));
    params.position = {player->current.pos.x, player->current.pos.y, player->current.pos.z};
    params.angle = {0, 0, 0};
    params.scale = {1.0f, 1.0f, 1.0f};

    ActorId id = 0;
    if (svc_actor->create_child_actor(
            mod_ctx, Cape_c::sProcName, fopAcM_GetID(player), &params, &id) != MOD_OK)
    {
        svc_log->warn(mod_ctx, "could not spawn the cape; will retry");
    }
    // Creation completes on a later tick; don't queue duplicates while we wait.
    sSpawnCooldown = 60;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    if (registerBool("show_cape", true, &sShowVar) != MOD_OK ||
        registerBool("flip_faces", false, &sFlipVar) != MOD_OK ||
        registerBool("hero_only", true, &sHeroVar) != MOD_OK ||
        registerInt("length", 100, &sLengthVar) != MOD_OK ||
        registerInt("stiffness", 40, &sStiffVar) != MOD_OK)
    {
        svc_log->error(mod_ctx, "failed to register config vars");
        return MOD_ERROR;
    }
    refreshTuning();

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = buildPanel;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        svc_log->warn(mod_ctx, "could not register the settings panel");
    }

    if (svc_actor->register_actor(
            mod_ctx, &Cape_c::sProfile, &Cape_c::sProcName, &Cape_c::sActorHandle) != MOD_OK)
    {
        svc_log->error(mod_ctx, "failed to register the cape actor");
        return MOD_ERROR;
    }

    svc_log->info(mod_ctx, "link_cape initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    refreshTuning();
    ensureCape();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // Reload / disable: take the cape out of the world so nothing refers to our code afterwards.
    if (Cape_c::sInstance != nullptr) {
        fopAcM_delete(Cape_c::sInstance);
    }
    return MOD_OK;
}
}
