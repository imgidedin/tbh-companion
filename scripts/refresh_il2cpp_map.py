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
       - o enum EGradeType (nomes das raridades, em ordem)
  4. VERIFICA na memoria do jogo vivo (rotina de leitura dos logs de eventos):
     resolve a cadeia de ponteiros, descobre o offset de static_fields por forca
     bruta, confirma quais offsets dao texto/relogio/DateTime validos e mostra
     uma amostra dos ultimos eventos com categoria/raridade.
  5. Atualiza, entre marcadores:
       - src/main.cpp                       (bloco IL2CPP MAP + kGradeNames)
       - ../tbh-farm-local-frontend/server.js          (gradePt)
       - ../tbh-farm-local-frontend/public/calculator.js (HISTORY_GRADE_PT)

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
CALCULATOR_JS = REPO_ROOT / "tbh-farm-local-frontend" / "public" / "calculator.js"
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
        r"// Namespace:[^\n]*\n(?:\[[^\n]*\]\n)*public (?:abstract |sealed )*class " + re.escape(name) + r"\b.*?\n\}",
        src, re.S,
    )
    return m.group(0) if m else None


def fields_of(body: str) -> list[tuple[str, str, int]]:
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
        if " static " in f" {decl} " or "const " in decl:
            continue
        parts = decl.replace("readonly ", "").split()
        # parts: [modifier, ..., TYPE, NAME]
        if len(parts) < 2:
            continue
        ftype, fname = parts[-2], parts[-1]
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

    bal = require_class(src, "bal")
    info["save_typeinfo_rva"], info["save_typeinfo_name"] = find_typeinfo_rva(
        dump_dir / "script.json",
        singleton_typeinfo_candidates(bal, "bal", ["bal_TypeInfo"]),
        "bal",
    )
    info["save_manager_account_offset"] = field_offset(bal, "bgaw", "bal")
    info["save_manager_player_offset"] = field_offset(bal, "bgax", "bal")

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
constexpr uintptr_t kLogManagerTypeInfoRva = 0x{info['typeinfo_rva']:X};
constexpr uintptr_t kKlassStaticFieldsOffset = 0x{static_off:X};
constexpr uintptr_t kLogManagerListOffset = 0x{list_off:X};
constexpr uintptr_t kLogDataTextOffset = 0x{text_off:X};
constexpr uintptr_t kLogDataClockOffset = 0x{clock_off:X};
constexpr uintptr_t kLogDataDateTimeOffset = 0x{info['datetime_offset']:X};
constexpr uintptr_t kBoxOpenItemKeyOffset = 0x{info['box_item_key_offset']:X};
constexpr uintptr_t kBoxOpenGradeOffset = 0x{info['box_grade_offset']:X};
constexpr uintptr_t kSaveManagerTypeInfoRva = 0x{info['save_typeinfo_rva']:X};
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
    m = re.search(re.escape(begin) + r"[^\n]*\n", text)
    if not m or end not in text:
        raise SystemExit(f"Marcadores GRADE MAP nao encontrados em {path}.")
    begin_line = m.group(0)  # linha BEGIN completa (com sufixo) + \n
    lines = ",\n".join(f'  {g}: "{GRADE_PT[g]}"' for g in info["grade_names"])
    block = begin_line + f"{var_decl} = {{\n{lines},\n}};\n" + end
    new = re.sub(re.escape(begin) + r".*?" + re.escape(end), lambda _m: block, text, flags=re.S)
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
    print(f"[*] Save manager: TypeInfo {info.get('save_typeinfo_name', '?')} "
          f"RVA=0x{info['save_typeinfo_rva']:X} "
          f"account=0x{info['save_manager_account_offset']:X} "
          f"player=0x{info['save_manager_player_offset']:X}")
    print(f"[*] EGradeType: {info['grade_names']}")

    if not args.no_live:
        info = verify_live(info)
    else:
        info["static_fields_offset"] = None

    print("[*] Atualizando arquivos:")
    changed = False
    changed |= patch_main_cpp(info, version, args.dry_run)
    changed |= patch_grade_map_js(SERVER_JS, "const gradePt", info, args.dry_run)
    changed |= patch_grade_map_js(CALCULATOR_JS, "const HISTORY_GRADE_PT", info, args.dry_run)

    print()
    if args.dry_run:
        print("Dry-run concluido. Rode sem --dry-run para aplicar.")
    elif changed:
        print("Pronto! Agora:")
        print("   1) cd tbh-companion-agent && build.bat")
        print("   2) reinicie o agente (apague %LOCALAPPDATA%\\TBH Companion\\sync-state.json p/ reenvio total)")
        print("   3) suba o frontend (server.js + public/calculator.js)")
    else:
        print("Nada mudou — o mapa ja estava atualizado para esta versao.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
