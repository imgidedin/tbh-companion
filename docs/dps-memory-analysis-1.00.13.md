# DPS memory analysis - TaskBarHero 1.00.13

## Goal

Evaluate whether the companion can read enough live memory to log real run DPS by hero/skill/source without writing to the game process.

## Current runtime reader

The current agent reads `LogManager` only. The live cache at `%LOCALAPPDATA%\TBH Companion\memory-history.json` contains stage, death, drop and craft events, but no damage attribution fields.

Observed event keys from the live cache:

```text
category, clock, color, enemy, grade, hero, id, index, item, itemKey, label,
progress, raw, seconds, ts, type
```

Current event totals in the local cache when this analysis was run:

```text
total=6575
death=347
failure=95
clear=3577
drop=2369
craft=187
```

Conclusion: `LogManager` is not enough for DPS. It is useful for run windows and stage result anchors, but not for hit-level damage or skill source.

## Relevant IL2CPP structures

`TaskbarHero.DamageInfo` is the main damage payload:

```text
DamageInfo.Attacker           @ 0x00 Unit*
DamageInfo.OriginDamage       @ 0x08 float
DamageInfo.IsCritical         @ 0x0C bool
DamageInfo.FloatingDamageText @ 0x0D bool
DamageInfo.DamageAttribute    @ 0x10 EDamageAttribute
DamageInfo.DamageType         @ 0x14 EDamageType
DamageInfo.PlayHitFeedBack    @ 0x18 bool
DamageInfo.PlayHitSound       @ 0x19 bool
DamageInfo.HitEffects         @ 0x20 List<BuffEffectData>*
```

Important methods from `scripts/.cache/dump/script.json`:

```text
TaskbarHero_DamageInfo___ctor                                   RVA 0xBEA740
TaskbarHero_DamageInfo__grr                                     RVA 0xBEA810
TaskbarHero_Unit__gtk                                           RVA 0xBFB490
TaskbarHero_Unit__gpw                                           RVA 0xBF9AF0
TaskbarHero_Hero__gpw                                           RVA 0xBED500
TaskbarHero_Monster__gpw                                        RVA 0xBF1F70
TaskbarHero_Hero__edg / TaskbarHero_Monster__edg                 RVA 0xBEB210
TaskbarHero_Combat_ActiveSkill__AttackDamage                    RVA 0xB741B0
TaskbarHero_Combat_ActiveSkill__mgt                             RVA 0xB74C60
TaskbarHero_Combat_ActiveSkill__mfn                             RVA 0xB74A30
TaskbarHero_Combat_Projectile_HunterExplosiveBolt__AttackDamage RVA 0xB780F0
```

`TaskbarHero.Unit` has enough identity fields to distinguish live unit kinds:

```text
Hero.cache             @ 0x3A0 vd*
Monster.cache          @ 0x3B0 vb.ul*
MonsterType            @ 0x3B8
Unit.b_isHero          @ 0x100 bool
UnitHealthController   @ 0xB0
AnimationController    @ 0xB8
```

`TaskbarHero.Combat.ActiveSkill` keeps the skill execution context:

```text
skillSo           @ 0x10 SkillSO*
skillCache        @ 0x18 vi*
skillCastDistance @ 0x20 float
bgrn              @ 0x28 int
bgro              @ 0x2C int
bgrq              @ 0x38 Unit*       owner/caster
AnimClipName      @ 0x40 string
ActionTimeName    @ 0x48 string
bgrt              @ 0x68 beb*        behavior
bgru              @ 0x70 int
bgrx              @ 0x74 float
```

`SkillBehaviorContext` also makes the intended source explicit:

```text
SkillOwner                      @ 0x00 Unit*
SkillCastDistance               @ 0x08 float
MaxCoolDown                     @ 0x0C float
ActivationValue                 @ 0x10 int
SkillCache                      @ 0x18 vi*
ActiveSkill                     @ 0x20 ActiveSkill*
PlayerNormalAttackCountWhenInit @ 0x28 int
```

## Damage path

Static decompilation indicates this flow:

