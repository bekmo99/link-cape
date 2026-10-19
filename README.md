# Link's Cape

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod that gives Link a cloth-simulated cape, hanging from
his shoulders.

> [!NOTE]
> This mod has been compiled against Dusklight v2.0.2's headers, but has **not been run in the game yet**.
> Expect to tune the fit (see below), and please report anything odd.

## How it works

- The mod registers a custom actor (`lnkcape`) through Dusklight's `ActorService` and spawns it as a child of
  Link whenever he exists. It deletes itself when Link goes away (stage changes, game over) and the mod respawns it.
- Each game tick the cape reads the position of Link's upper spine joint (`backbone2`), works out two shoulder
  attachment points behind it, and runs a small Verlet cloth simulation (9 x 12 grid) hanging from them. The cloth
  stays behind Link's back, doesn't sink below his feet, and streams out behind him when he runs.
- It is drawn with the same GX set-up the game's own cloak actor uses (one TEV stage, scene lighting), so it picks up
  the area's lighting. It is hidden in wolf form.
- Nothing is extracted from the game. The geometry is generated in code, and the artwork is a texture replacement
  shipped with the mod.

## Build

```sh
cmake -B build
cmake --build build
```

The result is `build/mods/link_cape.dusk`. Copy it into the game's mods folder:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

During development, rebuild, copy and click **Reload** in the in-game mod manager.

> [!IMPORTANT]
> A mod built locally is only valid for your own platform. Pushing a tag builds the cross-platform bundle
> (see `.github/workflows/build.yml`).

## Fitting the cape

The two top corners of the cape are placed relative to Link's upper spine, and I could not test where that lands on
his body. Open the **Mods** window, select *Link's Cape*, and adjust:

| Setting | What it does |
|---|---|
| Shoulder height | Raises / lowers the attachment points |
| Distance behind back | Moves them away from his back (increase if the cape is inside him) |
| Half width | Half the distance between the two attachment points |
| Length | How long the cape is |
| Shoulder wrap | How far the top corners curl forward over the shoulders (0 = flat) |
| Body padding | Extra clearance around Link's body colliders; raise it if the cape clips his shield, sword or bulky outfits |

Values are saved in the game's `config.json` under `mod.com.bekmo99.link_cape.*`.

**Troubleshooting**

- *The cape is dark, or only its inside shows:* turn on **Flip faces**. The mod decides which side of the cloth is
  "outside" from the geometry, but I could not verify the renderer's winding convention.
- *The cape is a flat red-brown instead of green with gold trim:* the texture replacement isn't being applied. Check
  that "Enable Texture Replacements" is on in Dusklight's settings. The flat colour is the placeholder the mod draws
  with.

## Changing the cape's look

The cape is drawn with a small placeholder texture, and the actual artwork is the texture replacement
`textures/tex1_80x144_$_5.png`. Paint over that PNG, keeping the file name (any resolution with a 5:9 aspect ratio
works; it ships at 160x288):

- Transparent pixels cut the silhouette (the ragged hem is done this way).
- The `80x144` size is deliberately unusual and the `$` means "any content hash", so it can only match this mod's
  placeholder, never a texture the game uses.
- `tools/make_default_texture.py` regenerates the default art.

To change the physics, the constants at the top of the *Simulation* section of `src/cape_actor.cpp` (drag, gravity,
how wide the hem flares, ...) are the place.

## Known limitations

- Not tested in game yet (see above).
- The cape updates once per game tick and is not interpolated, so at high frame rates its motion can look a little
  steppier than Link's. Dusklight v2.0.2 has no mod-side interpolation service.
- Collision is a set of capsules fitted to Link's torso, hips, arms and legs, plus the ground height at his feet. The
  stowed shield and sword are not separate colliders (use *Body padding*), and it does not know about walls. It can
  still clip when he crawls, rolls, swims or rides.
- It does not react to Link's sword or shield, only to his position and facing.

## Updating Dusklight

Change the `DUSKLIGHT_VERSION` line in `CMakeLists.txt` to the new release tag (or commit hash) and reconfigure.
