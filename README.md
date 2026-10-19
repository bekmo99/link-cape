# Link's Cape

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod that gives Link a cloth-simulated cape, hanging from
his shoulders.

> [!NOTE]
> This mod has been compiled against Dusklight v2.0.2's headers, but has **not been run in the game yet**.
> Expect to tune the fit (see below), and please report anything odd.

## How it works

This cape is made for one model: the Hylian-armor Link model (blue tunic with gold trim, worn as the
Hero's Clothes). It does not draw on any other model or outfit. The top of the cape sits at the shoulders, under the collar.

- The mod registers a custom actor (`lnkcape`) through Dusklight's `ActorService` and spawns it as a child of
  Link whenever he exists. It deletes itself when Link goes away (stage changes, game over) and the mod respawns it.
- The model's back was measured offline (`tools/gen_body_data.py` reads the model's `.dae` and writes
  `src/body_data.hpp`).
- **Collar to waist is pinned.** Those cape points are stored relative to Link's chest and waist and just carried
  along by them: no physics, so the top can never pull away from his back. The cape's top edge sits under the collar.
- **Below the waist it hangs and flows.** The cloth is a small Verlet simulation that falls from the waist row. Link's
  tunic flap, legs and boots are about 180 small spheres glued to his skeleton, so the cloth can't pass through them.
  Gear stowed on his back (shield, sheath, sword) goes over the cape.
- It is drawn with the same GX set-up the game's own cloak actor uses, so it picks up the area's lighting. It is
  hidden in wolf form, and (by default) in any outfit other than the Hero's Clothes.

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

## Settings

Open the **Mods** window and select *Link's Cape*:

| Setting | What it does |
|---|---|
| Show cape | Turns the cape on or off |
| Hero's Clothes only | Only show the cape in the Hero's Clothes (on by default) |
| Length | Collar-to-hem length |
| Stiffness | How strongly the hanging part is pulled back to its resting shape (higher = stiffer, less sway) |
| Flip faces | Troubleshooting: turn on if the cape looks dark or only shows its inside |

Values are saved in the game's `config.json` under `mod.com.bekmo99.link_cape.*`.

**Troubleshooting**

- *The cape is dark, or only its inside shows:* turn on **Flip faces**.
- *The cape is a flat red-brown instead of blue with gold trim:* the texture replacement isn't being applied. Check
  that "Enable Texture Replacements" is on in Dusklight's settings. The flat colour is the placeholder the mod draws
  with.
- *The cape doesn't show up at all:* it only shows for the Hero's Clothes and needs a 35-joint human skeleton.

## Changing the cape's look

The cape is drawn with a small placeholder texture, and the actual artwork is the texture replacement
`textures/tex1_80x144_$_5.png`. Paint over that PNG, keeping the file name (any resolution with a 5:9 aspect ratio
works; it ships at 160x288, blue with gold trim and a Triforce):

- Transparent pixels cut the silhouette (the ragged hem is done this way).
- The `80x144` size is deliberately unusual and the `$` means "any content hash", so it can only match this mod's
  placeholder, never a texture the game uses.
- `tools/make_default_texture.py` regenerates the default art.

To change the physics, the constants at the top of the *Simulation* section of `src/cape_actor.cpp` (drag, gravity,
how wide the hem flares, ...) are the place.

## Known limitations

- Not tested in game yet (see above).
- Blended between game ticks by wall-clock time when the game draws more often than it simulates; it can still look a
  little different from Link's own interpolation.
- Collision is ~180 spheres measured from the model's back, legs and boots, plus the ground height at his feet. Gear
  on his back is boxes from those models' own bounds, and the cape is pushed in under them. It does not know about
  walls, and it can still clip when he crawls, rolls, swims or rides.
- It was made from the model's rest pose; extreme poses can still show small gaps.

## Updating Dusklight

Change the `DUSKLIGHT_VERSION` line in `CMakeLists.txt` to the new release tag (or commit hash) and reconfigure.
