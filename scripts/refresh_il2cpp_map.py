#!/usr/bin/env python3
"""Recalcula o mapa IL2CPP do TaskBarHero e atualiza o codigo automaticamente.

Quando sai uma versao nova do jogo, os RVAs/offsets que o agente usa para ler a
List<LogData> do LogManager mudam. Este script:

  1. Localiza a instalacao do jogo (Steam) e a versao.
  2. Baixa/usa o Il2CppDumper e gera o dump (dump.cs + script.json).
  3. Extrai do dump TODOS os valores que o agente precisa:
       - RVA do TypeInfo do singleton base de LogManager
       - offsets de LogManager (List<LogData>) e LogData (texto/relogio/DateTime)
       - offsets de BoxOpenLog (itemStringKey + EGradeType)
      - TypeInfos/offsets de StageManager, MonsterSpawnManager, MonsterInfoData e runtime currency para diagnostico runtime
       - o enum EGradeType (nomes das raridades, em ordem)
  4. VERIFICA na memoria do jogo vivo (rotina de leitura dos logs de eventos):
     resolve a cadeia de ponteiros, descobre o offset de static_fields por forca
     bruta, confirma quais offsets dao texto/relogio/DateTime validos e mostra
     uma amostra dos ultimos eventos com categoria/raridade.
  5. Atualiza, entre marcadores:
       - src/main.cpp                       (bloco IL2CPP MAP + kGradeNames)
       - ../tbh-farm-local-frontend/server.js          (gradePt)
       - ../tbh-farm-local-frontend/public/app/history-domain.js (HISTORY_GRADE_PT)

Uso:
  py scripts/refresh_il2cpp_map.py [--game-dir DIR] [--no-live] [--dry-run]

Depois e so: build.bat  e subir o frontend.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import io
import json
import os
import re
import struct
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
AGENT_DIR = SCRIPT_DIR.parent
REPO_ROOT = AGENT_DIR.parent
MAIN_CPP = AGENT_DIR / "src" / "main.cpp"
SERVER_JS = REPO_ROOT / "tbh-farm-local-frontend" / "server.js"
HISTORY_DOMAIN_JS = REPO_ROOT / "tbh-farm-local-frontend" / "public" / "app" / "history-domain.js"
CACHE_DIR = SCRIPT_DIR / ".cache"

PROCESS_NAME = "TaskBarHero.exe"
MODULE_NAME = "GameAssembly.dll"

# EGradeType (canonico, em ingles) -> PT-BR oficial do jogo. Mantido aqui para
# regenerar os mapas do frontend a partir do enum. Se uma versao nova adicionar
# um grade, o script avisa para voce preencher a traducao.
GRADE_PT = {
    "COMMON": "Comum",
    "UNCOMMON": "Incomum",
    "RARE": "Raro",
    "LEGENDARY": "Lendário",
    "IMMORTAL": "Imortal",
    "ARCANA": "Arcano",
    "BEYOND": "Além",
    "CELESTIAL": "Celestial",
    "DIVINE": "Divino",
    "COSMIC": "Cósmico",
    "NONE": None,  # ignorado (sem raridade)
}

DUMPER_API = "https://api.github.com/repos/Perfare/Il2CppDumper/releases/latest"


# --------------------------------------------------------------------------- #
# Localizacao do jogo
# --------------------------------------------------------------------------- #
def find_game_dir(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if (p / MODULE_NAME).exists():
            return p
        raise SystemExit(f"--game-dir invalido (sem {MODULE_NAME}): {p}")

    candidates: list[Path] = []
    # Steam: lê libraryfolders.vdf para achar as bibliotecas instaladas.
    for steam in (
        Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Steam",
        Path("C:/Program Files/Steam"),
    ):
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            for m in re.finditer(r'"path"\s*"([^"]+)"', vdf.read_text(encoding="utf-8", errors="ignore")):
                candidates.append(Path(m.group(1).replace("\\\\", "\\")) / "steamapps" / "common" / "TaskbarHero")
    # Caminhos comuns de fallback.
    for drive in "CDEFGH":
        candidates.append(Path(f"{drive}:/SteamLibrary/steamapps/common/TaskbarHero"))
        candidates.append(Path(f"{drive}:/Program Files (x86)/Steam/steamapps/common/TaskbarHero"))

    for c in candidates:
        if (c / MODULE_NAME).exists():
            return c
    raise SystemExit(
        "Nao encontrei a instalacao do TaskBarHero. Passe --game-dir \"caminho\\TaskbarHero\"."
    )


def game_version(game_dir: Path) -> str:
    vt = game_dir / "Version.txt"
    return vt.read_text(encoding="utf-8", errors="ignore").strip() if vt.exists() else "desconhecida"


# --------------------------------------------------------------------------- #
# Il2CppDumper
# --------------------------------------------------------------------------- #
def ensure_dumper() -> Path:
    exe = CACHE_DIR / "il2cppdumper" / "Il2CppDumper.exe"
    if exe.exists():
        return exe
    print("[*] Baixando Il2CppDumper (ultima release)...")
    rel = json.loads(urllib.request.urlopen(DUMPER_API, timeout=60).read())
    asset = next((a for a in rel["assets"] if re.search(r"net6.*win", a["name"], re.I)), None)
    asset = asset or next((a for a in rel["assets"] if a["name"].endswith(".zip")), None)
    if not asset:
        raise SystemExit("Nao achei um asset .zip na release do Il2CppDumper.")
    data = urllib.request.urlopen(asset["browser_download_url"], timeout=300).read()
    dst = exe.parent
    dst.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(dst)
    if not exe.exists():
        raise SystemExit("Il2CppDumper.exe nao encontrado apos extrair o zip.")
    print(f"    Il2CppDumper {rel.get('tag_name','')} pronto.")
    return exe


def run_dumper(dumper: Path, game_dir: Path) -> Path:
    out = CACHE_DIR / "dump"
    out.mkdir(parents=True, exist_ok=True)
    meta = game_dir / "TaskBarHero_Data" / "il2cpp_data" / "Metadata" / "global-metadata.dat"
    asm = game_dir / MODULE_NAME
    print("[*] Rodando Il2CppDumper (pode levar ~1 min)...")
    # O Il2CppDumper espera um Enter no fim; manda stdin vazio e ignora a excecao
    # de "Cannot read keys" — os arquivos ja foram gerados antes disso.
    proc = subprocess.run(
        [str(dumper), str(asm), str(meta), str(out)],
        input="\n", capture_output=True, text=True, timeout=600,
    )
    if not (out / "dump.cs").exists() or not (out / "script.json").exists():
        sys.stderr.write(proc.stdout + "\n" + proc.stderr + "\n")
        raise SystemExit("Il2CppDumper nao gerou dump.cs/script.json.")
    print("    Dump gerado.")
    return out


# --------------------------------------------------------------------------- #
# Parsing do dump
# --------------------------------------------------------------------------- #
def class_body(src: str, name: str) -> str | None:
    m = re.search(
        r"// Namespace:[^\n]*\n(?:\[[^\n]*\]\n)*(?:public|private) "
        r"(?:(?:abstract|sealed|static)\s+)*class " + re.escape(name) + r"(?=\s|:).*?\n\}",
        src, re.S,
    )
    return m.group(0) if m else None


def iter_class_bodies(src: str) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    pattern = re.compile(
        r"// Namespace:[^\n]*\n(?:\[[^\n]*\]\n)*(?:public|private) "
        r"(?:(?:abstract|sealed|static)\s+)*class\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\b.*?\n\}",
        re.S,
    )
    for match in pattern.finditer(src):
        out.append((match.group(1), match.group(0)))
    return out


def fields_of(body: str, include_static: bool = False) -> list[tuple[str, str, int]]:
    """Retorna [(tipo, nome, offset)] dos campos de instancia (com // 0xNN)."""
    out = []
    in_fields = False
    for line in body.splitlines():
        s = line.strip()
        if s == "// Fields":
            in_fields = True
            continue
        if s.startswith("// Properties") or s.startswith("// Methods"):
            break
        if not in_fields:
            continue
        m = re.match(r"(?:\[[^\]]*\]\s*)*((?:private|public|protected|internal).*?);\s*//\s*(0x[0-9A-Fa-f]+)", s)
        if not m:
            continue
        decl, off = m.group(1), int(m.group(2), 16)
        if (not include_static and " static " in f" {decl} ") or "const " in decl:
            continue
        parts = decl.replace("readonly ", "").split()
        # parts: [modifier, ..., TYPE..., NAME]. Tipos genericos podem conter
        # espaco depois da virgula, ex.: Dictionary<int, vd>.
        if len(parts) < 2:
            continue
        fname = parts[-1]
        modifiers = {"private", "public", "protected", "internal", "static", "readonly"}
        type_parts = [part for part in parts[:-1] if part not in modifiers]
        if not type_parts:
            continue
        ftype = " ".join(type_parts)
        out.append((ftype, fname, off))
    return out


def parse_enum(src: str, name: str) -> list[str]:
    m = re.search(r"public enum " + re.escape(name) + r"\b.*?\n\}", src, re.S)
    if not m:
        raise SystemExit(f"enum {name} nao encontrado no dump.")
    pairs = {}
    for em in re.finditer(r"public const " + re.escape(name) + r"\s+(\w+)\s*=\s*(\d+);", m.group(0)):
        pairs[int(em.group(2))] = em.group(1)
    return [pairs[i] for i in range(max(pairs) + 1) if i in pairs]


def singleton_typeinfo_candidates(class_body_text: str, class_name: str, fallback_names: list[str] | None = None) -> list[str]:
    candidates: list[str] = []
    header = next((line for line in class_body_text.splitlines() if f"class {class_name}" in line), "")
    m = re.search(r"\bclass\s+" + re.escape(class_name) + r"\s*:\s*([A-Za-z_]\w*)<" + re.escape(class_name) + r">", header)
    if m:
        candidates.append(f"{m.group(1)}<{class_name}>_TypeInfo")

    candidates.extend([f"np<{class_name}>_TypeInfo", f"nn<{class_name}>_TypeInfo", f"{class_name}_TypeInfo"])
    if fallback_names:
        candidates.extend(fallback_names)

    deduped: list[str] = []
    for candidate in candidates:
        if candidate not in deduped:
            deduped.append(candidate)
    return deduped


def find_typeinfo_rva(script_json: Path, names: list[str], class_name: str) -> tuple[int, str]:
    data = json.loads(script_json.read_text(encoding="utf-8"))
    metadata = data.get("ScriptMetadata", [])
    by_name = {str(entry.get("Name")): entry for entry in metadata}
    for name in names:
        entry = by_name.get(name)
        if entry:
            return int(entry["Address"]), str(entry["Name"])

    for entry in data.get("ScriptMetadata", []):
        entry_name = str(entry.get("Name", ""))
        if entry_name.endswith(f"<{class_name}>_TypeInfo") or entry_name == f"{class_name}_TypeInfo":
            return int(entry["Address"]), entry_name

    raise SystemExit(
        f"TypeInfo de {class_name} nao encontrado em script.json (ScriptMetadata). "
        f"Candidatos: {', '.join(names)}"
    )


def require_class(src: str, name: str) -> str:
    body = class_body(src, name)
    if not body:
        raise SystemExit(f"classe {name} nao encontrada.")
    return body


def field_offset(body: str, name: str, class_name: str) -> int:
    for _typ, fname, off in fields_of(body):
        if fname == name:
            return off
    raise SystemExit(f"campo {class_name}.{name} nao encontrado no dump.")


def field_offset_any_name(body: str, names: list[str], class_name: str) -> int | None:
    for typ, fname, off in fields_of(body):
        if fname in names:
            return off
    return None


def field_offset_by_type(body: str, type_name: str, class_name: str) -> int:
    matches = [(fname, off) for typ, fname, off in fields_of(body) if typ == type_name]
    if not matches:
        raise SystemExit(f"campo {class_name} com tipo {type_name} nao encontrado no dump.")
    if len(matches) > 1:
        names = ", ".join(f"{fname}@0x{off:X}" for fname, off in matches)
        raise SystemExit(f"campo {class_name} com tipo {type_name} ambiguo no dump: {names}")
    return matches[0][1]


def field_offsets_by_type(body: str, type_name: str) -> list[int]:
    return [off for typ, _fname, off in fields_of(body) if typ == type_name]


def field_offset_name_or_type(body: str, name: str, type_name: str, class_name: str) -> int:
    try:
        return field_offset(body, name, class_name)
    except SystemExit:
        return field_offset_by_type(body, type_name, class_name)


def is_singleton_class(body: str, class_name: str) -> bool:
    header = body.split("{", 1)[0]
    return bool(re.search(r"\bclass\s+" + re.escape(class_name) + r"\s*:\s*[A-Za-z_]\w*<" + re.escape(class_name) + r">", header))


def find_save_manager_class(src: str) -> tuple[str, str]:
    candidates: list[tuple[str, str]] = []
    for class_name, body in iter_class_bodies(src):
        fields = fields_of(body)
        account_offsets = [off for typ, _fname, off in fields if typ == "AccountSaveData"]
        player_offsets = [off for typ, _fname, off in fields if typ == "PlayerSaveData"]
        if len(account_offsets) == 1 and len(player_offsets) == 1:
            candidates.append((class_name, body))

    singleton_candidates = [(name, body) for name, body in candidates if is_singleton_class(body, name)]
    if len(singleton_candidates) == 1:
        return singleton_candidates[0]
    if len(candidates) == 1:
        return candidates[0]

    details = ", ".join(name for name, _body in (singleton_candidates or candidates))
    if details:
        raise SystemExit(f"classe save manager ambigua no dump: {details}")
    raise SystemExit("classe save manager com AccountSaveData + PlayerSaveData nao encontrada no dump.")


def find_class_by_predicate(src: str, description: str, predicate) -> tuple[str, str]:
    matches = [(name, body) for name, body in iter_class_bodies(src) if predicate(name, body, fields_of(body))]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit(f"classe {description} nao encontrada no dump.")
    details = ", ".join(name for name, _body in matches)
    raise SystemExit(f"classe {description} ambigua no dump: {details}")


def find_static_owner_by_field_type(src: str, field_type: str, description: str) -> tuple[str, str]:
    return find_class_by_predicate(
        src,
        description,
        lambda _name, body, _fields: any(typ == field_type for typ, _fname, _off in fields_of(body, include_static=True)),
    )


def find_monster_cache_class(src: str) -> tuple[str, str]:
    return find_class_by_predicate(
        src,
        "cache runtime de MonsterInfoData",
        lambda _name, _body, fields: any(typ == "MonsterInfoData" for typ, _fname, _off in fields)
        and sum(1 for typ, _fname, _off in fields if typ == "ObscuredFloat") >= 3,
    )


def find_runtime_currency_classes(src: str) -> tuple[str, str, str, str]:
    runtime_name, runtime_body = find_class_by_predicate(
        src,
        "runtime currency",
        lambda _name, _body, fields: any(typ == "CurrencyInfoData" for typ, _fname, _off in fields)
        and any(typ == "ObscuredLong" for typ, _fname, _off in fields)
        and any(typ == "ObscuredInt" for typ, _fname, _off in fields),
    )
    manager_name, manager_body = find_static_owner_by_field_type(src, f"List<{runtime_name}>", "manager runtime currency")
    return manager_name, manager_body, runtime_name, runtime_body


def find_runtime_hero_classes(src: str) -> tuple[str, str, str, str]:
    runtime_name, runtime_body = find_class_by_predicate(
        src,
        "runtime hero",
        lambda _name, _body, fields: any(typ == "HeroInfoData" for typ, _fname, _off in fields)
        and sum(1 for typ, _fname, _off in fields if typ == "ObscuredInt") >= 4
        and any(typ == "ObscuredFloat" for typ, _fname, _off in fields),
    )
    manager_name, manager_body = find_static_owner_by_field_type(src, f"Dictionary<int, {runtime_name}>", "manager runtime hero")
    return manager_name, manager_body, runtime_name, runtime_body


def stage_manager_runtime_offsets(body: str) -> dict[str, int]:
    fields = fields_of(body)

    runtime_float = field_offset_any_name(body, ["bdha", "bdhx"], "StageManager")
    if runtime_float is None:
        cts_offsets = [off for typ, _fname, off in fields if typ == "CancellationTokenSource"]
        if not cts_offsets:
            raise SystemExit("campo StageManager CancellationTokenSource nao encontrado para inferir runtime float.")
        cts_offset = min(cts_offsets)
        candidates = [(fname, off) for typ, fname, off in fields if typ == "float" and off < cts_offset]
        if not candidates:
            raise SystemExit("campo StageManager runtime float nao encontrado por heuristica.")
        runtime_float = max(candidates, key=lambda item: item[1])[1]

    runtime_int = field_offset_any_name(body, ["bdhc", "bdhz"], "StageManager")
    if runtime_int is None:
        opening_offsets = [off for typ, _fname, off in fields if typ == "OpeningDirection"]
        if not opening_offsets:
            raise SystemExit("campo StageManager OpeningDirection nao encontrado para inferir runtime int.")
        opening_offset = max(opening_offsets)
        candidates = [(fname, off) for typ, fname, off in fields if typ == "int" and off > opening_offset]
        if not candidates:
            raise SystemExit("campo StageManager runtime int nao encontrado por heuristica.")
        runtime_int = min(candidates, key=lambda item: item[1])[1]

    list_a = field_offset_any_name(body, ["bdgx", "bdhu"], "StageManager")
    list_b = field_offset_any_name(body, ["bdgy", "bdhv"], "StageManager")
    if list_a is None or list_b is None:
        list_offsets = [off for typ, _fname, off in fields if typ == "List<bn>"]
        if len(list_offsets) < 2:
            raise SystemExit("campos StageManager List<bn> insuficientes para inferir listas runtime.")
        list_a, list_b = sorted(list_offsets)[:2]

    return {
        "float": runtime_float,
        "int": runtime_int,
        "list_a": list_a,
        "list_b": list_b,
    }


def monster_runtime_offsets(body: str) -> dict[str, int]:
    fields = fields_of(body)
    monster_type = field_offset(body, "MonsterType", "Monster")
    tail = [(typ, fname, off) for typ, fname, off in fields if off > monster_type]
    expected = ["int", "int", "float", "EStageType", "int"]
    found: list[int] = []
    start = 0
    for expected_type in expected:
        match_index = next((i for i in range(start, len(tail)) if tail[i][0] == expected_type), None)
        if match_index is None:
            raise SystemExit(f"Monster nao tem sequencia runtime esperada apos MonsterType: {expected}")
        found.append(tail[match_index][2])
        start = match_index + 1
    return {
        "runtime_int_a": found[0],
        "runtime_int_b": found[1],
        "runtime_float": found[2],
        "stage_type": found[3],
        "runtime_int_c": found[4],
    }


def static_field_offset_by_type(body: str, type_name: str, class_name: str) -> int:
    matches = [(fname, off) for typ, fname, off in fields_of(body, include_static=True) if typ == type_name]
    if not matches:
        raise SystemExit(f"campo static {class_name} com tipo {type_name} nao encontrado no dump.")
    if len(matches) > 1:
        names = ", ".join(f"{fname}@0x{off:X}" for fname, off in matches)
        raise SystemExit(f"campo static {class_name} com tipo {type_name} ambiguo no dump: {names}")
    return matches[0][1]


def extract_map(dump_dir: Path) -> dict:
    src = (dump_dir / "dump.cs").read_text(encoding="utf-8", errors="ignore")
    info: dict = {}

    # LogManager: primeiro campo List<LogData> = log completo em ordem de insercao.
    lm = class_body(src, "LogManager")
    if not lm:
        raise SystemExit("classe LogManager nao encontrada.")
    info["typeinfo_rva"], info["typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        singleton_typeinfo_candidates(lm, "LogManager", ["TaskbarHero.Log.LogManager_TypeInfo", "LogManager_TypeInfo"]),
        "LogManager",
    )
    lists = [off for (t, _n, off) in fields_of(lm) if t == "List<LogData>"]
    if not lists:
        raise SystemExit("LogManager nao tem campo List<LogData>.")
    info["list_offsets"] = sorted(lists)  # candidatos (verificados ao vivo)

    # LogData: strings (texto/relogio) + DateTime.
    ld = class_body(src, "LogData")
    if not ld:
        raise SystemExit("classe LogData nao encontrada.")
    ld_fields = fields_of(ld)
    info["string_offsets"] = sorted(off for (t, _n, off) in ld_fields if t == "string")
    dt = [off for (t, _n, off) in ld_fields if t == "DateTime"]
    if not dt:
        raise SystemExit("LogData nao tem campo DateTime.")
    info["datetime_offset"] = dt[0]

    # BoxOpenLog: itemStringKey (string) + EGradeType.
    box = class_body(src, "BoxOpenLog")
    if not box:
        raise SystemExit("classe BoxOpenLog nao encontrada.")
    box_fields = fields_of(box)
    item_key = [off for (t, _n, off) in box_fields if t == "string"]
    grade = [off for (t, _n, off) in box_fields if t == "EGradeType"]
    if not item_key or not grade:
        raise SystemExit("BoxOpenLog sem campo string/EGradeType esperado.")
    info["box_item_key_offset"] = item_key[0]
    info["box_grade_offset"] = grade[0]

    # Enum de raridade.
    grades = parse_enum(src, "EGradeType")
    info["grade_names"] = [g for g in grades if g != "NONE"]

    save_manager_name, save_manager = find_save_manager_class(src)
    info["save_manager_class_name"] = save_manager_name
    info["save_typeinfo_rva"], info["save_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        singleton_typeinfo_candidates(save_manager, save_manager_name, [f"{save_manager_name}_TypeInfo", "bal_TypeInfo"]),
        save_manager_name,
    )
    info["save_manager_account_offset"] = field_offset_name_or_type(save_manager, "bgaw", "AccountSaveData", save_manager_name)
    info["save_manager_player_offset"] = field_offset_name_or_type(save_manager, "bgax", "PlayerSaveData", save_manager_name)

    account = require_class(src, "AccountSaveData")
    common = require_class(src, "CommonSaveData")
    player = require_class(src, "PlayerSaveData")
    info["account_player_id_offset"] = field_offset(account, "<playerId>k__BackingField", "AccountSaveData")
    info["account_version_offset"] = field_offset(account, "version", "AccountSaveData")
    info["account_owner_steam_id_offset"] = field_offset(account, "ownerSteamId", "AccountSaveData")
    info["common_version_offset"] = field_offset(common, "version", "CommonSaveData")
    info["common_play_time_offset"] = field_offset(common, "playTime", "CommonSaveData")
    info["common_arranged_pet_key_offset"] = field_offset(common, "ArrangedPetKey", "CommonSaveData")
    info["common_arranged_hero_key_offset"] = field_offset(common, "arrangedHeroKey", "CommonSaveData")
    info["common_max_completed_stage_offset"] = field_offset(common, "maxCompletedStage", "CommonSaveData")
    info["common_current_stage_key_offset"] = field_offset(common, "currentStageKey", "CommonSaveData")
    info["common_current_stage_wave_offset"] = field_offset(common, "currentStageWave", "CommonSaveData")
    info["player_common_offset"] = field_offset(player, "commonSaveData", "PlayerSaveData")
    info["player_currencies_offset"] = field_offset(player, "currenySaveDatas", "PlayerSaveData")
    info["player_heroes_offset"] = field_offset(player, "heroSaveDatas", "PlayerSaveData")
    info["player_attributes_offset"] = field_offset(player, "attributeSaveDatas", "PlayerSaveData")
    info["player_pets_offset"] = field_offset(player, "PetSaveData", "PlayerSaveData")
    info["player_runes_offset"] = field_offset(player, "RuneSaveData", "PlayerSaveData")
    info["player_inventory_offset"] = field_offset(player, "inventorySaveDatas", "PlayerSaveData")
    info["player_stash_offset"] = field_offset(player, "stashSaveDatas", "PlayerSaveData")
    info["player_trade_stash_offset"] = field_offset(player, "tradingStashSaveDatas", "PlayerSaveData")
    info["player_items_offset"] = field_offset(player, "itemSaveDatas", "PlayerSaveData")
    info["player_aggregates_offset"] = field_offset(player, "aggregateSaveDatas", "PlayerSaveData")

    stage_manager = require_class(src, "StageManager")
    monster_spawn_manager = require_class(src, "MonsterSpawnManager")
    monster = require_class(src, "Monster")
    monster_cache_name, monster_cache = find_monster_cache_class(src)
    monster_info = require_class(src, "MonsterInfoData")
    runtime_currency_manager_name, runtime_currency_manager, runtime_currency_name, runtime_currency = find_runtime_currency_classes(src)
    currency_info = require_class(src, "CurrencyInfoData")
    runtime_hero_manager_name, runtime_hero_manager, runtime_hero_name, runtime_hero = find_runtime_hero_classes(src)
    hero_info = require_class(src, "HeroInfoData")
    runtime_backend_inventory_name, runtime_backend_inventory = find_static_owner_by_field_type(src, "List<InventoryItemData>", "backend inventory runtime")
    backend_inventory_item = require_class(src, "InventoryItemData")
    info["stage_manager_typeinfo_rva"], info["stage_manager_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        singleton_typeinfo_candidates(stage_manager, "StageManager", ["np<StageManager>_TypeInfo"]),
        "StageManager",
    )
    info["monster_spawn_manager_typeinfo_rva"], info["monster_spawn_manager_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        singleton_typeinfo_candidates(monster_spawn_manager, "MonsterSpawnManager", ["np<MonsterSpawnManager>_TypeInfo"]),
        "MonsterSpawnManager",
    )
    info["stage_manager_stage_state_offset"] = field_offset(stage_manager, "stageState", "StageManager")
    info["stage_manager_stage_started_offset"] = field_offset(stage_manager, "b_StageStart", "StageManager")
    stage_runtime = stage_manager_runtime_offsets(stage_manager)
    info["stage_manager_runtime_float_offset"] = stage_runtime["float"]
    info["stage_manager_runtime_int_offset"] = stage_runtime["int"]
    info["stage_manager_runtime_list_a_offset"] = stage_runtime["list_a"]
    info["stage_manager_runtime_list_b_offset"] = stage_runtime["list_b"]
    info["monster_spawn_manager_monster_list_offset"] = field_offset(monster_spawn_manager, "MonsterList", "MonsterSpawnManager")
    info["monster_spawn_manager_dead_monster_list_offset"] = field_offset(monster_spawn_manager, "DeadMonsterUnit", "MonsterSpawnManager")
    info["monster_spawn_manager_summoned_monster_list_offset"] = field_offset(monster_spawn_manager, "SummonedMonsterList", "MonsterSpawnManager")
    info["monster_spawn_manager_force_boss_wave_offset"] = field_offset(monster_spawn_manager, "IsForceEnterBossWave", "MonsterSpawnManager")
    info["monster_cache_offset"] = field_offset(monster, "cache", "Monster")
    info["monster_type_offset"] = field_offset(monster, "MonsterType", "Monster")
    runtime_monster = monster_runtime_offsets(monster)
    info["monster_runtime_int_a_offset"] = runtime_monster["runtime_int_a"]
    info["monster_runtime_int_b_offset"] = runtime_monster["runtime_int_b"]
    info["monster_runtime_float_offset"] = runtime_monster["runtime_float"]
    info["monster_stage_type_offset"] = runtime_monster["stage_type"]
    info["monster_runtime_int_c_offset"] = runtime_monster["runtime_int_c"]
    info["monster_cache_class_name"] = monster_cache_name
    info["monster_cache_info_data_offset"] = field_offset_by_type(monster_cache, "MonsterInfoData", monster_cache_name)
    info["monster_info_monster_key_offset"] = field_offset(monster_info, "MonsterKey", "MonsterInfoData")
    info["monster_info_monster_type_offset"] = field_offset(monster_info, "MONSTERTYPE", "MonsterInfoData")
    info["monster_info_reward_gold_offset"] = field_offset(monster_info, "RewardGold", "MonsterInfoData")
    info["monster_info_reward_exp_offset"] = field_offset(monster_info, "RewardExp", "MonsterInfoData")
    info["runtime_currency_manager_typeinfo_rva"], info["runtime_currency_manager_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        [f"{runtime_currency_manager_name}_TypeInfo"],
        runtime_currency_manager_name,
    )
    info["runtime_currency_manager_list_offset"] = static_field_offset_by_type(
        runtime_currency_manager,
        f"List<{runtime_currency_name}>",
        runtime_currency_manager_name,
    )
    info["runtime_currency_info_offset"] = field_offset_by_type(
        runtime_currency,
        "CurrencyInfoData",
        runtime_currency_name,
    )
    info["runtime_currency_amount_offset"] = field_offset_by_type(
        runtime_currency,
        "ObscuredLong",
        runtime_currency_name,
    )
    info["runtime_currency_alt_amount_offset"] = field_offset_by_type(
        runtime_currency,
        "ObscuredInt",
        runtime_currency_name,
    )
    info["currency_info_key_offset"] = field_offset(currency_info, "CurrencyKey", "CurrencyInfoData")
    info["runtime_hero_manager_typeinfo_rva"], info["runtime_hero_manager_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        [f"{runtime_hero_manager_name}_TypeInfo"],
        runtime_hero_manager_name,
    )
    info["runtime_hero_class_name"] = runtime_hero_name
    info["runtime_hero_dictionary_offset"] = static_field_offset_by_type(runtime_hero_manager, f"Dictionary<int, {runtime_hero_name}>", runtime_hero_manager_name)
    info["runtime_hero_info_offset"] = field_offset_by_type(runtime_hero, "HeroInfoData", runtime_hero_name)
    runtime_hero_int_offsets = field_offsets_by_type(runtime_hero, "ObscuredInt")
    if len(runtime_hero_int_offsets) < 4:
        raise SystemExit(f"classe {runtime_hero_name} sem os quatro ObscuredInt esperados para level/ability points.")
    info["runtime_hero_level_offset"] = runtime_hero_int_offsets[0]
    info["runtime_hero_ability_point_offset"] = runtime_hero_int_offsets[2]
    info["runtime_hero_allocated_ability_point_offset"] = runtime_hero_int_offsets[3]
    info["runtime_hero_exp_offset"] = field_offset_by_type(runtime_hero, "ObscuredFloat", runtime_hero_name)
    info["hero_info_hero_key_offset"] = field_offset(hero_info, "HeroKey", "HeroInfoData")
    info["runtime_backend_inventory_typeinfo_rva"], info["runtime_backend_inventory_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        [f"{runtime_backend_inventory_name}_TypeInfo"],
        runtime_backend_inventory_name,
    )
    info["runtime_backend_inventory_class_name"] = runtime_backend_inventory_name
    info["runtime_backend_inventory_items_offset"] = static_field_offset_by_type(
        runtime_backend_inventory,
        "List<InventoryItemData>",
        runtime_backend_inventory_name,
    )
    info["backend_inventory_item_unique_key_offset"] = field_offset(backend_inventory_item, "itemKey", "InventoryItemData")
    info["backend_inventory_item_id_offset"] = field_offset(backend_inventory_item, "itemId", "InventoryItemData")

    return info


# --------------------------------------------------------------------------- #
# Verificacao na memoria viva (rotina de leitura dos logs)
# --------------------------------------------------------------------------- #
k32 = ctypes.windll.kernel32 if sys.platform == "win32" else None
STATIC_FIELDS_CANDIDATES = [0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xA8, 0xD8]


def _find_pid(name: str) -> int:
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"], text=True)
    for line in out.splitlines()[1:]:
        parts = line.strip('"').split('","')
        if len(parts) > 1 and parts[0].lower() == name.lower():
            try:
                return int(parts[1])
            except ValueError:
                pass
    return 0


def _module_base(pid: int, modname: str) -> int:
    class ME(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                    ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD),
                    ("hModule", wt.HMODULE), ("szModule", ctypes.c_wchar * 256),
                    ("szExePath", ctypes.c_wchar * 260)]
    snap = k32.CreateToolhelp32Snapshot(0x18, pid)  # SNAPMODULE | SNAPMODULE32
    me = ME(); me.dwSize = ctypes.sizeof(ME); base = 0
    if k32.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == modname.lower():
                base = me.modBaseAddr; break
            if not k32.Module32NextW(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return base or 0


class LiveReader:
    def __init__(self, pid: int):
        self.h = k32.OpenProcess(0x0410, False, pid)  # VM_READ | QUERY_INFORMATION
        self.base = _module_base(pid, MODULE_NAME)
        self._got = ctypes.c_size_t()

    def rd(self, addr: int, n: int):
        b = ctypes.create_string_buffer(n)
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), b, n, ctypes.byref(self._got)):
            return None
        return b.raw[: self._got.value]

    def rptr(self, addr: int) -> int:
        d = self.rd(addr, 8)
        return struct.unpack("<Q", d)[0] if d and len(d) == 8 else 0

    def rint(self, addr: int):
        d = self.rd(addr, 4)
        return struct.unpack("<i", d)[0] if d and len(d) == 4 else None

    def rstr(self, addr: int):
        if not addr:
            return None
        d = self.rd(addr + 0x10, 4)
        if not d:
            return None
        n = struct.unpack("<i", d)[0]
        if n < 0 or n > 8192:
            return None
        d = self.rd(addr + 0x14, n * 2)
        return d.decode("utf-16-le", "replace") if d else None

    def class_name(self, obj: int):
        kl = self.rptr(obj) & ~1
        p = self.rptr(kl + 0x10)
        d = self.rd(p, 96)
        return d.split(b"\0")[0].decode("ascii", "replace") if d else ""


