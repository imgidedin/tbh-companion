# Applies Il2CppDumper script.json symbols to the currently open GameAssembly.dll.
#@category TaskBarHero
#@menupath Tools.TaskBarHero.Apply Il2CppDumper Labels

import json
import re

from ghidra.program.model.symbol import SourceType


PROCESS_LABEL_FIELDS = [
    "ScriptMethod",
    "ScriptString",
    "ScriptMetadata",
    "ScriptMetadataMethod",
]

USER_DEFINED = SourceType.USER_DEFINED
base_address = currentProgram.getImageBase()


def to_addr(rva):
    return base_address.add(rva)


def safe_text(value):
    if value is None:
        return ""
    try:
        if isinstance(value, unicode):
            return value
    except NameError:
        pass
    return str(value)


def safe_label(name):
    text = safe_text(name)
    text = text.replace(" ", "_")
    text = re.sub(r"[^0-9A-Za-z_.$@<>`~-]", "_", text)
    text = re.sub(r"_+", "_", text)
    text = text.strip("_")
    if not text:
        text = "Il2CppSymbol"
    if text[0].isdigit():
        text = "_" + text
    return text[:240]


def set_label(addr, name):
    label = safe_label(name)
    try:
        createLabel(addr, label, True, USER_DEFINED)
        return label
    except Exception:
        fallback = safe_label(label + "_" + str(addr))
        createLabel(addr, fallback, True, USER_DEFINED)
        return fallback


def set_comment(addr, text):
    try:
        setEOLComment(addr, safe_text(text))
    except Exception:
        pass


def maybe_make_function(addr):
    if getFunctionAt(addr) is None:
        try:
            createFunction(addr, None)
        except Exception:
            pass


script_file = askFile("Selecione script.json do Il2CppDumper", "Open")
with open(script_file.getAbsolutePath(), "rb") as handle:
    data = json.loads(handle.read().decode("utf-8"))

make_functions = askYesNo(
    "Il2CppDumper",
    "Criar funcoes nos enderecos do script.json? Pode demorar, mas ajuda o Decompiler.",
)

total = 0
for field in PROCESS_LABEL_FIELDS:
    total += len(data.get(field, []))
if make_functions:
    total += len(data.get("Addresses", []))

monitor.initialize(total)

for script_method in data.get("ScriptMethod", []):
    monitor.checkCancelled()
    monitor.setMessage("Il2CppDumper: methods")
    addr = to_addr(script_method["Address"])
    name = script_method.get("Name", "")
    set_label(addr, name)
    set_comment(addr, name)
    monitor.incrementProgress(1)

index = 1
for script_string in data.get("ScriptString", []):
    monitor.checkCancelled()
    monitor.setMessage("Il2CppDumper: strings")
    addr = to_addr(script_string["Address"])
    name = "StringLiteral_" + str(index)
    set_label(addr, name)
    set_comment(addr, script_string.get("Value", ""))
    index += 1
    monitor.incrementProgress(1)

for script_metadata in data.get("ScriptMetadata", []):
    monitor.checkCancelled()
    monitor.setMessage("Il2CppDumper: metadata")
    addr = to_addr(script_metadata["Address"])
    name = script_metadata.get("Name", "")
    set_label(addr, name)
    set_comment(addr, name)
    monitor.incrementProgress(1)

for script_metadata_method in data.get("ScriptMetadataMethod", []):
    monitor.checkCancelled()
    monitor.setMessage("Il2CppDumper: metadata methods")
    addr = to_addr(script_metadata_method["Address"])
    name = script_metadata_method.get("Name", "")
    set_label(addr, name)
    set_comment(addr, name)
    monitor.incrementProgress(1)

if make_functions:
    for rva in data.get("Addresses", []):
        monitor.checkCancelled()
        monitor.setMessage("Il2CppDumper: functions")
        maybe_make_function(to_addr(rva))
        monitor.incrementProgress(1)

print("Il2CppDumper labels applied.")
