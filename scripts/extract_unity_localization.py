import argparse
import csv
import datetime as dt
import glob
import json
import os
import re
import sys

try:
    import UnityPy
except ImportError as exc:
    raise SystemExit(
        "UnityPy is required. Install it with: python -m pip install UnityPy"
    ) from exc


LOCALE_BUNDLE_PATTERNS = {
    "en-US": "localization-string-tables-english(unitedstates)(en-us)_assets_all*.bundle",
    "pt-BR": "localization-string-tables-portuguese(brazil)(pt-br)_assets_all*.bundle",
}


def fail(message: str) -> None:
    raise SystemExit(message)


def find_addressables_dir(game_dir: str) -> str:
    candidate = os.path.join(
        game_dir,
        "TaskBarHero_Data",
        "StreamingAssets",
        "aa",
        "StandaloneWindows64",
    )
    if not os.path.isdir(candidate):
        fail(f"Addressables directory not found: {candidate}")
    return candidate


def find_one_bundle(addressables_dir: str, pattern: str) -> str:
    matches = sorted(glob.glob(os.path.join(addressables_dir, pattern)))
    if not matches:
        fail(f"Bundle not found for pattern: {pattern}")
    return matches[0]


def read_shared_tables(shared_bundle: str) -> dict[str, dict[int, str]]:
    env = UnityPy.load(shared_bundle)
    tables: dict[str, dict[int, str]] = {}

    for obj in env.objects:
        if obj.type.name != "MonoBehaviour":
            continue
        tree = obj.read_typetree()
        name = tree.get("m_Name", "")
        if name == "ItemTable Shared Data":
            table_name = "ItemTable"
        elif name == "StringTable Shared Data":
            table_name = "StringTable"
        else:
            continue

        entries = {}
        for entry in tree.get("m_Entries", []):
            key = entry.get("m_Key", "")
            if key:
                entries[int(entry["m_Id"])] = key
        tables[table_name] = entries

    for required in ("StringTable", "ItemTable"):
        if required not in tables:
            fail(f"Shared localization table missing: {required}")

    return tables


def read_locale_tables(bundle: str, shared_tables: dict[str, dict[int, str]]) -> dict[str, dict[str, str]]:
    env = UnityPy.load(bundle)
    tables: dict[str, dict[str, str]] = {}

    for obj in env.objects:
        if obj.type.name != "MonoBehaviour":
            continue
        tree = obj.read_typetree()
        name = tree.get("m_Name", "")
        if name.startswith("ItemTable_"):
            table_name = "ItemTable"
        elif name.startswith("StringTable_"):
            table_name = "StringTable"
        else:
            continue

        shared = shared_tables[table_name]
        localized: dict[str, str] = {}
        for entry in tree.get("m_TableData", []):
            key = shared.get(int(entry["m_Id"]))
            if key:
                localized[key] = entry.get("m_Localized", "")
        tables[table_name] = localized

    for required in ("StringTable", "ItemTable"):
        if required not in tables:
            fail(f"Localized table missing in {bundle}: {required}")

    return tables


def key_group(key: str) -> str:
    prefixes = (
        "ItemName_",
        "ItemDescription_",
        "StageName_",
        "MonsterName_",
        "HeroName_",
        "HeroDescription_",
        "PetName_",
        "PetDescription_",
        "RuneName_",
        "SkillName_",
        "SkillDescription_",
        "PassiveSkillName_",
    )
    for prefix in prefixes:
        if key.startswith(prefix):
            return prefix[:-1]
    match = re.match(r"^([^_]+)_", key)
    return match.group(1) if match else "<plain>"


def build_summary(shared_tables: dict[str, dict[int, str]], locales: dict[str, dict[str, dict[str, str]]]) -> dict:
    summary = {
        "sharedCounts": {table: len(entries) for table, entries in shared_tables.items()},
        "localeCounts": {},
        "groups": {},
    }

    for locale, tables in locales.items():
        summary["localeCounts"][locale] = {}
        for table, entries in tables.items():
            summary["localeCounts"][locale][table] = {
                "localized": len(entries),
                "nonEmpty": sum(1 for value in entries.values() if value),
            }

    for table, shared in shared_tables.items():
        counts: dict[str, int] = {}
        for key in shared.values():
            group = key_group(key)
            counts[group] = counts.get(group, 0) + 1
        summary["groups"][table] = dict(sorted(counts.items(), key=lambda item: (-item[1], item[0])))

    return summary


def write_tsv(path: str, locales: dict[str, dict[str, dict[str, str]]], locale_order: list[str]) -> None:
    rows = []
    for table in ("StringTable", "ItemTable"):
        keys = sorted(
            set().union(*(locales[locale].get(table, {}).keys() for locale in locale_order))
        )
        rows.extend((table, key) for key in keys)

    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(["table", "key", *locale_order])
        for table, key in rows:
            writer.writerow([table, key, *(locales[locale].get(table, {}).get(key, "") for locale in locale_order)])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Extract TaskBarHero Unity localization bundles.")
    parser.add_argument("--game-dir", required=True, help="TaskbarHero installation directory.")
    parser.add_argument("--out-dir", required=True, help="Output directory for generated localization files.")
    parser.add_argument("--locales", nargs="+", default=["en-US", "pt-BR"], help="Locales to extract.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    game_dir = os.path.abspath(args.game_dir)
    out_dir = os.path.abspath(args.out_dir)
    locale_order = args.locales

    addressables_dir = find_addressables_dir(game_dir)
    shared_bundle = find_one_bundle(addressables_dir, "localization-assets-shared_assets_all*.bundle")
    shared_tables = read_shared_tables(shared_bundle)

    locale_bundles = {}
    locales = {}
    for locale in locale_order:
        pattern = LOCALE_BUNDLE_PATTERNS.get(locale)
        if not pattern:
            fail(f"Unsupported locale: {locale}")
        bundle = find_one_bundle(addressables_dir, pattern)
        locale_bundles[locale] = bundle
        locales[locale] = read_locale_tables(bundle, shared_tables)

    os.makedirs(out_dir, exist_ok=True)
    locale_suffix = "-".join(locale_order).replace("en-US-pt-BR", "en-pt")
    json_path = os.path.join(out_dir, f"game-localization-{locale_suffix}.json")
    tsv_path = os.path.join(out_dir, f"game-localization-{locale_suffix}.tsv")

    payload = {
        "generatedAt": dt.datetime.now(dt.UTC).isoformat(),
        "source": {
            "gameDir": game_dir,
            "addressablesDir": addressables_dir,
            "sharedBundle": shared_bundle,
            "localeBundles": locale_bundles,
        },
        "summary": build_summary(shared_tables, locales),
        "locales": locales,
    }

    with open(json_path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    write_tsv(tsv_path, locales, locale_order)

    print(json.dumps({"json": json_path, "tsv": tsv_path}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
