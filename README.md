# mod-world-boss-scale

Aldrynth custom AzerothCore module: linearly scale outdoor world bosses from their designed raid size down to a configurable player floor (default 1) using a live threat-list recount.

Does **not** touch instance content. Use [mod-autobalance](https://github.com/azerothcore/mod-autobalance) for dungeons and raids.

## Features

1. **Catalog** — Classic 40-man world bosses and TBC 25-man outdoor bosses. WotLK has no official outdoor raid bosses; add later via `ExtraEntries`.
2. **Live recount** — unique players on the boss threat list, about once per second in combat. Joins raise the multiplier; leaves/deaths can lower it.
3. **Linear curve** — `clamp(count, MinPlayers, designed) / designed`. Solo Classic = 2.5%, solo TBC = 4%, full designed size = 100%.
4. **What scales** — max HP (current % preserved), outgoing melee/spell/DoT, heals the boss receives, and boss summons.
5. **What does not** — loot, XP, money (full rewards).
6. **Evade** — restore original create health so the next pull starts clean.
7. **`.wbs status`** — GM command on a targeted boss: count, designed size, multipliers, original vs current HP.

## Catalog

| Era | Boss | Entry | Designed |
|-----|------|-------|----------|
| Classic | Azuregos | 6109 | 40 |
| Classic | Lord Kazzak | 12397 | 40 |
| Classic | Ysondre | 14887 | 40 |
| Classic | Lethon | 14888 | 40 |
| Classic | Emeriss | 14889 | 40 |
| Classic | Taerar | 14890 | 40 |
| TBC | Doomwalker | 17711 | 25 |
| TBC | Doom Lord Kazzak | 18728 | 25 |

## Config

| Key | Default | Meaning |
|-----|---------|---------|
| `WorldBossScale.Enable` | 1 | Master switch |
| `WorldBossScale.Announce` | 0 | Verbose catalog on load |
| `WorldBossScale.MinPlayers` | 1 | Floor (test value; raise after pulls) |
| `WorldBossScale.HealthMod` | 1.0 | Extra HP / incoming-heal knob |
| `WorldBossScale.DamageMod` | 1.0 | Extra outgoing-damage knob |
| `WorldBossScale.ThreatRange` | 150 | Ignore threat farther than this |
| `WorldBossScale.RecountMs` | 1000 | Recalc interval in combat |
| `WorldBossScale.ExtraEntries` | "" | `entry:size, entry:size` |

## Install

```bash
cd modules
git submodule add https://github.com/VenomekPL/mod-world-boss-scale.git mod-world-boss-scale
# reconfigure CMake, rebuild, copy conf.dist → conf, restart worldserver
```

GM: target a world boss and run `.wbs status`.
