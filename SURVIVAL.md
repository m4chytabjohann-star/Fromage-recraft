# Fromage Survival

Survival mode for fromage 0.93. Survival rules follow VoXide's survival code
(health 20, hunger 20, fall damage past 3 blocks, lava, drowning after 15 s,
slow regeneration when well fed, starvation down to 1 hp), re-written in C.
Textures: Minecraft Classic terrain atlas (res/atlas.png).

## Controls (survival)

| Button | Action |
|---|---|
| L2 (hold) | Mine the targeted block (takes time, bar under the crosshair) |
| R2 | Place the selected block (uses one from your inventory) |
| L1 / R1 | Previous / next hotbar slot |
| Square (S) | Cycle the selected slot through everything you carry |
| Circle (O) | Eat the selected food |
| SELECT | Crafting menu |
| START | Menu (new level, save, load, switch Survival/Creative) |

## What is in

- Health and hunger bars, regeneration, starvation (never below 1 hp)
- Fall, lava, drowning and TNT damage; respawn at the world spawn (inventory is kept)
- Mining time per block, drops (stone > cobblestone, grass > dirt, ...), inventory with counts
- 8 crafting recipes; leaves sometimes drop a red mushroom (food, +3 hunger)
- Saves keep health, hunger and inventory; old saves load as Creative

## Not in yet

Tools, mobs, day/night, furnace, chests. Pick-block (O) is replaced by eating.

## Build

Same as upstream fromage (candyk-psx + mipsel-elf toolchain, see README.md).
`src/survival.c` is already in the Makefile. Texture conversion needs scipy and Pillow.
