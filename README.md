# FalloutCraft

![FalloutCraft: a Minecraft player on Red Rocket's gas station](docs/screenshot.jpg)

Play Fallout 4 as a Minecraft player. You move with Minecraft's physics, carry Minecraft's
inventory and HUD, and place and break blocks in the Commonwealth. You fight Fallout's raiders,
mole rats and bloodbugs with Minecraft weapons, and they fight back.

Neither game is rewritten. Minecraft runs its own game logic, and Fallout 4 runs its world, NPCs,
quests, Pip-Boy and saves. A Fallout 4 F4SE plugin and a Minecraft Fabric mod talk to each other
through shared memory. Minecraft runs hidden in the background, and Fallout draws everything.

FalloutCraft is a fork of [SkyCraft](https://github.com/chasmlol/SkyCraft), which does the same
for Skyrim. It keeps SkyCraft's Minecraft mod and replaces the Skyrim plugin with a Fallout 4 one.

> **Status: early and experimental.** Expect rough edges, and back up your saves.
> This is a fan project. It isn't affiliated with Mojang, Microsoft, Bethesda or ZeniMax, and you
> need to own both games.

## What works

- **Movement:** Minecraft movement on Fallout's terrain, roads and buildings: walking,
  sprinting, jumping, crouching, swimming and falling. Fallout's collision (Havok) is streamed
  into Minecraft's own collision, and Fallout's ground under your feet is double-checked every
  frame so you don't fall through cracked roads.
- **Blocks:** place and break blocks anywhere in the Commonwealth. They're drawn inside Fallout's
  frame and hide behind Fallout's walls. Dropped items, arrows and block-breaking cracks are drawn
  too.
- **Combat:** hit Fallout's NPCs and creatures with any Minecraft weapon, including bows and
  tridents. Damage is scaled to the enemy's level. Enemies fight back, stagger on critical and
  knockback hits, and your kills count as yours.
- **Health:** Minecraft's hearts are your health. Fallout's damage (melee, bullets, bleeding) is
  sent to Minecraft, Minecraft's armour and shield reduce it, eating and natural regeneration heal
  you, and Fallout's health bar follows Minecraft's. If you die in Minecraft, you die in Fallout.
- **S.P.E.C.I.A.L.:** your Fallout stats shape your Minecraft body (5 is average; above is a
  bonus, below a penalty):

  | Stat | In Minecraft |
  |---|---|
  | Strength | Melee damage, knockback resistance |
  | Perception | Reach |
  | Endurance | Maximum health, breath under water, armour toughness |
  | Agility | Walking and running speed, attack speed, safer falls |
  | Luck | Minecraft's luck (better loot) |

- **Pip-Boy:** Tab opens the Pip-Boy as usual, radio included.
- **Camera:** Minecraft's F5 camera modes (behind and in front) show your Minecraft skin and
  armour in Fallout. Fallout's own first-person arms are hidden.
- **Interiors:** every interior and every other worldspace gets its own place in the Minecraft
  world, so what you build in one never shows up in another.

## Requirements

**Fallout 4**

| | |
|---|---|
| Fallout 4 (Steam), current runtime | Developed and tested on **1.11.240**. The old-gen 1.10.163 runtime isn't supported. |
| [Fallout 4 Script Extender (F4SE)](https://f4se.silverlock.org/) ([Nexus](https://www.nexusmods.com/fallout4/mods/42147)) | For your game version (tested with 0.7.9) |
| [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327) | The "All in One" file for your game version |

> **Recommended:** play from a save made after you leave Vault 111. The opening (the pre-war
> house and the vault) is heavily scripted and may not work while Minecraft drives the player.

**Minecraft**

| | |
|---|---|
| Minecraft: Java Edition | A Microsoft account that owns it |
| Minecraft 26.3 with [Fabric Loader](https://fabricmc.net/) 0.19.5 or newer | Started from this repo with `gradlew runClient` (see below) |
| [Fabric API](https://modrinth.com/mod/fabric-api) for 26.3 | Pulled in by the build |
| Java 25 (JDK) | To build and run the Minecraft mod |

Minecraft runs hidden next to Fallout. Budget about 3 GB of extra RAM and a GPU that runs
Minecraft 26.3.

**To play** you also need **JDK 25** (it runs the Minecraft mod) and **Git** (or download this
repo as a ZIP from GitHub).

**To build the plugin yourself** (optional: a compiled one comes with the repo):

| | |
|---|---|
| [Visual Studio](https://visualstudio.microsoft.com/) 2022 or newer | With the **Desktop development with C++** workload (the MSVC compiler) |
| [xmake](https://xmake.io/) | Builds the Fallout 4 plugin |
| [Git](https://git-scm.com/) | To clone this repo and the CommonLibF4 template |

## Installing

The repo comes with the plugin already compiled, so you don't need to build anything to play.
(If you'd rather build it yourself, see [Building from source](#building-from-source).)

### 1. Fallout 4 mods

Install these first, with your mod manager (Mod Organizer 2, Vortex) or by hand:

1. **F4SE:** copy `f4se_loader.exe`, the `f4se_1_*.dll` files and the `Data` folder from the F4SE
   archive into your Fallout 4 folder (where `Fallout4.exe` is). From now on, start the game with
   `f4se_loader.exe` (or through your mod manager).
2. **Address Library for F4SE Plugins:** its `.bin` file goes into
   `Fallout 4\Data\F4SE\Plugins\`.

### 2. Get FalloutCraft

```bat
git clone https://github.com/zeyvu/FalloutCraft
```

(or **Code > Download ZIP** on GitHub and unzip it).

### 3. Install the plugin

Copy **`FO4_Release\Data\F4SE\Plugins\commonlibf4-template.dll`** into
**`Fallout 4\Data\F4SE\Plugins\`** (next to the Address Library's `.bin`). That's the FalloutCraft
plugin; F4SE loads it when the game starts.

> With Mod Organizer 2 or Vortex: zip the `FO4_Release\Data` folder (so the zip has `Data` at its
> top) and install the zip as a mod.

### 4. Start Minecraft

```bat
cd FalloutCraft\fabric
gradlew runClient
```

That builds the FalloutCraft Minecraft mod and starts Minecraft with it. The first run downloads
Minecraft 26.3, Fabric and the Fabric API (a few minutes). Minecraft hides its window and waits
for Fallout. Leave it running.

### 5. Play

1. With Minecraft running, start **Fallout 4 through F4SE** (`f4se_loader.exe` or your mod
   manager) and load a save.
2. The two find each other on their own: Minecraft opens its FalloutCraft world, moves to where
   your Fallout character stands, and takes over. You'll see Minecraft's hotbar and hearts over
   Fallout.

The order doesn't matter, and either side can be restarted: they reconnect.

To check the plugin loaded, open `Documents\My Games\Fallout4\F4SE\commonlibf4-template.log`:
it starts with `SkyCraft (FalloutCraft) plugin build <date> <time>`.

### Updating

`git pull` (or download the ZIP again), copy the new `commonlibf4-template.dll` over the old one,
and start Minecraft again with `gradlew runClient` (it rebuilds the mod). Update both halves
together: they share a memory layout and only talk to a matching version.

### Uninstalling

Delete `Fallout 4\Data\F4SE\Plugins\commonlibf4-template.dll`. FalloutCraft doesn't change your
Fallout saves beyond what you did while playing.

## Controls

Minecraft has priority. These keys still go to Fallout:

| Key | Does |
|---|---|
| **E** | Fallout activate: doors, NPCs (talk), containers, terminals, workbenches |
| **Tab** | Pip-Boy |
| **Esc** | Fallout pause menu (or closes an open Minecraft screen) |
| **~** | Fallout console |
| **F9** | Fallout quickload |

And these are Minecraft's, moved or changed for Fallout:

| Key | Does |
|---|---|
| **I** | Minecraft inventory (E is Fallout's activate) |
| **Shift** | Sprint |
| **Ctrl** | Crouch / sneak |
| **O** | Minecraft pause / options menu |

Every other key is Minecraft's: **F5** camera, **T** chat, **/** commands, the hotbar keys, the
mouse buttons and so on.

## Known limitations

- If something goes wrong, the logs say what:
  - `Documents\My Games\Fallout4\F4SE\commonlibf4-template.log` (the Fallout plugin)
  - `Documents\My Games\Fallout4\F4SE\SkyCraft_crash.log` (crashes, with function names)
  - `fabric\run\logs\latest.log` (Minecraft)
- Some cracked or uneven ground (Concord's broken roads, rubble) can still make the player stumble
  for a moment.
- Fallout's inventory and perk screens open through the Pip-Boy; Fallout's weapons and VATS can't
  be used while Minecraft drives the player.
- Fallout's stimpaks and food don't heal Minecraft's hearts yet; eat Minecraft food.
- Charisma and Intelligence don't change anything in Minecraft yet.
- Not ported from SkyCraft yet: digging into the world itself, Minecraft lights lighting Fallout,
  Minecraft water, NPCs walking around blocks, training Fallout skills from Minecraft play, and
  multiplayer.
- `FalloutCraft_worlds.txt` (next to the logs) remembers where each interior lives in the
  Minecraft world. Deleting it moves interiors around, and what you built inside them won't show
  up in the same place.
- Mods that also take over the player's camera or movement will conflict.

## Building from source

Only needed if you change the plugin or want to compile it yourself.

The plugin is built on the
[CommonLibF4 plugin template](https://github.com/libxse/commonlibf4-template). The template is a
separate project with its own license, so this repo doesn't include it: `FO4_ModFiles/` has only
FalloutCraft's own files. You clone the template, drop them in, and build. With
`XSE_FO4_GAME_PATH` set, the build also copies the plugin straight into the game.

In a terminal (cmd), inside the `FalloutCraft` folder:

```bat
:: the CommonLibF4 template (with its submodules), inside the FalloutCraft folder
git clone --recurse-submodules https://github.com/libxse/commonlibf4-template

:: FalloutCraft's sources into the template's src\, and its build file over the template's
copy /Y FO4_ModFiles\*.cpp commonlibf4-template\src\
copy /Y FO4_ModFiles\*.h commonlibf4-template\src\
copy /Y FO4_ModFiles\xmake.lua commonlibf4-template\xmake.lua

:: your Fallout 4 folder (the one with Fallout4.exe), so the build installs into it
setx XSE_FO4_GAME_PATH "C:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
```

`FO4_ModFiles\xmake.lua` is the template's build file plus the Direct3D and DbgHelp libraries the
plugin needs (without them it doesn't link) and a `.pdb` for the crash log.

Close the terminal and open a new one (so it sees `XSE_FO4_GAME_PATH`), then:

```bat
cd FalloutCraft\commonlibf4-template
xmake build -r
```

The first build downloads CommonLibF4's dependencies and takes a few minutes. It ends with:

```
installing commonlibf4-template to "C:\Program Files (x86)\Steam\steamapps\common\Fallout 4\Data .."
install ok!
[100%]: build ok
```

That built `commonlibf4-template.dll` in `commonlibf4-template\build\windows\x64\release\` and
copied it to `Fallout 4\Data\F4SE\Plugins\`. Without `XSE_FO4_GAME_PATH`, copy it there yourself
(the `.pdb` next to it is optional; it lets the crash log name functions). To update the compiled
copy in the repo, copy it to `FO4_Release\Data\F4SE\Plugins\`.

The `commonlibf4-template` folder is ignored by Git: only FalloutCraft's own files are in this
repo.

For development:

- `fabric\gradlew runClient` starts a dev Minecraft that stays running when Fallout closes;
  `gradlew build` builds the mod's `.jar` into `fabric\build\libs\`.
- `docs\DESIGN.md` explains how the two halves fit together (written for SkyCraft), and
  `protocol\skycraft_protocol.h` is the shared-memory layout both sides follow
  (`FO4_ModFiles\skycraft_protocol.h` is the same file).

| Folder | |
|---|---|
| `FO4_Release/` | The compiled plugin, laid out like the game's `Data` folder |
| `FO4_ModFiles/` | The plugin's sources (C++, [CommonLibF4](https://github.com/libxse/commonlibf4)) and its `xmake.lua`, to drop into the CommonLibF4 template |
| `fabric/` | The Minecraft Fabric mod (Java), from SkyCraft with FalloutCraft changes |
| `protocol/` | `skycraft_protocol.h`: the shared-memory layout both sides follow |
| `docs/` | `DESIGN.md` explains how the two halves fit together (written for SkyCraft) |

## Credits

FalloutCraft is based on [SkyCraft](https://github.com/chasmlol/SkyCraft) by
[chasmlol](https://github.com/chasmlol): its Minecraft mod, its shared-memory design and most of
its ideas. The original README is kept as [READMEOriginal.md](READMEOriginal.md).

## License

[MIT](LICENSE)
