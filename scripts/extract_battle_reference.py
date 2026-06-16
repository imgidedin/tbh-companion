#!/usr/bin/env python3
"""
Extracts a static battle reference from AssetRipper + Il2CppDumper outputs.

This is intentionally read-only against the game. It parses exported Unity YAML,
CSV TextAssets, and the cached IL2CPP dump to produce a reusable reference for
frontend battle visualization work.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import struct
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DUMP_DIR = ROOT / "scripts" / ".cache" / "dump"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig", errors="ignore")


def find_latest_manifest(version: str | None) -> Path:
    base = ROOT / "exported-assets"
    if version:
        manifest = base / f"TaskBarHero-{version}" / "frontend-pack" / "manifest.json"
        if not manifest.exists():
            raise SystemExit(f"manifest not found: {manifest}")
        return manifest

    candidates = sorted(base.glob("TaskBarHero-*/frontend-pack/manifest.json"), key=lambda p: p.stat().st_mtime)
    if not candidates:
        raise SystemExit("no frontend-pack manifest found")
    return candidates[-1]


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def index_guids(assets_dir: Path) -> dict[str, str]:
    index: dict[str, str] = {}
    for meta in assets_dir.rglob("*.meta"):
        text = read_text(meta)
        m = re.search(r"^guid:\s*([0-9a-fA-F]{32})\s*$", text, re.MULTILINE)
        if not m:
            continue
        asset = meta.with_suffix("")
        # For "foo.asset.meta", with_suffix("") returns "foo.asset".
        index[m.group(1).lower()] = str(asset)
    return index


def png_size(path: Path) -> dict[str, int] | None:
    try:
        with path.open("rb") as f:
            header = f.read(24)
        if header[:8] != b"\x89PNG\r\n\x1a\n":
            return None
        width, height = struct.unpack(">II", header[16:24])
        return {"width": int(width), "height": int(height)}
    except OSError:
        return None


def parse_scalar(text: str, key: str) -> str | None:
    m = re.search(rf"^\s*{re.escape(key)}:\s*(.+?)\s*$", text, re.MULTILINE)
    return m.group(1) if m else None


def parse_float(value: str | None) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_sprite(path: Path, guid_index: dict[str, str]) -> dict[str, Any]:
    text = read_text(path)
    texture_guid = None
    m_texture = re.search(r"texture:\s*\{fileID:\s*2800000,\s*guid:\s*([0-9a-fA-F]{32}),", text)
    if m_texture:
        texture_guid = m_texture.group(1).lower()
    rect = {}
    for key in ("x", "y", "width", "height"):
        m = re.search(rf"^\s*{key}:\s*(-?\d+(?:\.\d+)?)\s*$", text, re.MULTILINE)
        if m:
            value = float(m.group(1))
            rect[key] = int(value) if value.is_integer() else value
    ppu = parse_float(parse_scalar(text, "m_PixelsToUnits"))
    return {
        "name": parse_scalar(text, "m_Name") or path.stem,
        "path": str(path),
        "rect": rect,
        "pixelsToUnits": ppu,
        "textureGuid": texture_guid,
        "texturePath": guid_index.get(texture_guid or ""),
    }


def build_sprite_index(assets_dir: Path, guid_index: dict[str, str]) -> dict[str, dict[str, Any]]:
    sprites: dict[str, dict[str, Any]] = {}
    for guid, asset_path in guid_index.items():
        path = Path(asset_path)
        if path.suffix.lower() == ".asset" and "\\Sprite\\" in str(path):
            sprites[guid] = parse_sprite(path, guid_index)
    return sprites


def parse_animation_clip(path: Path, guid_index: dict[str, str], sprites: dict[str, dict[str, Any]]) -> dict[str, Any]:
    text = read_text(path)
    sprite_guids = [g.lower() for g in re.findall(r"value:\s*\{fileID:\s*21300000,\s*guid:\s*([0-9a-fA-F]{32}),", text)]
    times = [
        float(t)
        for t in re.findall(
            r"^\s*(?:-\s*)?time:\s*(-?\d+(?:\.\d+)?(?:E-?\d+)?)\s*$",
            text,
            re.MULTILINE,
        )
    ]
    sample_rate = parse_float(parse_scalar(text, "m_SampleRate")) or 60.0
    frames = []
    widths: list[float] = []
    heights: list[float] = []
    texture_paths: Counter[str] = Counter()
    for i, guid in enumerate(sprite_guids):
        sprite = sprites.get(guid)
        rect = sprite.get("rect", {}) if sprite else {}
        if "width" in rect:
            widths.append(float(rect["width"]))
        if "height" in rect:
            heights.append(float(rect["height"]))
        if sprite and sprite.get("texturePath"):
            texture_paths[str(sprite["texturePath"])] += 1
        frames.append(
            {
                "index": i,
                "time": times[i] if i < len(times) else None,
                "spriteGuid": guid,
                "spriteName": sprite.get("name") if sprite else None,
                "spritePath": sprite.get("path") if sprite else guid_index.get(guid),
                "rect": rect,
            }
        )
    last_time = max(times) if times else 0.0
    estimated_duration = last_time + (1.0 / sample_rate if sprite_guids else 0.0)
    size_mode = None
    if widths and heights:
        size_mode = {
            "minWidth": min(widths),
            "maxWidth": max(widths),
            "minHeight": min(heights),
            "maxHeight": max(heights),
        }
    return {
        "name": parse_scalar(text, "m_Name") or path.stem,
        "path": str(path),
        "sampleRate": sample_rate,
        "wrapMode": parse_scalar(text, "m_WrapMode"),
        "frameCount": len(sprite_guids),
        "lastKeyTime": round(last_time, 6),
        "estimatedDuration": round(estimated_duration, 6),
        "sizeRange": size_mode,
        "texturePaths": [p for p, _count in texture_paths.most_common(5)],
        "frames": frames,
    }


def build_clip_index(assets_dir: Path, guid_index: dict[str, str], sprites: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    clips: dict[str, dict[str, Any]] = {}
    for guid, asset_path in guid_index.items():
        path = Path(asset_path)
        if path.suffix.lower() == ".anim" and "\\AnimationClip\\" in str(path):
            clips[guid] = parse_animation_clip(path, guid_index, sprites)
    return clips


def parse_override_controller(path: Path, guid_index: dict[str, str], clips: dict[str, dict[str, Any]]) -> dict[str, Any]:
    text = read_text(path)
    pairs = []
    pending_original: str | None = None
    for line in text.splitlines():
        m_orig = re.search(r"m_OriginalClip:\s*\{fileID:\s*7400000,\s*guid:\s*([0-9a-fA-F]{32}),", line)
        if m_orig:
            pending_original = m_orig.group(1).lower()
            continue
        m_override = re.search(r"m_OverrideClip:\s*\{fileID:\s*(\d+)(?:,\s*guid:\s*([0-9a-fA-F]{32}),)?", line)
        if m_override and pending_original:
            override_guid = (m_override.group(2) or "").lower() or None
            pairs.append(
                {
                    "originalGuid": pending_original,
                    "originalName": clips.get(pending_original, {}).get("name"),
                    "overrideGuid": override_guid,
                    "overrideName": clips.get(override_guid or "", {}).get("name"),
                    "originalPath": guid_index.get(pending_original),
                    "overridePath": guid_index.get(override_guid or ""),
                }
            )
            pending_original = None
    return {
        "name": parse_scalar(text, "m_Name") or path.stem,
        "path": str(path),
        "baseControllerGuid": (re.search(r"m_Controller:\s*\{fileID:\s*9100000,\s*guid:\s*([0-9a-fA-F]{32}),", text) or [None, None])[1],
        "overrides": pairs,
    }


def parse_animator_controller(path: Path, clips: dict[str, dict[str, Any]]) -> dict[str, Any]:
    text = read_text(path)
    states = []
    for block in re.split(r"\n--- !u!1102 ", text):
        if "AnimatorState:" not in block:
            continue
        name = parse_scalar(block, "m_Name")
        m_motion = re.search(r"m_Motion:\s*\{fileID:\s*7400000,\s*guid:\s*([0-9a-fA-F]{32}),", block)
        guid = m_motion.group(1).lower() if m_motion else None
        states.append({"name": name, "motionGuid": guid, "motionName": clips.get(guid or "", {}).get("name")})
    params = re.findall(r"m_Name:\s*([A-Za-z0-9_]+)\n\s*m_Type:\s*(\d+)", text)
    return {
        "name": parse_scalar(text, "m_Name") or path.stem,
        "path": str(path),
        "parameters": [{"name": n, "type": t} for n, t in params[:80]],
        "states": states,
    }


def parse_vec3(value: str) -> dict[str, float] | None:
    m = re.search(r"\{x:\s*(-?\d+(?:\.\d+)?),\s*y:\s*(-?\d+(?:\.\d+)?),\s*z:\s*(-?\d+(?:\.\d+)?)\}", value)
    if not m:
        return None
    return {"x": float(m.group(1)), "y": float(m.group(2)), "z": float(m.group(3))}


def parse_prefab_transforms(path: Path, guid_index: dict[str, str], sprites: dict[str, dict[str, Any]]) -> dict[str, Any]:
    text = read_text(path)
    names: dict[str, str] = {}
    transforms = []
    sprite_renderers = []
    for block in re.split(r"\n--- !u!", text):
        id_match = re.match(r"\d+\s*&(\d+)", block)
        file_id = id_match.group(1) if id_match else None
        if "GameObject:" in block and file_id:
            name = parse_scalar(block, "m_Name")
            if name:
                names[file_id] = name
    for block in re.split(r"\n--- !u!", text):
        if "Transform:" not in block and "RectTransform:" not in block:
            continue
        go = re.search(r"m_GameObject:\s*\{fileID:\s*(\d+)\}", block)
        pos = re.search(r"m_LocalPosition:\s*(\{.+?\})", block)
        scale = re.search(r"m_LocalScale:\s*(\{.+?\})", block)
        go_id = go.group(1) if go else None
        transforms.append(
            {
                "gameObjectId": go_id,
                "name": names.get(go_id or ""),
                "position": parse_vec3(pos.group(1)) if pos else None,
                "scale": parse_vec3(scale.group(1)) if scale else None,
            }
        )
    for block in re.split(r"\n--- !u!", text):
        if "SpriteRenderer:" not in block:
            continue
        go = re.search(r"m_GameObject:\s*\{fileID:\s*(\d+)\}", block)
        sprite = re.search(r"m_Sprite:\s*\{fileID:\s*21300000,\s*guid:\s*([0-9a-fA-F]{32}),", block)
        guid = sprite.group(1).lower() if sprite else None
        go_id = go.group(1) if go else None
        sprite_renderers.append(
            {
                "gameObjectId": go_id,
                "name": names.get(go_id or ""),
                "spriteGuid": guid,
                "spriteName": sprites.get(guid or "", {}).get("name"),
                "spritePath": sprites.get(guid or "", {}).get("path") or guid_index.get(guid or ""),
            }
        )
    interesting = [
        t
        for t in transforms
        if t.get("name")
        and any(
            token.lower() in str(t["name"]).lower()
            for token in (
                "view",
                "effect",
                "hit",
                "bound",
                "head",
                "projectile",
                "protjectile",
                "spawn",
                "prefab",
                "shadow",
                "background",
            )
        )
    ]
    return {
        "path": str(path),
        "objectCount": len(names),
        "transforms": interesting,
        "spriteRenderers": [s for s in sprite_renderers if s.get("spriteName")][:80],
    }


def path_for_resource(assets_dir: Path, resource_path: str, extension: str) -> Path | None:
    if not resource_path:
        return None
    name = Path(resource_path.replace("/", "\\")).name
    candidates = list(assets_dir.rglob(f"{name}{extension}"))
    return candidates[0] if candidates else None


def parse_stage_monsters(value: str) -> list[dict[str, int]]:
    result = []
    for part in value.split():
        if "_" not in part:
            continue
        key, weight = part.split("_", 1)
        if key.isdigit() and weight.isdigit():
            result.append({"monsterKey": int(key), "weight": int(weight)})
    return result


def associated_skills(hero_key: str, skills: list[dict[str, str]]) -> list[dict[str, str]]:
    if not hero_key:
        return []
    hero_family = hero_key[0]
    base_key = f"{hero_family}0001"
    prefix = hero_family
    out = []
    for skill in skills:
        key = skill.get("SkillKey", "")
        if key == base_key or (len(key) == 5 and key.startswith(prefix) and key.endswith("01")):
            out.append(skill)
    return out


def clip_by_name(clips: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {clip["name"]: clip for clip in clips.values()}


def compact_clip(clip: dict[str, Any] | None) -> dict[str, Any] | None:
    if not clip:
        return None
    return {
        "name": clip.get("name"),
        "path": clip.get("path"),
        "frameCount": clip.get("frameCount"),
        "estimatedDuration": clip.get("estimatedDuration"),
        "sizeRange": clip.get("sizeRange"),
        "texturePaths": clip.get("texturePaths"),
        "firstFrames": clip.get("frames", [])[:4],
    }


def extract_type_block(src: str, type_name: str) -> str | None:
    pattern = re.compile(rf"^(?:public|private|protected|internal).*?\b{re.escape(type_name)}\b.*$", re.MULTILINE)
    m = pattern.search(src)
    if not m:
        return None
    start = m.start()
    brace = src.find("{", m.end())
    if brace < 0:
        return None
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    return None


def il2cpp_summary(dump_cs: Path) -> dict[str, Any]:
    src = read_text(dump_cs)
    types = [
        "StageManager",
        "Unit",
        "Hero",
        "Monster",
        "ActiveSkill",
        "HeroActiveSkill",
        "MonsterActive",
        "ContinuousSkill",
        "bew",
        "HeroInfoData",
        "MonsterInfoData",
        "SkillInfoData",
        "StageInfoData",
        "BackGroundContainer",
        "HeroIdleSpriteData",
        "FollowCamera",
        "ObstacleScriptableObject",
    ]
    out: dict[str, Any] = {}
    for type_name in types:
        block = extract_type_block(src, type_name)
        if not block:
            continue
        fields = re.findall(r"^\s*(?:public|private|protected|internal).*?;\s*//\s*0x[0-9A-Fa-f]+", block, re.MULTILINE)
        methods = re.findall(r"^\s*(?:public|private|protected|internal).*?\)\s*\{\s*\}", block, re.MULTILINE)
        attrs = []
        for line in fields:
            if any(token in line for token in ("Transform", "Vector", "List<", "Dictionary", "Skill", "Hero", "Monster", "Stage", "Projectile", "Animator", "Speed", "Range", "Damage", "Position", "Camera", "BackGround")):
                attrs.append(line.strip())
        meth = []
        for line in methods:
            if any(token in line for token in ("Spawn", "Attack", "Damage", "Skill", "Projectile", "Target", "Move", "Destroy", "Range", "Unit", "Hero", "Monster")):
                meth.append(line.strip())
        out[type_name] = {
            "fieldCount": len(fields),
            "methodCount": len(methods),
            "relevantFields": attrs[:80],
            "relevantMethods": meth[:80],
        }
    for enum_name in ("bew.EProjectileTarget", "bew.ProjectileType"):
        block = extract_type_block(src, enum_name)
        if block:
            values = re.findall(r"public const .*? ([A-Za-z0-9_]+) = (\d+);", block)
            out[enum_name] = {"values": [{"name": n, "value": int(v)} for n, v in values]}
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", help="TaskBarHero version, e.g. 1.00.13")
    ap.add_argument("--manifest", type=Path, help="frontend-pack manifest path")
    ap.add_argument("--out-json", type=Path)
    ap.add_argument("--out-md", type=Path)
    args = ap.parse_args()

    manifest_path = args.manifest or find_latest_manifest(args.version)
    manifest = json.loads(read_text(manifest_path))
    version = manifest["gameVersion"]
    exported_project = Path(manifest["exportedProject"])
    frontend_pack = Path(manifest["frontendPack"])
    assets_dir = exported_project / "Assets"
    text_dir = assets_dir / "TextAsset"

    heroes = load_csv(text_dir / "HeroInfoData.txt")
    monsters = load_csv(text_dir / "MonsterInfoData.txt")
    skills = load_csv(text_dir / "SkillInfoData.txt")
    stages = load_csv(text_dir / "StageInfoData.txt")

    guid_index = index_guids(assets_dir)
    sprites = build_sprite_index(assets_dir, guid_index)
    clips = build_clip_index(assets_dir, guid_index, sprites)
    clips_by_name = clip_by_name(clips)

    unit_controller_path = assets_dir / "AnimatorController" / "UnitBaseAnimator.controller"
    unit_controller = parse_animator_controller(unit_controller_path, clips) if unit_controller_path.exists() else None

    hero_refs = []
    for hero in heroes:
        key = hero["HeroKey"]
        override_path = assets_dir / "AnimatorOverrideController" / f"Hero_{key}.overrideController"
        prefab_path = assets_dir / "GameObject" / f"Hero_{key}.prefab"
        hero_skills = associated_skills(key, skills)
        clip_names = [f"{key}_{suffix}" for suffix in ("Idle", "Walk", "Run", "Dead", "Landing", "ForceResurrection")]
        hero_refs.append(
            {
                "key": int(key),
                "classType": hero.get("ClassType"),
                "movementSpeed": int(hero.get("MovementSpeed") or 0),
                "attackSpeed": int(hero.get("AttackSpeed") or 0),
                "skillKey": int(hero.get("SkillKey") or 0),
                "prefabPathCsv": hero.get("PrefabPath"),
                "animatorPathCsv": hero.get("AnimatorPath"),
                "overrideController": parse_override_controller(override_path, guid_index, clips) if override_path.exists() else None,
                "clips": {name: compact_clip(clips_by_name.get(name)) for name in clip_names if clips_by_name.get(name)},
                "skills": [
                    {
                        "skillKey": int(s.get("SkillKey") or 0),
                        "activationType": s.get("ACTIVATIONTYPE"),
                        "activationValue": s.get("ActivationValue"),
                        "slotType": s.get("SLOTTYPE"),
                        "range": s.get("Range"),
                        "order": s.get("Order"),
                        "damageType": s.get("DamageType"),
                        "damageDeliveryType": s.get("DamageDeliveryType"),
                        "clips": [
                            compact_clip(clips_by_name.get(Path(s.get(f"AnimClipPath{i}", "")).name))
                            for i in (1, 2, 3)
                            if s.get(f"AnimClipPath{i}", "")
                        ],
                    }
                    for s in hero_skills
                ],
                "prefabAnchors": parse_prefab_transforms(prefab_path, guid_index, sprites) if prefab_path.exists() else None,
            }
        )

    skill_by_key = {s.get("SkillKey"): s for s in skills}
    monster_refs = []
    for monster in monsters:
        key = monster["MonsterKey"]
        override_path = assets_dir / "AnimatorOverrideController" / f"Monster_{key}.overrideController"
        prefab_path = assets_dir / "GameObject" / f"Monster_{key}.prefab"
        skill_keys = [part for part in monster.get("SkillKey", "").split() if part]
        monster_refs.append(
            {
                "key": int(key),
                "type": monster.get("MONSTERTYPE"),
                "movementSpeed": int(monster.get("MovementSpeed") or 0),
                "attackSpeed": int(monster.get("AttackSpeed") or 0),
                "maxLife": int(monster.get("MaxLife") or 0),
                "prefabPathCsv": monster.get("PrefabPath"),
                "animatorPathCsv": monster.get("AnimatorPath"),
                "skillKeys": [int(k) for k in skill_keys if k.isdigit()],
                "skills": [
                    {
                        "skillKey": int(k),
                        "damageDeliveryType": skill_by_key.get(k, {}).get("DamageDeliveryType"),
                        "range": skill_by_key.get(k, {}).get("Range"),
                        "clips": [
                            compact_clip(clips_by_name.get(Path(skill_by_key.get(k, {}).get(f"AnimClipPath{i}", "")).name))
                            for i in (1, 2, 3)
                            if skill_by_key.get(k, {}).get(f"AnimClipPath{i}", "")
                        ],
                    }
                    for k in skill_keys
                    if k in skill_by_key
                ],
                "overrideController": parse_override_controller(override_path, guid_index, clips) if override_path.exists() else None,
                "prefabAnchors": parse_prefab_transforms(prefab_path, guid_index, sprites) if prefab_path.exists() else None,
            }
        )

    stage_refs = []
    for stage in stages:
        stage_refs.append(
            {
                "stageKey": int(stage.get("StageKey") or 0),
                "type": stage.get("STAGETYPE"),
                "difficulty": stage.get("STAGEDIFFICULITY"),
                "act": int(stage.get("Act") or 0),
                "stageNo": int(stage.get("StageNo") or 0),
                "waveAmount": int(stage.get("WaveAmount") or 0),
                "waveMonsterAmount": int(stage.get("WaveMonsterAmount") or 0),
                "monsters": parse_stage_monsters(stage.get("Monsters", "")),
                "bossMonsterKey": int(stage.get("BossMonsterKey") or 0),
                "bossScale": float(stage.get("BossScale") or 0),
            }
        )

    background_refs = {}
    for stage_key in ("1101", "1102", "1110", "1201", "1210"):
        layers = []
        for layer in range(3):
            path = assets_dir / "GameObject" / f"StageBackground_Layer{layer}_{stage_key}.prefab"
            if path.exists():
                layers.append({"layer": layer, **parse_prefab_transforms(path, guid_index, sprites)})
        if layers:
            background_refs[stage_key] = layers

    image_counts = Counter(item.get("bucket", "") for item in manifest.get("images", []))
    data = {
        "metadata": {
            "gameVersion": version,
            "generatedAt": datetime.now(timezone.utc).isoformat(),
            "manifest": str(manifest_path),
            "exportedProject": str(exported_project),
            "frontendPack": str(frontend_pack),
            "dumpCs": str(DUMP_DIR / "dump.cs"),
            "scriptJson": str(DUMP_DIR / "script.json"),
        },
        "counts": {
            "manifest": manifest.get("counts"),
            "imageBuckets": dict(sorted(image_counts.items())),
            "heroes": len(hero_refs),
            "monsters": len(monster_refs),
            "skills": len(skills),
            "stages": len(stage_refs),
            "sprites": len(sprites),
            "animationClips": len(clips),
            "guidIndex": len(guid_index),
        },
        "unitBaseAnimator": unit_controller,
        "heroes": hero_refs,
        "monsters": monster_refs,
        "stages": stage_refs,
        "stageBackgroundSamples": background_refs,
        "il2cpp": il2cpp_summary(DUMP_DIR / "dump.cs") if (DUMP_DIR / "dump.cs").exists() else {},
    }

    out_json = args.out_json or ROOT / "docs" / f"battle-reference-{version}.json"
    out_md = args.out_md or ROOT / "docs" / f"battle-reference-{version}.md"
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(data, indent=2, ensure_ascii=True), encoding="utf-8")

    stage_1101 = next((s for s in stage_refs if s["stageKey"] == 1101), None)
    lines = [
        f"# Battle reference - TaskBarHero {version}",
        "",
        "Generated from static, read-only sources:",
        "",
        f"- AssetRipper ExportedProject: `{exported_project}`",
        f"- Frontend pack manifest: `{manifest_path}`",
        f"- Il2CppDumper dump: `{DUMP_DIR / 'dump.cs'}`",
        "",
        "## Routines run",
        "",
        "- `py scripts\\refresh_il2cpp_map.py --dry-run --no-live`",
        "- `powershell -NoProfile -ExecutionPolicy Bypass -File .\\run-release.ps1 -SkipAgentRelease -SkipFrontend`",
        f"- `py scripts\\extract_battle_reference.py --version {version}`",
        "",
        "## Counts",
        "",
        f"- Images in frontend-pack: {manifest.get('counts', {}).get('images')}",
        f"- Audio clips in frontend-pack: {manifest.get('counts', {}).get('audio')}",
        f"- Contact sheets: {manifest.get('counts', {}).get('contactSheets')}",
        f"- Parsed sprites: {len(sprites)}",
        f"- Parsed AnimationClips: {len(clips)}",
        f"- Heroes: {len(hero_refs)}",
        f"- Monsters: {len(monster_refs)}",
        f"- Skills: {len(skills)}",
        f"- Stages: {len(stage_refs)}",
        "",
        "## Battle model facts",
        "",
        "- `HeroInfoData.txt` links each hero to `Prefab/Hero/Hero_<key>` and `Animation/Hero/<key>/Hero_<key>`.",
        "- `MonsterInfoData.txt` links each monster to `Prefab/Monster/Monster_<key>` and `Animation/Monster/<key>/Monster_<key>`.",
        "- `SkillInfoData.txt` is the authoritative source for activation type, range, order, damage type, delivery type, and `Animation/Skill/Skill_*` clips.",
        "- `StageInfoData.txt` is the authoritative source for weighted stage monsters and boss monster keys.",
        "- Units use `AnimatorController/UnitBaseAnimator.controller`; per-unit variation is applied through `AnimatorOverrideController/Hero_<key>.overrideController` and `Monster_<key>.overrideController`.",
        "- Hero prefabs include named anchors such as `View`, `EffectPosition`, `HitEffectPosition`, `BoundCenterPosition_AutoSet`, `BoundBottomPosition_AutoSet`, `HeadUpPosition_AutoSet`, `ProtjectileFirePosition`, `ProtjectileTargetPos_1`, `ProtjectileTargetPos_2`, `SpawnTurretPos`, and `PrefabPosition`.",
        "- Do not flip sprites by assumption. Use the prefab `View` scale/position and the extracted sprite frames as the default orientation reference.",
        "- Method bodies in the AssetRipper C# export are stubs for IL2CPP. Exact runtime decisions such as target selection and movement timing still require native decompilation in Ghidra/IDA using the regenerated `dump.cs`/`script.json` names.",
        "",
        "## UnitBaseAnimator states",
        "",
    ]
    if unit_controller:
        state_names = [s["name"] for s in unit_controller["states"] if s.get("name")]
        lines.append(", ".join(state_names[:80]))
        lines.append("")
    lines.extend(["## Heroes", ""])
    for hero in hero_refs:
        clip_bits = ", ".join(f"{name}:{clip['frameCount']}f/{clip['estimatedDuration']}s" for name, clip in hero["clips"].items())
        lines.append(f"- Hero {hero['key']} `{hero['classType']}` speed={hero['movementSpeed']} attackSpeed={hero['attackSpeed']} clips: {clip_bits}")
    lines.extend(["", "## Stage 1101 sample", ""])
    if stage_1101:
        lines.append(f"- waves={stage_1101['waveAmount']} waveMonsterAmount={stage_1101['waveMonsterAmount']} boss={stage_1101['bossMonsterKey']} bossScale={stage_1101['bossScale']}")
        lines.append(f"- weighted monsters: {stage_1101['monsters']}")
    lines.extend(["", "## Output", "", f"- JSON reference: `{out_json}`"])
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"Wrote {out_json}")
    print(f"Wrote {out_md}")
    print(json.dumps(data["counts"], indent=2, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