CLOCK_RE = re.compile(r"\[\d{1,2}:\d{2}\]")


def verify_live(info: dict) -> dict:
    """Confirma offsets lendo o jogo vivo. Atualiza info com os escolhidos."""
    pid = _find_pid(PROCESS_NAME)
    if not pid:
        print("[!] Jogo nao esta rodando — pulei a verificacao ao vivo.")
        print("    (static_fields sera mantido do main.cpp atual.)")
        info["static_fields_offset"] = None
        return info
    r = LiveReader(pid)
    if not r.base:
        raise SystemExit("Nao consegui achar GameAssembly.dll no processo.")
    klass = r.rptr(r.base + info["typeinfo_rva"])
    if not klass:
        raise SystemExit("TypeInfo nulo ao vivo (jogo ainda carregando?).")

    # 1) Descobre static_fields + qual List<LogData> da o log completo.
    best = None  # (static_off, list_off, items_ptr, size)
    empty_best = None  # (static_off, list_off)
    for soff in STATIC_FIELDS_CANDIDATES:
        statics = r.rptr(klass + soff)
        instance = r.rptr(statics)
        if not instance:
            continue
        for loff in info["list_offsets"]:
            lst = r.rptr(instance + loff)
            if not lst:
                continue
            items = r.rptr(lst + 0x10)
            size = r.rint(lst + 0x18)
            if not items or size is None or size < 0 or size > 4000:
                continue
            if size == 0:
                if empty_best is None:
                    empty_best = (soff, loff)
                continue
            # valida: primeiro item parece um objeto gerenciado com classe nomeada
            obj0 = r.rptr(items + 0x20)
            cn = r.class_name(obj0) if obj0 else ""
            if cn.endswith("Log") or cn == "TempLogData":
                if best is None or size > best[3]:
                    best = (soff, loff, items, size)
    if not best:
        if empty_best:
            soff, loff = empty_best
            info["static_fields_offset"] = soff
            info["list_offset"] = loff
            print(f"[*] Cadeia resolvida ao vivo: static_fields=0x{soff:X} list=0x{loff:X} size=0")
            print("[!] Lista de logs vazia — offsets de texto/relogio serao mantidos do main.cpp atual.")
            return info
        raise SystemExit("Nao consegui resolver a cadeia LogManager ao vivo (offsets mudaram demais?).")
    soff, loff, items, size = best
    info["static_fields_offset"] = soff
    info["list_offset"] = loff
    print(f"[*] Cadeia resolvida ao vivo: static_fields=0x{soff:X} list=0x{loff:X} size={size}")

    # 2) Confirma texto vs relogio entre os offsets de string do LogData.
    sample = [r.rptr(items + 0x20 + i * 8) for i in range(max(0, size - 40), size)]
    sample = [o for o in sample if o]
    if len(sample) < 3:
        print("[!] Poucos eventos no LogManager para identificar texto/relogio — mantendo offsets atuais do main.cpp.")
        return info
    # Pontua cada offset de string. O relogio casa [HH:MM]; o TEXTO renderizado
    # carrega markup <color=...> (mensagem do evento). A chave de formato
    # (ex.: "LogMessage_...") nao tem markup e e ignorada.
    FMT_PREFIX = ("LogMessage_", "ItemName_", "MonsterName_", "Grade_")
    scores = {}
    for off in info["string_offsets"]:
        vals = [r.rstr(r.rptr(o + off)) or "" for o in sample]
        clockish = sum(1 for v in vals if CLOCK_RE.search(v))
        colorish = sum(1 for v in vals if "<color=" in v)
        scores[off] = (clockish, colorish, vals)
    thresh = max(3, len(sample) // 4)
    clock_off = max(info["string_offsets"], key=lambda o: scores[o][0])
    if scores[clock_off][0] < thresh:
        clock_off = None
    text_candidates = [o for o in info["string_offsets"] if o != clock_off]
    text_off = max(text_candidates, key=lambda o: scores[o][1]) if text_candidates else None
    if text_off is not None and scores[text_off][1] < thresh:
        # Sem markup detectado: pega o 1o offset que nao e chave de formato nem relogio.
        text_off = next(
            (o for o in text_candidates
             if not all(v.startswith(FMT_PREFIX) for v in scores[o][2] if v)
             and scores[o][0] < thresh),
            text_off,
        )
    if text_off is None or clock_off is None:
        raise SystemExit("Nao identifiquei offsets de texto/relogio do LogData ao vivo.")
    info["text_offset"] = text_off
    info["clock_offset"] = clock_off
    print(f"[*] LogData: texto=0x{text_off:X} relogio=0x{clock_off:X} datetime=0x{info['datetime_offset']:X}")

    # 3) Amostra: ultimos BoxOpenLog/GetBoxLog com item key + grade.
    print("[*] Amostra dos ultimos eventos (verificacao):")
    shown = 0
    for i in range(size - 1, -1, -1):
        if shown >= 6:
            break
        obj = r.rptr(items + 0x20 + i * 8)
        if not obj:
            continue
        cn = r.class_name(obj)
        if cn not in ("BoxOpenLog", "GetBoxLog"):
            continue
        text = (r.rstr(r.rptr(obj + text_off)) or "").strip()
        msg = re.sub(r"<[^>]+>", "", text)
        extra = ""
        if cn == "BoxOpenLog":
            key = r.rstr(r.rptr(obj + info["box_item_key_offset"])) or "?"
            g = r.rint(obj + info["box_grade_offset"])
            gname = info["grade_names"][g] if g is not None and 0 <= g < len(info["grade_names"]) else "?"
            extra = f"  key={key} grade={gname}"
        print(f"    {cn:11} {msg[:48]!r}{extra}")
        shown += 1
    return info


# --------------------------------------------------------------------------- #
# Patching
# --------------------------------------------------------------------------- #
def patch_main_cpp(info: dict, version: str, dry: bool) -> bool:
    text = MAIN_CPP.read_text(encoding="utf-8")
    return _write_if_changed(MAIN_CPP, _rebuild_main(text, version, info), dry)


def _rebuild_main(text: str, version: str, info: dict) -> str:
    grades_inner = ",\n".join(
        "    " + ", ".join(f'"{g}"' for g in info["grade_names"][i : i + 5])
        for i in range(0, len(info["grade_names"]), 5)
    )

    # Valor atual no main.cpp (fallback quando a verificacao ao vivo nao rodou).
    def current(name: str) -> int | None:
        m = re.search(rf"{name} = (0x[0-9A-Fa-f]+);", text)
        return int(m.group(1), 16) if m else None

    # static_fields, list, texto e relogio so sao confiaveis com o jogo vivo;
    # sem isso, preserva os valores atuais (nao adivinha pela ordem do dump).
    static_off = info.get("static_fields_offset") or current("kKlassStaticFieldsOffset")
    list_off = info.get("list_offset") or current("kLogManagerListOffset") or info["list_offsets"][0]
    text_off = info.get("text_offset") or current("kLogDataTextOffset")
    clock_off = info.get("clock_offset") or current("kLogDataClockOffset")
    block = f"""// ===== BEGIN IL2CPP MAP (TaskBarHero {version}) =====
constexpr wchar_t kIl2CppMapGameVersion[] = L"{version}";
constexpr uintptr_t kLogManagerTypeInfoRva = 0x{info['typeinfo_rva']:X};
constexpr uintptr_t kKlassStaticFieldsOffset = 0x{static_off:X};
constexpr uintptr_t kLogManagerListOffset = 0x{list_off:X};
constexpr uintptr_t kLogDataTextOffset = 0x{text_off:X};
constexpr uintptr_t kLogDataClockOffset = 0x{clock_off:X};
constexpr uintptr_t kLogDataDateTimeOffset = 0x{info['datetime_offset']:X};
constexpr uintptr_t kBoxOpenItemKeyOffset = 0x{info['box_item_key_offset']:X};
constexpr uintptr_t kBoxOpenGradeOffset = 0x{info['box_grade_offset']:X};
constexpr uintptr_t kSaveManagerTypeInfoRva = 0x{info['save_typeinfo_rva']:X};
constexpr uintptr_t kStageManagerTypeInfoRva = 0x{info['stage_manager_typeinfo_rva']:X};
constexpr uintptr_t kMonsterSpawnManagerTypeInfoRva = 0x{info['monster_spawn_manager_typeinfo_rva']:X};
constexpr uintptr_t kRuntimeCurrencyManagerTypeInfoRva = 0x{info['runtime_currency_manager_typeinfo_rva']:X};
constexpr uintptr_t kRuntimeHeroManagerTypeInfoRva = 0x{info['runtime_hero_manager_typeinfo_rva']:X};
constexpr uintptr_t kRuntimeBackendInventoryTypeInfoRva = 0x{info['runtime_backend_inventory_typeinfo_rva']:X};
constexpr uintptr_t kSaveManagerAccountSaveOffset = 0x{info['save_manager_account_offset']:X};
constexpr uintptr_t kSaveManagerPlayerSaveOffset = 0x{info['save_manager_player_offset']:X};
constexpr uintptr_t kAccountSavePlayerIdOffset = 0x{info['account_player_id_offset']:X};
constexpr uintptr_t kAccountSaveVersionOffset = 0x{info['account_version_offset']:X};
constexpr uintptr_t kAccountSaveOwnerSteamIdOffset = 0x{info['account_owner_steam_id_offset']:X};
constexpr uintptr_t kCommonSaveVersionOffset = 0x{info['common_version_offset']:X};
constexpr uintptr_t kCommonSavePlayTimeOffset = 0x{info['common_play_time_offset']:X};
constexpr uintptr_t kCommonSaveArrangedPetKeyOffset = 0x{info['common_arranged_pet_key_offset']:X};
constexpr uintptr_t kCommonSaveArrangedHeroKeyOffset = 0x{info['common_arranged_hero_key_offset']:X};
constexpr uintptr_t kCommonSaveMaxCompletedStageOffset = 0x{info['common_max_completed_stage_offset']:X};
constexpr uintptr_t kCommonSaveCurrentStageKeyOffset = 0x{info['common_current_stage_key_offset']:X};
constexpr uintptr_t kCommonSaveCurrentStageWaveOffset = 0x{info['common_current_stage_wave_offset']:X};
constexpr uintptr_t kPlayerSaveCommonOffset = 0x{info['player_common_offset']:X};
constexpr uintptr_t kPlayerSaveCurrenciesOffset = 0x{info['player_currencies_offset']:X};
constexpr uintptr_t kPlayerSaveHeroesOffset = 0x{info['player_heroes_offset']:X};
constexpr uintptr_t kPlayerSaveAttributesOffset = 0x{info['player_attributes_offset']:X};
constexpr uintptr_t kPlayerSavePetsOffset = 0x{info['player_pets_offset']:X};
constexpr uintptr_t kPlayerSaveRunesOffset = 0x{info['player_runes_offset']:X};
constexpr uintptr_t kPlayerSaveInventoryOffset = 0x{info['player_inventory_offset']:X};
constexpr uintptr_t kPlayerSaveStashOffset = 0x{info['player_stash_offset']:X};
constexpr uintptr_t kPlayerSaveTradeStashOffset = 0x{info['player_trade_stash_offset']:X};
constexpr uintptr_t kPlayerSaveItemsOffset = 0x{info['player_items_offset']:X};
constexpr uintptr_t kPlayerSaveAggregatesOffset = 0x{info['player_aggregates_offset']:X};
constexpr uintptr_t kStageManagerStageStateOffset = 0x{info['stage_manager_stage_state_offset']:X};
constexpr uintptr_t kStageManagerStageStartedOffset = 0x{info['stage_manager_stage_started_offset']:X};
constexpr uintptr_t kStageManagerRuntimeFloatOffset = 0x{info['stage_manager_runtime_float_offset']:X};
constexpr uintptr_t kStageManagerRuntimeIntOffset = 0x{info['stage_manager_runtime_int_offset']:X};
constexpr uintptr_t kStageManagerRuntimeListAOffset = 0x{info['stage_manager_runtime_list_a_offset']:X};
constexpr uintptr_t kStageManagerRuntimeListBOffset = 0x{info['stage_manager_runtime_list_b_offset']:X};
constexpr uintptr_t kMonsterSpawnManagerMonsterListOffset = 0x{info['monster_spawn_manager_monster_list_offset']:X};
constexpr uintptr_t kMonsterSpawnManagerDeadMonsterListOffset = 0x{info['monster_spawn_manager_dead_monster_list_offset']:X};
constexpr uintptr_t kMonsterSpawnManagerSummonedMonsterListOffset = 0x{info['monster_spawn_manager_summoned_monster_list_offset']:X};
constexpr uintptr_t kMonsterSpawnManagerForceBossWaveOffset = 0x{info['monster_spawn_manager_force_boss_wave_offset']:X};
constexpr uintptr_t kMonsterCacheOffset = 0x{info['monster_cache_offset']:X};
constexpr uintptr_t kMonsterTypeOffset = 0x{info['monster_type_offset']:X};
constexpr uintptr_t kMonsterRuntimeIntAOffset = 0x{info['monster_runtime_int_a_offset']:X};
constexpr uintptr_t kMonsterRuntimeIntBOffset = 0x{info['monster_runtime_int_b_offset']:X};
constexpr uintptr_t kMonsterRuntimeFloatOffset = 0x{info['monster_runtime_float_offset']:X};
constexpr uintptr_t kMonsterStageTypeOffset = 0x{info['monster_stage_type_offset']:X};
constexpr uintptr_t kMonsterRuntimeIntCOffset = 0x{info['monster_runtime_int_c_offset']:X};
constexpr uintptr_t kMonsterCacheInfoDataOffset = 0x{info['monster_cache_info_data_offset']:X};
constexpr uintptr_t kMonsterInfoMonsterKeyOffset = 0x{info['monster_info_monster_key_offset']:X};
constexpr uintptr_t kMonsterInfoMonsterTypeOffset = 0x{info['monster_info_monster_type_offset']:X};
constexpr uintptr_t kMonsterInfoRewardGoldOffset = 0x{info['monster_info_reward_gold_offset']:X};
constexpr uintptr_t kMonsterInfoRewardExpOffset = 0x{info['monster_info_reward_exp_offset']:X};
constexpr uintptr_t kRuntimeCurrencyManagerListOffset = 0x{info['runtime_currency_manager_list_offset']:X};
constexpr uintptr_t kRuntimeCurrencyInfoOffset = 0x{info['runtime_currency_info_offset']:X};
constexpr uintptr_t kRuntimeCurrencyAmountOffset = 0x{info['runtime_currency_amount_offset']:X};
constexpr uintptr_t kRuntimeCurrencyAltAmountOffset = 0x{info['runtime_currency_alt_amount_offset']:X};
constexpr uintptr_t kCurrencyInfoKeyOffset = 0x{info['currency_info_key_offset']:X};
constexpr uintptr_t kRuntimeHeroDictionaryOffset = 0x{info['runtime_hero_dictionary_offset']:X};
constexpr uintptr_t kRuntimeHeroInfoOffset = 0x{info['runtime_hero_info_offset']:X};
constexpr uintptr_t kRuntimeHeroLevelOffset = 0x{info['runtime_hero_level_offset']:X};
constexpr uintptr_t kRuntimeHeroAbilityPointOffset = 0x{info['runtime_hero_ability_point_offset']:X};
constexpr uintptr_t kRuntimeHeroAllocatedAbilityPointOffset = 0x{info['runtime_hero_allocated_ability_point_offset']:X};
constexpr uintptr_t kRuntimeHeroExpOffset = 0x{info['runtime_hero_exp_offset']:X};
constexpr uintptr_t kHeroInfoHeroKeyOffset = 0x{info['hero_info_hero_key_offset']:X};
constexpr uintptr_t kRuntimeBackendInventoryItemsOffset = 0x{info['runtime_backend_inventory_items_offset']:X};
constexpr uintptr_t kBackendInventoryItemUniqueKeyOffset = 0x{info['backend_inventory_item_unique_key_offset']:X};
constexpr uintptr_t kBackendInventoryItemIdOffset = 0x{info['backend_inventory_item_id_offset']:X};
static const char* const kGradeNames[] = {{
{grades_inner},
}};
// ===== END IL2CPP MAP ====="""
    return re.sub(
        r"// ===== BEGIN IL2CPP MAP.*?// ===== END IL2CPP MAP =====",
        lambda _m: block,
        text,
        flags=re.S,
    )


def patch_grade_map_js(path: Path, var_decl: str, info: dict, dry: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    missing = [g for g in info["grade_names"] if g not in GRADE_PT or GRADE_PT[g] is None]
    if missing:
        print(f"[!] Sem traducao PT para: {missing} — preencha GRADE_PT no script. Mantendo {path.name}.")
        return False
    begin = "// ===== BEGIN GRADE MAP"
    end = "// ===== END GRADE MAP ====="
    m = re.search(r"(?m)^([ \t]*)" + re.escape(begin) + r"[^\n]*\n", text)
    if not m or end not in text:
        raise SystemExit(f"Marcadores GRADE MAP nao encontrados em {path}.")
    prefix = m.group(1)
    begin_line = m.group(0)  # linha BEGIN completa (com sufixo) + \n
    lines = ",\n".join(f'{prefix}  {g}: "{GRADE_PT[g]}"' for g in info["grade_names"])
    block = begin_line + f"{prefix}{var_decl} = {{\n{lines},\n{prefix}}};\n{prefix}{end}"
    pattern = r"(?ms)^[ \t]*" + re.escape(begin) + r".*?^[ \t]*" + re.escape(end)
    new = re.sub(pattern, lambda _m: block, text, count=1)
    return _write_if_changed(path, new, dry)


def _write_if_changed(path: Path, new: str, dry: bool) -> bool:
    old = path.read_text(encoding="utf-8")
    if old == new:
        print(f"    {path.name}: sem mudancas.")
        return False
    if dry:
        print(f"    {path.name}: MUDARIA (dry-run).")
        return True
    path.write_text(new, encoding="utf-8")
    print(f"    {path.name}: atualizado.")
    return True


# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description="Recalcula o mapa IL2CPP e atualiza o codigo.")
    ap.add_argument("--game-dir", help="pasta do TaskBarHero (com GameAssembly.dll)")
    ap.add_argument("--no-live", action="store_true", help="nao verificar na memoria do jogo")
    ap.add_argument("--dry-run", action="store_true", help="so mostra o que mudaria")
    ap.add_argument("--print-version", action="store_true",
                    help="so imprime a versao do jogo (sem dump/patch) e sai")
    args = ap.parse_args()

    if sys.platform != "win32":
        raise SystemExit("Este script roda no Windows (precisa ler a memoria do jogo).")

    game_dir = find_game_dir(args.game_dir)
    version = game_version(game_dir)
    if args.print_version:
        print(version)
        return 0
    print(f"[*] Jogo: {game_dir}  (versao {version})")

    dumper = ensure_dumper()
    dump_dir = run_dumper(dumper, game_dir)
    info = extract_map(dump_dir)
    print(f"[*] Extraido do dump: TypeInfo {info.get('typeinfo_name', '?')} "
          f"RVA=0x{info['typeinfo_rva']:X} "
          f"listas={[hex(x) for x in info['list_offsets']]} "
          f"strings={[hex(x) for x in info['string_offsets']]} "
          f"datetime=0x{info['datetime_offset']:X} "
          f"box(item=0x{info['box_item_key_offset']:X},grade=0x{info['box_grade_offset']:X})")
    print(f"[*] Save manager: {info.get('save_manager_class_name', '?')} "
          f"TypeInfo {info.get('save_typeinfo_name', '?')} "
          f"RVA=0x{info['save_typeinfo_rva']:X} "
          f"account=0x{info['save_manager_account_offset']:X} "
          f"player=0x{info['save_manager_player_offset']:X}")
    print(f"[*] Runtime stage: StageManager {info.get('stage_manager_typeinfo_name', '?')} "
          f"RVA=0x{info['stage_manager_typeinfo_rva']:X}; "
          f"MonsterSpawnManager {info.get('monster_spawn_manager_typeinfo_name', '?')} "
          f"RVA=0x{info['monster_spawn_manager_typeinfo_rva']:X}")
    print(f"[*] Runtime rewards: Monster.cache=0x{info['monster_cache_offset']:X} "
          f"MonsterInfoData=0x{info['monster_cache_info_data_offset']:X} "
          f"rewardGold=0x{info['monster_info_reward_gold_offset']:X} "
          f"rewardExp=0x{info['monster_info_reward_exp_offset']:X}")
    print(f"[*] Runtime currency: {info.get('runtime_currency_manager_typeinfo_name', '?')} "
          f"RVA=0x{info['runtime_currency_manager_typeinfo_rva']:X} "
          f"list=0x{info['runtime_currency_manager_list_offset']:X} "
          f"amount=0x{info['runtime_currency_amount_offset']:X}")
    print(f"[*] Runtime heroes: {info.get('runtime_hero_manager_typeinfo_name', '?')} "
          f"RVA=0x{info['runtime_hero_manager_typeinfo_rva']:X} "
          f"dict=0x{info['runtime_hero_dictionary_offset']:X} "
          f"level=0x{info['runtime_hero_level_offset']:X} "
          f"ability=0x{info['runtime_hero_ability_point_offset']:X} "
          f"allocatedAbility=0x{info['runtime_hero_allocated_ability_point_offset']:X} "
          f"exp=0x{info['runtime_hero_exp_offset']:X}")
    print(f"[*] Runtime backend inventory: {info.get('runtime_backend_inventory_typeinfo_name', '?')} "
          f"RVA=0x{info['runtime_backend_inventory_typeinfo_rva']:X} "
          f"items=0x{info['runtime_backend_inventory_items_offset']:X} "
          f"uniqueKey=0x{info['backend_inventory_item_unique_key_offset']:X} "
          f"itemId=0x{info['backend_inventory_item_id_offset']:X}")
    print(f"[*] EGradeType: {info['grade_names']}")

    if not args.no_live:
        info = verify_live(info)
    else:
        info["static_fields_offset"] = None

    print("[*] Atualizando arquivos:")
    changed = False
    changed |= patch_main_cpp(info, version, args.dry_run)
    changed |= patch_grade_map_js(SERVER_JS, "const gradePt", info, args.dry_run)
    changed |= patch_grade_map_js(HISTORY_DOMAIN_JS, "const HISTORY_GRADE_PT", info, args.dry_run)

    print()
    if args.dry_run:
        print("Dry-run concluido. Rode sem --dry-run para aplicar.")
    elif changed:
        print("Pronto! Agora:")
        print("   1) cd tbh-companion-agent && build.bat")
        print("   2) reinicie o agente (apague %LOCALAPPDATA%\\TBH Companion\\sync-state.json p/ reenvio total)")
        print("   3) suba o frontend (server.js + public/app/history-domain.js)")
    else:
        print("Nada mudou — o mapa ja estava atualizado para esta versao.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