1. `Unit.gtk()` builds normal attack damage from the attacker's current stats and returns `DamageInfo`.
2. `ActiveSkill.AttackDamage()` delegates to the active behavior (`beb.AttackDamage()`), then adjusts attribute/type from `skillCache` and owner stats.
3. Projectile/turret paths store a `Func<DamageInfo>` and invoke it at hit time.
4. `bed.edg(DamageInfo,bool)` dispatches into `Hero.gpw` or `Monster.gpw`.
5. `Hero.gpw` / `Monster.gpw` call `Unit.gpw`.
6. `Unit.gpw` applies absorb/reduction/avoidance and calls the health controller with the final damage value.

The most useful passive observation point for real damage is `Unit.gpw`, after `DamageAbsorbComponent.mtd` has possibly rewritten the `DamageInfo` and before or at the health-controller damage call.

## Feasibility

Real DPS attribution looks possible, but not from the existing `LogManager` reader alone.

What memory has:

- final applied damage can be derived in `Unit.gpw`;
- attacker is inside `DamageInfo.Attacker`;
- target is the `Unit.gpw` receiver;
- critical flag, damage attribute and damage type are in `DamageInfo`;
- active skill source exists during skill execution through `ActiveSkill.skillCache`, `ActiveSkill.bgrq` and `SkillBehaviorContext`.

What memory does not expose as a stable passive list yet:

- no existing damage log list was found in `LogManager`;
- no persistent ring buffer of hits was identified in the current dumps;
- `DamageInfo` is stack/by-value in the hot path, so a polling-only reader can easily miss individual hits unless it finds a persistent UI/object-pool artifact that outlives the frame.

## Recommended implementation options

### Option A - read-only polling with source inference

Poll live heroes, monsters, active skills/projectiles and health values. Infer damage from HP deltas and correlate with recently active skills.

Pros:

- stays strictly read-only;
- matches the companion safety posture.

Cons:

- attribution is probabilistic when multiple heroes hit in the same polling window;
- fast multi-hit, DoT, projectile and reflected/absorbed damage can be missed or misattributed;
- requires robust discovery of StageManager hero/monster collections and current active skill/projectile objects.

Expected quality: useful estimates, not authoritative real DPS.

### Option B - function-level observation / hook

Instrument `Unit.gpw`, `Hero.gpw`, `Monster.gpw` or `DamageInfo.ctor` and emit each hit to a local side-channel.

Pros:

- authoritative hit-level DPS;
- has final damage, attacker, target, critical, type and attribute at the exact application point;
- source attribution can be added by tracking current `ActiveSkill`/`SkillBehaviorContext`.

Cons:

- requires injection/hooking or another active instrumentation technique;
- breaks the current read-only/no-write process posture;
- higher maintenance risk after updates.

Expected quality: authoritative, but outside the current agent safety model.

### Option C - hybrid

Keep the production agent read-only and add a separate development-only analyzer that hooks `Unit.gpw` to validate field offsets and attribution rules. Use the analyzer to calibrate a polling model if production must stay read-only.

This is the safest path for research.

## Next technical steps

1. Extend the Ghidra target list to include all `DamageInfo` producers and all `edg/gpw` callers.
2. Resolve `StageManager` live collections for hero units, monster units, active skill instances and projectile pools.
3. Add a read-only development harness that dumps:
   - live hero pointers and hero keys;
   - live monster pointers and monster keys;
   - active skill pointers, owner pointer and `skillCache` key;
   - health controller values before/after stage ticks.
4. Run a controlled stage with one hero and one known skill, then compare HP deltas against `Unit.gpw` decompilation.
5. Only after the mapping is stable, decide whether production should expose estimated DPS or require an explicit opt-in instrumentation build for authoritative DPS.

## Current conclusion

The game memory contains the right information to know what caused damage and the source unit/skill, but the current companion reader does not yet have a passive event stream for it. Accurate DPS by skill will require either:

- finding and polling persistent combat objects plus accepting inferred attribution, or
- instrumenting the damage application path, especially `TaskbarHero.Unit.gpw`.
