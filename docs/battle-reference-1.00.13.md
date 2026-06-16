# Battle reference - TaskBarHero 1.00.13

Generated from static, read-only sources:

- AssetRipper ExportedProject: `C:\Users\mbrar\OneDrive\Documentos\Outros\tbh-compiler\work\AssetRipper-1.00.13-20260615-204727\ExportedProject`
- Frontend pack manifest: `C:\Users\mbrar\OneDrive\Documentos\Outros\tbh-companion-agent\exported-assets\TaskBarHero-1.00.13\frontend-pack\manifest.json`
- Il2CppDumper dump: `C:\Users\mbrar\OneDrive\Documentos\Outros\tbh-companion-agent\scripts\.cache\dump\dump.cs`

## Routines run

- `py scripts\refresh_il2cpp_map.py --dry-run --no-live`
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\run-release.ps1 -SkipAgentRelease -SkipFrontend`
- `py scripts\extract_battle_reference.py --version 1.00.13`

## Counts

- Images in frontend-pack: 1844
- Audio clips in frontend-pack: 307
- Contact sheets: 33
- Parsed sprites: 6799
- Parsed AnimationClips: 438
- Heroes: 6
- Monsters: 61
- Skills: 106
- Stages: 120

## Battle model facts

- `HeroInfoData.txt` links each hero to `Prefab/Hero/Hero_<key>` and `Animation/Hero/<key>/Hero_<key>`.
- `MonsterInfoData.txt` links each monster to `Prefab/Monster/Monster_<key>` and `Animation/Monster/<key>/Monster_<key>`.
- `SkillInfoData.txt` is the authoritative source for activation type, range, order, damage type, delivery type, and `Animation/Skill/Skill_*` clips.
- `StageInfoData.txt` is the authoritative source for weighted stage monsters and boss monster keys.
- Units use `AnimatorController/UnitBaseAnimator.controller`; per-unit variation is applied through `AnimatorOverrideController/Hero_<key>.overrideController` and `Monster_<key>.overrideController`.
- Hero prefabs include named anchors such as `View`, `EffectPosition`, `HitEffectPosition`, `BoundCenterPosition_AutoSet`, `BoundBottomPosition_AutoSet`, `HeadUpPosition_AutoSet`, `ProtjectileFirePosition`, `ProtjectileTargetPos_1`, `ProtjectileTargetPos_2`, `SpawnTurretPos`, and `PrefabPosition`.
- Do not flip sprites by assumption. Use the prefab `View` scale/position and the extracted sprite frames as the default orientation reference.
- Method bodies in the AssetRipper C# export are stubs for IL2CPP. Exact runtime decisions such as target selection and movement timing still require native decompilation in Ghidra/IDA using the regenerated `dump.cs`/`script.json` names.

## UnitBaseAnimator states

New State, idle, walk, dead, skill0, skill1, skill2, 101_Run, 201_Walk, 301_Walk, run, Skill_30001, Skill_30001_2, Skill_10201, Skill_10301, Skill_10101, 101_Dead, 101_Idle, 101_Landing, 101_Run 0, 101_Walk, Skill_10001, Skill_10001_2, Skill_10001_3, baselanding, Skill_100111, Skill_100211, Skill_100221, Skill_100231, Skill_100311, Skill_100411, Skill_100421, Skill_100431, Skill_100511, Skill_100521, Skill_100531, Skill_109011, Skill_30301, 201_Landing, Skill_109011_2, Skill_109021, 301_Landing, 401_Dead, 401_Idle, 401_Landing, 401_Run, 401_Walk, 501_Dead, 501_Idle, 501_Landing, 501_Run, 501_Walk, 601_Dead, 601_Idle, 601_Landing, 601_Run, 601_Walk, Skill_20401, Skill_20501, Skill_20601, Skill_30401, Skill_30501, Skill_30601, Skill_10401, Skill_10501, Skill_10601, Skill_40001, Skill_50001, Skill_60001, revivie, Skill_60101, Skill_50101, Skill_40101, Skill_40201, Skill_40301, Skill_40401, Skill_40501, Skill_40601, Skill_50201, Skill_50301

## Heroes

- Hero 101 `Knight` speed=950 attackSpeed=90 clips: 101_Idle:7f/0.616667s, 101_Walk:7f/0.616667s, 101_Run:7f/0.416667s, 101_Dead:14f/1.016667s, 101_Landing:10f/0.616667s, 101_ForceResurrection:10f/0.833333s
- Hero 201 `Ranger` speed=850 attackSpeed=100 clips: 201_Idle:7f/0.816667s, 201_Walk:7f/0.616667s, 201_Run:7f/0.416667s, 201_Dead:14f/1.016667s, 201_Landing:10f/0.616667s, 201_ForceResurrection:10f/0.833333s
- Hero 301 `Sorcerer` speed=770 attackSpeed=55 clips: 301_Idle:7f/0.616667s, 301_Walk:7f/0.416667s, 301_Run:7f/0.416667s, 301_Dead:15f/1.016667s, 301_Landing:9f/0.683333s, 301_ForceResurrection:10f/0.833333s
- Hero 401 `Priest` speed=700 attackSpeed=90 clips: 401_Idle:7f/0.516667s, 401_Walk:7f/0.516667s, 401_Run:7f/0.516667s, 401_Dead:15f/1.016667s, 401_Landing:10f/0.516667s, 401_ForceResurrection:10f/0.833333s
- Hero 501 `Hunter` speed=750 attackSpeed=70 clips: 501_Idle:7f/0.516667s, 501_Walk:7f/0.516667s, 501_Run:7f/0.516667s, 501_Dead:14f/1.016667s, 501_Landing:10f/0.516667s, 501_ForceResurrection:10f/0.833333s
- Hero 601 `Slayer` speed=850 attackSpeed=70 clips: 601_Idle:7f/0.516667s, 601_Walk:7f/0.516667s, 601_Run:7f/0.516667s, 601_Dead:14f/1.016667s, 601_Landing:10f/0.516667s, 601_ForceResurrection:10f/0.833333s

## Stage 1101 sample

- waves=10 waveMonsterAmount=1 boss=10022 bossScale=3.0
- weighted monsters: [{'monsterKey': 10011, 'weight': 1000}, {'monsterKey': 10021, 'weight': 1000}]

## Output

- JSON reference: `C:\Users\mbrar\OneDrive\Documentos\Outros\tbh-companion-agent\docs\battle-reference-1.00.13.json`
