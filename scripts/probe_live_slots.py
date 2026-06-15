#!/usr/bin/env python3
"""Probe live TaskBarHero item slots.

Development-only helper for the inventory -> storage automation. It reads the
current Il2CppDumper output, resolves SlotInteractionManager's singleton, and
tries to enumerate the live Dictionary<GameObject, qr> that registers item slots.

This script is intentionally not production runtime. Use it to validate memory
layouts before porting the stable reader to C++.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from refresh_il2cpp_map import (  # noqa: E402
    CACHE_DIR,
    MODULE_NAME,
    STATIC_FIELDS_CANDIDATES,
    LiveReader,
    _find_pid,
    class_body,
    fields_of,
    find_typeinfo_rva,
)


def field_offsets(dump_text: str, class_name: str) -> dict[str, int]:
    body = class_body(dump_text, class_name)
    if not body:
        raise SystemExit(f"classe {class_name} nao encontrada no dump.")
    return {name: offset for (_type, name, offset) in fields_of(body)}


def read_object_ptrs_from_list(reader: LiveReader, list_ptr: int, max_items: int = 512) -> list[int]:
    if not list_ptr:
        return []
    items = reader.rptr(list_ptr + 0x10)
    size = reader.rint(list_ptr + 0x18)
    if not items or size is None or size < 0 or size > max_items:
        return []
    out: list[int] = []
    for i in range(size):
        ptr = reader.rptr(items + 0x20 + i * 8)
        if ptr:
            out.append(ptr)
    return out


def candidate_dictionary_values(reader: LiveReader, dict_ptr: int, max_count: int = 2048) -> tuple[int, list[tuple[int, int, str]]]:
    """Return (entry_stride, [(entry_index, value_ptr, class_name)]).

    IL2CPP Dictionary entry layout is version/runtime dependent enough that this
    helper tries a small set of plausible strides. The correct one is the first
    that yields slot-like objects.
    """
    if not dict_ptr:
        return 0, []
    entries = reader.rptr(dict_ptr + 0x18)
    count = reader.rint(dict_ptr + 0x20)
    if not entries or count is None or count < 0 or count > max_count:
        return 0, []

    best_stride = 0
    best: list[tuple[int, int, str]] = []
    for stride in (0x18, 0x20, 0x28, 0x30):
        found: list[tuple[int, int, str]] = []
        for index in range(count):
            entry = entries + 0x20 + index * stride
            # Dictionary Entry<TKey,TValue>: hashCode(int), next(int), key, value.
            value = reader.rptr(entry + 0x10)
            if not value:
                continue
            name = reader.class_name(value)
            if name in {"InventorySlot", "StashSlot", "TradingStashSlot", "CubeInventorySlot"}:
                found.append((index, value, name))
        if len(found) > len(best):
            best_stride = stride
            best = found
    return best_stride, best


def resolve_singleton(reader: LiveReader, typeinfo_rva: int) -> tuple[int, int]:
    klass = reader.rptr(reader.base + typeinfo_rva)
    if not klass:
        return 0, 0
    for static_offset in STATIC_FIELDS_CANDIDATES:
        statics = reader.rptr(klass + static_offset)
        instance = reader.rptr(statics)
        if instance and reader.class_name(instance) == "SlotInteractionManager":
            return instance, static_offset
    return 0, 0


def main() -> int:
    dump_dir = CACHE_DIR / "dump"
    dump_cs = dump_dir / "dump.cs"
    script_json = dump_dir / "script.json"
    if not dump_cs.exists() or not script_json.exists():
        raise SystemExit("dump.cs/script.json nao encontrados. Rode scripts/refresh_il2cpp_map.py primeiro.")

    dump_text = dump_cs.read_text(encoding="utf-8", errors="ignore")
    slot_interaction = field_offsets(dump_text, "SlotInteractionManager")
    item_slot = field_offsets(dump_text, "ItemSlot")
    inventory_slot = field_offsets(dump_text, "InventorySlot")
    stash_slot = field_offsets(dump_text, "StashSlot")
    ui_hero = field_offsets(dump_text, "UI_Hero")
    ui_stash = field_offsets(dump_text, "UI_RemakeStash")
    typeinfo_rva, typeinfo_name = find_typeinfo_rva(script_json, ["np<SlotInteractionManager>_TypeInfo"])

    print(f"[*] SlotInteractionManager TypeInfo: {typeinfo_name} RVA=0x{typeinfo_rva:X}")
    print(f"[*] Offsets: bddd=0x{slot_interaction['bddd']:X}, itemRect=0x{item_slot['m_slotRectTransform']:X}, "
          f"inventory.index=0x{inventory_slot['index']:X}, stash.Index=0x{stash_slot['Index']:X}")
    print(f"[*] Alternative roots: UI_Hero.inventorySlots=0x{ui_hero['inventorySlots']:X}, "
          f"UI_RemakeStash.m_stashSlotList=0x{ui_stash['m_stashSlotList']:X}")

    pid = _find_pid("TaskBarHero.exe")
    if not pid:
        print("[!] TaskBarHero.exe nao esta rodando; probe ao vivo pulado.")
        return 2
    reader = LiveReader(pid)
    if not reader.base:
        print(f"[!] {MODULE_NAME} nao encontrado no processo.")
        return 2

    instance, static_offset = resolve_singleton(reader, typeinfo_rva)
    if not instance:
        print("[!] SlotInteractionManager singleton nao resolvido.")
        return 2

    print(f"[*] SlotInteractionManager instance=0x{instance:X} static_fields_offset=0x{static_offset:X}")
    slot_dict = reader.rptr(instance + slot_interaction["bddd"])
    print(f"[*] bddd Dictionary<GameObject, qr>=0x{slot_dict:X}")

    stride, values = candidate_dictionary_values(reader, slot_dict)
    if not values:
        print("[!] Nenhum slot encontrado em bddd. Abra inventario/storage no jogo e rode novamente.")
        return 1

    print(f"[*] Slots vivos encontrados: {len(values)} (dictionary entry stride=0x{stride:X})")
    for entry_index, ptr, klass in values[:120]:
        if klass == "InventorySlot":
            index = reader.rint(ptr + inventory_slot["index"])
        elif klass == "StashSlot":
            index = reader.rint(ptr + stash_slot["Index"])
        else:
            index = None
        rect = reader.rptr(ptr + item_slot["m_slotRectTransform"])
        item_a = reader.rptr(ptr + item_slot["bfqq"])
        print(f"{klass:16} entry={entry_index:03d} ptr=0x{ptr:X} index={index} rect=0x{rect:X} itemField=0x{item_a:X}")
    print("[*] Coordenada de tela ainda nao e calculada por este probe.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
