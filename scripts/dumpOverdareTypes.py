# Scrapes the OVERDARE Studio API reference docs (https://docs.overdare.com/development/api-reference)
# into Luau `declare extern type` definitions, playing the same role for OVERDARE that
# scripts/dumpRobloxTypes.py plays for Roblox's official Full-API-Dump.json.
#
# There is no machine-readable OVERDARE API dump (unlike Roblox), so this instead scrapes
# the Markdown mirror of the docs site (append `.md` to any page URL) using the page index
# published at https://docs.overdare.com/llms.txt.
#
# Usage:
#   python3 dumpOverdareTypes.py --dump-json overdareApiDump.json   # scraped raw data only
#   python3 dumpOverdareTypes.py --output globalTypes.overdare.d.luau  # + generate Luau defs
#
# The generated .luau file is currently STANDALONE (includes stub declarations for any
# external type it references, e.g. Vector3/Instance/BasePart) so it can be compiled and
# sanity-checked in isolation. It is NOT YET merged into scripts/globalTypes.d.luau - see
# the .claude/skills/overdare-conversion/SKILL.md for the merge strategy still to be decided
# (OVERDARE class defs should replace same-named Roblox class defs; append OVERDARE-only
# classes; enum lists need merging into the existing Enum/ENUM_LIST rather than redeclared).

import argparse
import json
import re
import sys
import time
import urllib.request

LLMS_TXT_URL = "https://docs.overdare.com/llms.txt"
CLASS_URL_RE = re.compile(r"\[([^\]]+)\]\((https://docs\.overdare\.com/development/api-reference/classes/[^)]+)\)")
ENUM_URL_RE = re.compile(r"\[([^\]]+)\]\((https://docs\.overdare\.com/development/api-reference/enums/[^)]+)\)")
DATATYPE_URL_RE = re.compile(r"\[([^\]]+)\]\((https://docs\.overdare\.com/development/api-reference/datatype/[^)]+)\)")

FETCH_RETRY_ATTEMPTS = 3
FETCH_RETRY_DELAY_SECONDS = 1.0

LUAU_PRIMITIVES = {"number", "string", "boolean", "any", "nil", "unknown", "never", "true", "false"}

TYPE_ALIASES = {
    "void": "nil",
    # The docs use these as informal prose placeholders rather than real Luau type names.
    "Value": "any",
    "Array": "{ any }",
    "array": "{ any }",
    "Dictionary": "{ [string]: any }",
    "Tuple": "...any",
    "bool": "boolean",
    "function": "(...any) -> ...any",
    "table": "{ any }",
}


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; overdare-lsp-dump/1.0)"})
    for attempt in range(FETCH_RETRY_ATTEMPTS):
        try:
            with urllib.request.urlopen(request, timeout=20) as resp:
                return resp.read().decode("utf-8")
        except Exception:
            if attempt == FETCH_RETRY_ATTEMPTS - 1:
                raise
            time.sleep(FETCH_RETRY_DELAY_SECONDS)


def get_english_links(llms_text):
    en_start = llms_text.index("## English")
    en_end = llms_text.index("## Korean") if "## Korean" in llms_text else len(llms_text)
    en_section = llms_text[en_start:en_end]
    classes = {name: url for name, url in CLASS_URL_RE.findall(en_section)}
    enums = {name: url for name, url in ENUM_URL_RE.findall(en_section)}
    datatypes = {name: url for name, url in DATATYPE_URL_RE.findall(en_section)}
    return classes, enums, datatypes


MD_ESCAPE_RE = re.compile(r"\\([\\`*_{}\[\]()#+\-.!<>])")


def unescape_md(text):
    """Undo Markdown backslash-escaping (e.g. `L\\_ECC\\_Camera` -> `L_ECC_Camera`)."""
    return MD_ESCAPE_RE.sub(r"\1", text)


def is_identifier(name):
    return re.match(r"^[A-Za-z_]\w*$", name) is not None


def escape_name(name):
    if name == "function":
        return "func"
    if not is_identifier(name):
        return '["' + name.replace('"', '\\"') + '"]'
    return name


def resolve_doc_type(raw):
    raw = raw.strip()
    optional = raw.endswith("?")
    if optional:
        raw = raw[:-1].strip()
    if raw.startswith("Enum."):
        raw = "Enum" + raw[len("Enum."):]
    raw = TYPE_ALIASES.get(raw, raw)
    return raw + ("?" if optional else "")


def split_sections_list(md, marker):
    """Split markdown into [(heading, body), ...] at lines starting with `marker` (e.g. '### ').
    Unlike a dict, this preserves duplicate headings (e.g. overloaded `### new` constructors)."""
    sections = []
    pattern = re.compile(r"^" + re.escape(marker) + r"(.+)$", re.MULTILINE)
    matches = list(pattern.finditer(md))
    for i, m in enumerate(matches):
        name = m.group(1).strip()
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(md)
        sections.append((name, md[start:end]))
    return sections


def split_sections(md, marker):
    """Split markdown into {heading: body} at lines starting with `marker` (e.g. '## ').
    Top-level headings (Overview/Properties/Methods/Events/Constructors/Items) don't repeat
    within a page, so last-write-wins is fine here; use split_sections_list where duplicates
    are expected (member names under a section)."""
    return dict(split_sections_list(md, marker))


def is_separator_row(line):
    cell = line.strip().strip("|")
    return bool(cell) and set(cell.replace("|", "").strip()) <= set("- ")


def parse_table_row_type_name(line):
    m = re.match(r"^\|\s*`([^`]*)`\s*([^|]*?)\s*\|", line)
    if not m:
        return None
    return unescape_md(m.group(1).strip()), unescape_md(m.group(2).strip())


def parse_params(body):
    params = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("|") or is_separator_row(line):
            continue
        parsed = parse_table_row_type_name(line)
        if parsed:
            params.append(parsed)
    return params


def parse_return(body):
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("|") or is_separator_row(line):
            continue
        parsed = parse_table_row_type_name(line)
        if parsed:
            return parsed[0]
    return "void"


def parse_member_block(name, body, kind):
    subsections = split_sections(body, "#### ")
    params = parse_params(subsections["Parameters"]) if "Parameters" in subsections else []
    ret = parse_return(subsections["Return"]) if kind == "method" and "Return" in subsections else "void"
    return {"name": name, "params": params, "return": ret}


def parse_property_block(name, body):
    m = re.search(r"^\s*`([^`]+)`\s*$", body, re.MULTILINE)
    return {"name": unescape_md(name), "type": unescape_md(m.group(1).strip()) if m else "any"}


def parse_members(md):
    """Shared by classes and datatypes: parse ## Properties / ## Methods|Functions / ## Events."""
    sections = split_sections(md, "## ")
    properties, methods, events = [], [], []

    if "Properties" in sections:
        for pname, pbody in split_sections_list(sections["Properties"], "### "):
            properties.append(parse_property_block(pname, pbody))

    for key in ("Methods", "Functions"):
        if key in sections:
            for mname, mbody in split_sections_list(sections[key], "### "):
                methods.append(parse_member_block(unescape_md(mname), mbody, "method"))

    if "Events" in sections:
        for ename, ebody in split_sections_list(sections["Events"], "### "):
            events.append(parse_member_block(unescape_md(ename), ebody, "event"))

    return sections, properties, methods, events


def parse_class_page(md):
    name_m = re.search(r"^# (.+)$", md, re.MULTILINE)
    name = unescape_md(name_m.group(1).strip()) if name_m else None

    parent = None
    if name:
        parent_m = re.search(r"^" + re.escape(name) + r"\s*:\s*`([^`]+)`", md, re.MULTILINE)
        if parent_m:
            parent = unescape_md(parent_m.group(1).strip())

    _sections, properties, methods, events = parse_members(md)

    return {"name": name, "parent": parent, "properties": properties, "methods": methods, "events": events}


def parse_datatype_page(md):
    name_m = re.search(r"^# (.+)$", md, re.MULTILINE)
    name = unescape_md(name_m.group(1).strip()) if name_m else None

    sections, properties, methods, events = parse_members(md)

    # Constructors aren't only named "new" - e.g. Color3.fromRGB, CFrame.fromEulerAnglesXYZ/
    # lookAt are separate named static constructors alongside "new" overloads.
    constructors = []
    if "Constructors" in sections:
        for cname, cbody in split_sections_list(sections["Constructors"], "### "):
            subsections = split_sections(cbody, "#### ")
            params = parse_params(subsections["Parameters"]) if "Parameters" in subsections else []
            constructors.append((unescape_md(cname), params))

    # The docs lump both real instance properties (Magnitude, X, Unit, ...) and static
    # namespace constants (identity, zero, one, xAxis, ...) under the same "## Properties"
    # heading with no distinguishing tag. Matching the real Roblox/OVERDARE convention
    # (see e.g. `declare Vector3: { zero: Vector3, one: Vector3, xAxis: Vector3, ..., new: (...) }`),
    # lowercase-leading names are static constants that belong on the global namespace table
    # alongside constructors, not inside `declare extern type X with ... end`.
    instance_properties = [p for p in properties if not p["name"] or not p["name"][0].islower()]
    static_properties = [p for p in properties if p["name"] and p["name"][0].islower()]

    return {
        "name": name,
        "properties": instance_properties,
        "staticProperties": static_properties,
        "methods": methods,
        "events": events,
        "constructors": constructors,
    }


def parse_enum_page(md):
    name_m = re.search(r"^# (.+)$", md, re.MULTILINE)
    name = unescape_md(name_m.group(1).strip()) if name_m else None

    items_section = split_sections(md, "## ").get("Items", "")
    items = []
    for line in items_section.splitlines():
        line = line.strip()
        if not line.startswith("|") or is_separator_row(line):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 2 or cells[0] == "Name":
            continue
        items.append(unescape_md(cells[0]))

    return {"name": name, "items": items}


def scrape_all(log=print):
    log(f"Fetching {LLMS_TXT_URL}")
    llms_text = fetch(LLMS_TXT_URL)
    class_links, enum_links, datatype_links = get_english_links(llms_text)
    log(f"Found {len(class_links)} classes, {len(enum_links)} enums, {len(datatype_links)} datatypes")

    classes = []
    for i, (name, url) in enumerate(sorted(class_links.items())):
        log(f"[{i + 1}/{len(class_links)}] class {name}")
        classes.append(parse_class_page(fetch(url + ".md" if not url.endswith(".md") else url)))

    enums = []
    for i, (name, url) in enumerate(sorted(enum_links.items())):
        log(f"[{i + 1}/{len(enum_links)}] enum {name}")
        enums.append(parse_enum_page(fetch(url + ".md" if not url.endswith(".md") else url)))

    # Enum/EnumItem are also listed as "datatypes" in the docs, but they're foundational
    # plumbing types handled specially elsewhere (every Enum* class extends EnumItem, and
    # ENUM_LIST is built from Enum) - scraping and replacing them here is not safe.
    #
    # ScriptConnection/ScriptSignal are also listed as datatypes, but every event property
    # (declare_event(), above) needs ScriptSignal to be generic over the event's argument
    # types (e.g. `Changed: ScriptSignal<string>`), which the generic datatype scraper here
    # can't produce (it always emits a plain non-generic `declare extern type`). A
    # hand-written generic version is hardcoded into the merged output instead - see
    # generate_luau()'s standalone-stub block and scripts/globalTypes.d.luau directly.
    # Scraping and adding a second, non-generic ScriptSignal/ScriptConnection here would
    # collide with that hardcoded pair (duplicate `declare extern type` for the same name
    # crashes the Luau frontend on load).
    datatype_links = {name: url for name, url in datatype_links.items() if name not in ("Enum", "EnumItem", "ScriptConnection", "ScriptSignal")}

    datatypes = []
    for i, (name, url) in enumerate(sorted(datatype_links.items())):
        log(f"[{i + 1}/{len(datatype_links)}] datatype {name}")
        datatypes.append(parse_datatype_page(fetch(url + ".md" if not url.endswith(".md") else url)))

    return {"classes": classes, "enums": enums, "datatypes": datatypes}


# ---- Luau declaration generation (standalone-testable output) ----

# docs.overdare.com's datatype constructor tables never mark a parameter as optional - no
# trailing '?' in the Type column, no separate Default column. When a parameter does have a
# default, it's only ever mentioned in free-form Description prose (e.g. CFrame.lookAt's `up`:
# "if omitted, the default (0, 1, 0) is used"), which isn't machine-parseable. That silently
# turned genuinely-optional parameters into required ones once a scraped page replaced the
# Roblox-derived (correctly-optional) constructor it was based on (CFrame.lookAt's `up`,
# every TweenInfo.new parameter). Manually confirmed against Roblox's original API dump /
# docs.overdare.com prose; keyed by
# (datatype name, constructor name, parameter name). Extend this if another constructor regresses.
DATATYPE_CONSTRUCTOR_OPTIONAL_PARAMS = {
    ("CFrame", "lookAt", "up"),
    ("TweenInfo", "new", "InTime"),
    ("TweenInfo", "new", "InEasingStyle"),
    ("TweenInfo", "new", "InEasingDirection"),
    ("TweenInfo", "new", "InRepeatCount"),
    ("TweenInfo", "new", "InReverses"),
    ("TweenInfo", "new", "InDelayTime"),
}


def declare_property(prop):
    return f"\t{escape_name(prop['name'])}: {resolve_doc_type(prop['type'])}\n"


def declare_param(param_type, param_name, force_optional=False):
    resolved = resolve_doc_type(param_type)
    if resolved.startswith("..."):
        return "...: " + resolved[3:]  # e.g. "...any" -> "...: any" (variadic param syntax)
    if force_optional and not resolved.endswith("?"):
        resolved += "?"
    return f"{escape_name(param_name) if param_name else 'arg'}: {resolved}"


def declare_method(method):
    params = ", ".join(declare_param(t, n) for t, n in method["params"])
    prefix = ", " if params else ""
    ret = resolve_doc_type(method["return"])
    return f"\tfunction {escape_name(method['name'])}(self{prefix}{params}): {ret}\n"


def declare_event(event):
    types = [resolve_doc_type(t) for t, _n in event["params"]]
    type_list = ", ".join(types)
    return f"\t{escape_name(event['name'])}: ScriptSignal<{type_list}>\n"


def declare_class(klass):
    if not klass["name"]:
        return ""
    out = f"declare extern type {klass['name']}"
    if klass["parent"]:
        out += f" extends {klass['parent']}"
    out += " with\n"
    for prop in klass["properties"]:
        out += declare_property(prop)
    for method in klass["methods"]:
        out += declare_method(method)
    for event in klass["events"]:
        out += declare_event(event)
    out += "end\n"
    return out


def declare_datatype(dt):
    if not dt["name"]:
        return ""
    out = f"declare extern type {dt['name']} with\n"
    for prop in dt["properties"]:
        out += declare_property(prop)
    for method in dt["methods"]:
        out += declare_method(method)
    for event in dt["events"]:
        out += declare_event(event)
    out += "end\n"
    return out


def declare_datatype_constructor(dt):
    """The `declare Name: { new: (...) -> Name, fromRGB: (...) -> Name, zero: Name, ... }`
    global namespace table, matching the real scripts/globalTypes.d.luau convention (e.g.
    `declare Vector3: { zero: Vector3, one: Vector3, xAxis: Vector3, ..., new: (...) }` -
    constructors aren't only ever named "new", and static constants like `.zero`/`.identity`
    live here too, not as instance properties). Skipped entirely if the docs listed neither.

    Some constructor names (e.g. CFrame's "new") have multiple documented overloads with
    different parameter lists - a Luau table type can't repeat a key, so multiple entries
    with the same name must be joined into one `&`-intersected overloaded function, not
    emitted as separate `name: (...)` lines (which silently collapses to just the last one,
    shadowing every other overload)."""
    if not dt["name"] or (not dt["constructors"] and not dt.get("staticProperties")):
        return ""
    out = f"declare {dt['name']}: {{\n"
    for prop in dt.get("staticProperties", []):
        out += f"\t{escape_name(prop['name'])}: {resolve_doc_type(prop['type'])},\n"

    overloads_by_name = {}
    for ctor_name, params in dt["constructors"]:
        param_list = ", ".join(
            declare_param(t, n, force_optional=(dt["name"], ctor_name, n) in DATATYPE_CONSTRUCTOR_OPTIONAL_PARAMS)
            for t, n in params
        )
        overloads_by_name.setdefault(ctor_name, []).append(f"(({param_list}) -> {dt['name']})")

    for ctor_name, overloads in overloads_by_name.items():
        out += f"\t{escape_name(ctor_name)}: {' & '.join(overloads)},\n"
    out += "}\n"
    return out


def declare_enum(enum):
    name = enum["name"]
    out = f"declare extern type Enum{name} extends EnumItem with end\n"
    out += f"declare extern type Enum{name}_INTERNAL extends Enum with\n"
    for item in sorted(enum["items"]):
        out += f"\t{escape_name(item)}: Enum{name}\n"
    out += f"\tfunction GetEnumItems(self): {{ Enum{name} }}\n"
    out += f"\tfunction FromName(self, Name: string): Enum{name}?\n"
    out += f"\tfunction FromValue(self, Value: number): Enum{name}?\n"
    out += "end\n"
    return out


def collect_referenced_types(dump):
    declared = {"Enum", "EnumItem"} | {c["name"] for c in dump["classes"] if c["name"]}
    declared |= {d["name"] for d in dump.get("datatypes", []) if d["name"]}
    declared |= {f"Enum{e['name']}" for e in dump["enums"]}
    declared |= {f"Enum{e['name']}_INTERNAL" for e in dump["enums"]}

    referenced = set()

    def note(raw):
        t = resolve_doc_type(raw).rstrip("?")
        referenced.add(t)

    def scan_members(entity, has_parent=False):
        for prop in entity["properties"]:
            note(prop["type"])
        for method in entity["methods"]:
            note(method["return"])
            for t, _n in method["params"]:
                note(t)
        for event in entity["events"]:
            for t, _n in event["params"]:
                note(t)
        if has_parent and entity["parent"]:
            referenced.add(entity["parent"])

    for klass in dump["classes"]:
        scan_members(klass, has_parent=True)
    for dt in dump.get("datatypes", []):
        scan_members(dt)
        for prop in dt.get("staticProperties", []):
            note(prop["type"])
        for _ctor_name, params in dt["constructors"]:
            for t, _n in params:
                note(t)

    candidates = referenced - declared - LUAU_PRIMITIVES - {"ScriptSignal", "...any"}
    return sorted(name for name in candidates if is_identifier(name))


def generate_luau(dump):
    out = []
    out.append("-- AUTO-GENERATED by scripts/dumpOverdareTypes.py - do not edit by hand.\n")
    out.append("-- Standalone stub definitions for types referenced but not scraped from OVERDARE docs\n")
    out.append("-- (these already exist for real in scripts/globalTypes.d.luau once merged).\n")
    out.append(
        "export type ScriptSignal<T... = ...any> = {\n"
        "\tWait: (self: ScriptSignal<T...>) -> T...,\n"
        "\tConnect: (self: ScriptSignal<T...>, callback: (T...) -> ()) -> ScriptConnection,\n"
        "}\n"
    )
    out.append("declare extern type ScriptConnection with\n\tfunction Disconnect(self): nil\nend\n")

    stub_names = collect_referenced_types(dump)
    for name in stub_names:
        out.append(f"declare extern type {name} with end\n")

    out.append("declare extern type Enum with\n\tfunction GetEnumItems(self): { any }\nend\n")
    out.append("declare extern type EnumItem with\n\tName: string\n\tValue: number\nend\n")

    for enum in dump["enums"]:
        out.append(declare_enum(enum))

    enum_list = "type ENUM_LIST = {\n"
    for enum in dump["enums"]:
        enum_list += f"\t{enum['name']}: Enum{enum['name']}_INTERNAL,\n"
    enum_list += "} & { GetEnums: (self: ENUM_LIST) -> { Enum } }\n"
    enum_list += "declare Enum: ENUM_LIST\n"
    out.append(enum_list)

    for dt in dump.get("datatypes", []):
        out.append(declare_datatype(dt))
        out.append(declare_datatype_constructor(dt))

    for klass in topo_sort_classes(dump["classes"]):
        out.append(declare_class(klass))

    return "\n".join(out)


def topo_sort_classes(classes):
    """Order classes so a parent is always declared before any class that extends it
    (Luau's `declare extern type` files are resolved in a single top-to-bottom pass)."""
    by_name = {c["name"]: c for c in classes if c["name"]}
    ordered = []
    visited = set()

    def visit(klass):
        if klass["name"] in visited:
            return
        visited.add(klass["name"])
        parent = by_name.get(klass["parent"])
        if parent:
            visit(parent)
        ordered.append(klass)

    for klass in classes:
        visit(klass)

    return ordered


# ---- Merging into the existing scripts/globalTypes.d.luau (Roblox-sourced) ----
#
# Strategy: OVERDARE classes/enums that share a name with an existing Roblox-sourced
# declaration REPLACE it in place (same file position, to preserve forward-reference
# ordering for anything declared after it that isn't itself being replaced). OVERDARE-only
# classes/enums are APPENDED at the end, topologically sorted among themselves (their
# parents, if also new, must come first; if the parent already exists in the base file,
# no ordering constraint is needed since it's declared earlier).

CLASS_BLOCK_START_RE = re.compile(r"^declare extern type (\w+)(?:\s+extends\s+(\w+))?\s+with(\s+end)?\s*$")


def find_named_blocks(lines):
    """Find `declare extern type NAME ... end` blocks by line range. Returns {name: (start, end)}
    with `end` inclusive. Assumes blocks don't nest (true for this codebase's generated files)."""
    return {name: (info["start"], info["end"]) for name, info in find_named_blocks_with_parent(lines).items()}


def find_named_blocks_with_parent(lines):
    """Like find_named_blocks, but also captures each block's `extends` parent (or None)."""
    blocks = {}
    i = 0
    while i < len(lines):
        m = CLASS_BLOCK_START_RE.match(lines[i])
        if m:
            name, parent, oneline = m.group(1), m.group(2), m.group(3)
            if oneline:  # single-line "... with end"
                blocks[name] = {"start": i, "end": i, "parent": parent}
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != "end":
                j += 1
            blocks[name] = {"start": i, "end": j, "parent": parent}
            i = j + 1
        else:
            i += 1
    return blocks


def fix_class_ordering(lines, log=print):
    """Repeatedly relocate any class block that's declared before its `extends` parent to
    immediately after that parent's block, until the whole file is forward-reference-clean
    (or an iteration cap is hit, which would indicate a genuine cycle)."""
    for iteration in range(1000):
        blocks = find_named_blocks_with_parent(lines)
        violation = None
        for name, info in blocks.items():
            parent = info["parent"]
            if parent and parent in blocks and blocks[parent]["start"] > info["start"]:
                violation = (name, info, parent, blocks[parent])
                break
        if not violation:
            if iteration > 0:
                log(f"[ok] fixed class ordering in {iteration} move(s)")
            return lines

        name, info, parent, parent_info = violation
        block_lines = lines[info["start"]: info["end"] + 1]
        # Remove the violating block from its current position.
        lines = lines[: info["start"]] + lines[info["end"] + 1:]
        # Re-locate the parent's end position in the post-removal list.
        parent_end = parent_info["end"] if parent_info["end"] < info["start"] else parent_info["end"] - len(block_lines)
        insert_at = parent_end + 1
        lines = lines[:insert_at] + block_lines + lines[insert_at:]

    log("[warn] fix_class_ordering did not converge after 1000 iterations - possible extends cycle, leaving as-is")
    return lines


def replace_line_ranges(lines, replacements):
    """replacements: {(start, end): [new_lines]}, ranges inclusive and non-overlapping.
    Returns a new line list with each range swapped for its replacement."""
    result = []
    i = 0
    starts = {start: (end, new_lines) for (start, end), new_lines in replacements.items()}
    while i < len(lines):
        if i in starts:
            end, new_lines = starts[i]
            result.extend(new_lines)
            i = end + 1
        else:
            result.append(lines[i])
            i += 1
    return result


CONSTRUCTOR_BLOCK_START_RE = re.compile(r"^declare (\w+): \{$")


def find_constructor_blocks(lines):
    """Find `declare Name: { new: (...) -> Name, ... }` global-constructor blocks by line range."""
    blocks = {}
    i = 0
    while i < len(lines):
        m = CONSTRUCTOR_BLOCK_START_RE.match(lines[i])
        if m:
            name = m.group(1)
            j = i + 1
            while j < len(lines) and lines[j] != "}":
                j += 1
            blocks[name] = (i, j)
            i = j + 1
        else:
            i += 1
    return blocks


EXPORT_TYPE_NAME_RE = re.compile(r"^export type (\w+)")


def find_export_type_names(lines):
    """Names already defined via `export type NAME = ...` (possibly generic, e.g.
    `RaycastResult<T = BasePart>`) rather than `declare extern type`. These aren't
    detected by find_named_blocks and must not be duplicated - Luau's checker segfaults
    on a name declared both ways."""
    names = set()
    for line in lines:
        m = EXPORT_TYPE_NAME_RE.match(line)
        if m:
            names.add(m.group(1))
    return names


METADATA_PREFIX = "--#METADATA#"


# OVERDARE-only classes that never existed in Roblox at all, so intersecting the inherited
# Roblox whitelist below can never surface them even though OVERDARE's own docs confirm
# they're Instance.new()-creatable (see
# https://docs.overdare.com/manual/studio-manual/object/outline-fill, which shows
# `Instance.new("Outline")`/`Instance.new("Fill")` - unlike their own class pages, which
# document properties only and never mention construction).
EXTRA_CREATABLE_INSTANCES = {"Fill", "Outline"}


def prune_services_metadata(lines, dump, log=print):
    """The `SERVICES` and `CREATABLE_INSTANCES` lists in the `--#METADATA#` header line
    (used by GetService's and Instance.new's magic functions for both validation and
    autocomplete) are inherited verbatim from Roblox's Full-API-Dump and contain hundreds
    of Roblox-only class/service names OVERDARE doesn't have - OVERDARE's docs have no
    equivalent dump, so prune both down to the intersection with our scraped class list
    (a name with no matching OVERDARE class can't be real)."""
    if not lines or not lines[0].startswith(METADATA_PREFIX):
        log("[warn] no --#METADATA# header line found - not pruned")
        return lines

    meta = json.loads(lines[0][len(METADATA_PREFIX):])
    od_class_names = {c["name"] for c in dump["classes"] if c["name"]}
    od_enum_names = {e["name"] for e in dump.get("enums", []) if e["name"]}

    for key, label in (("SERVICES", "Services"), ("CREATABLE_INSTANCES", "Creatable instances")):
        if key not in meta:
            continue
        before = len(meta[key])
        pruned = set(meta[key]) & od_class_names
        if key == "CREATABLE_INSTANCES":
            pruned |= EXTRA_CREATABLE_INSTANCES & od_class_names
        meta[key] = sorted(pruned)
        log(f"{label} (metadata whitelist): pruned {before} -> {len(meta[key])} "
            f"(kept only names that match a scraped OVERDARE class)")

    # CLASSES: every real OVERDARE class, used by OverdareCompletion.cpp to filter
    # IsA/FindFirstChildOfClass/etc. autocomplete down from "every Instance-derived type
    # still declared in the file" (including thousands of untouched Roblox classes) to just
    # the ones that actually exist in OVERDARE.
    meta["CLASSES"] = sorted(od_class_names)
    log(f"Classes (metadata whitelist for IsA/FindFirstChildOfClass completion): {len(meta['CLASSES'])} entries")

    # ENUMS: every real OVERDARE enum, same purpose as CLASSES but for EnumX type-name
    # completion (e.g. `local x: EnumFoo`), which isn't covered by the ENUM_LIST prune below
    # since that only fixes `Enum.Foo` member access, not standalone type annotations.
    meta["ENUMS"] = sorted(od_enum_names)
    log(f"Enums (metadata whitelist for EnumX type-annotation completion): {len(meta['ENUMS'])} entries")

    lines[0] = METADATA_PREFIX + json.dumps(meta)
    return lines


def prune_enum_list(text, dump, log=print):
    """`declare Enum: ENUM_LIST` (the `Enum.Foo` global table) is inherited verbatim from
    Roblox's dump and, unlike SERVICES/CREATABLE_INSTANCES/CLASSES, was never pruned by the
    per-enum block replacement above (that only replaces/appends matching entries, it never
    removes stale ones) - so `Enum.` autocomplete was suggesting hundreds of Roblox-only enum
    names. Rewrite the whole entry list to just the real OVERDARE enums."""
    od_enum_names = sorted({e["name"] for e in dump.get("enums", []) if e["name"]})

    m = re.search(r"type ENUM_LIST = \{\n(.*?)\n\} & \{ GetEnums:", text, re.DOTALL)
    if not m:
        log("[warn] could not find 'type ENUM_LIST = { ... }' block - not pruned")
        return text

    before = m.group(1).count("\n") + 1
    new_entries = "".join(f"\t{name}: Enum{name}_INTERNAL,\n" for name in od_enum_names)
    log(f"Enum global (ENUM_LIST table): pruned {before} -> {len(od_enum_names)} entries")
    return text[: m.start(1)] + new_entries.rstrip("\n") + text[m.end(1) :]


OPERATOR_METHOD_RE = re.compile(r"^\tfunction (__\w+)\(")


def preserve_operator_overloads(new_block_lines, lines, start, end):
    """docs.overdare.com's class/datatype pages never document metamethod operator overloads
    (__add, __sub, __mul, __div, __unm, ...) - Vector3/Vector2/CFrame all lost their
    Roblox-inherited arithmetic operators this way when their scraped page replaced the whole
    block. Carry over any `function __foo(...)` lines still present in the block being
    replaced so a re-scrape can't silently regress arithmetic support again."""
    preserved = [line for line in lines[start : end + 1] if OPERATOR_METHOD_RE.match(line)]
    if not preserved:
        return new_block_lines
    return new_block_lines[:-1] + preserved + new_block_lines[-1:]


def merge_into_base(base_text, dump, log=print):
    lines = base_text.split("\n")
    lines = prune_services_metadata(lines, dump, log=log)
    class_blocks = find_named_blocks(lines)
    export_type_names = find_export_type_names(lines)

    od_classes_by_name = {c["name"]: c for c in dump["classes"] if c["name"]}
    replaced_classes, new_classes, skipped_export_type = [], [], []
    class_replacements = {}
    for klass in dump["classes"]:
        if not klass["name"]:
            continue
        if klass["name"] in class_blocks:
            start, end = class_blocks[klass["name"]]
            new_block = declare_class(klass).rstrip("\n").split("\n")
            class_replacements[(start, end)] = preserve_operator_overloads(new_block, lines, start, end)
            replaced_classes.append(klass["name"])
        elif klass["name"] in export_type_names:
            skipped_export_type.append(klass["name"])
        else:
            new_classes.append(klass)

    datatypes = dump.get("datatypes", [])
    replaced_datatypes, new_datatypes = [], []
    for dt in datatypes:
        if not dt["name"]:
            continue
        if dt["name"] in class_blocks:
            start, end = class_blocks[dt["name"]]
            new_block = declare_datatype(dt).rstrip("\n").split("\n")
            class_replacements[(start, end)] = preserve_operator_overloads(new_block, lines, start, end)
            replaced_datatypes.append(dt["name"])
        elif dt["name"] in export_type_names:
            skipped_export_type.append(dt["name"])
        else:
            new_datatypes.append(dt)

    new_class_names = {c["name"] for c in new_classes}

    def visit_new(klass, visited, ordered):
        if klass["name"] in visited:
            return
        visited.add(klass["name"])
        parent = od_classes_by_name.get(klass["parent"])
        if parent and parent["name"] in new_class_names:
            visit_new(parent, visited, ordered)
        ordered.append(klass)

    ordered_new_classes = []
    visited = set()
    for klass in new_classes:
        visit_new(klass, visited, ordered_new_classes)

    lines = replace_line_ranges(lines, class_replacements)

    # Re-find blocks after the splice (line numbers shifted) to locate enum blocks + ENUM_LIST.
    enum_atom_blocks = find_named_blocks(lines)  # keys like "EnumFoo", "EnumFoo_INTERNAL"

    od_enums_new, od_enums_replaced = [], []
    enum_replacements = {}
    for enum in dump["enums"]:
        atom_name = f"Enum{enum['name']}"
        internal_name = f"{atom_name}_INTERNAL"
        if atom_name in enum_atom_blocks and internal_name in enum_atom_blocks:
            atom_start, atom_end = enum_atom_blocks[atom_name]
            internal_start, internal_end = enum_atom_blocks[internal_name]
            enum_replacements[(atom_start, atom_end)] = [f"declare extern type {atom_name} extends EnumItem with end"]
            new_internal_lines = declare_enum(enum).rstrip("\n").split("\n")
            # declare_enum emits both the atom and _INTERNAL block; keep only the _INTERNAL part here
            internal_idx = next(i for i, l in enumerate(new_internal_lines) if l.startswith(f"declare extern type {internal_name}"))
            enum_replacements[(internal_start, internal_end)] = new_internal_lines[internal_idx:]
            od_enums_replaced.append(enum["name"])
        else:
            od_enums_new.append(enum)

    lines = replace_line_ranges(lines, enum_replacements)

    # Re-find constructor blocks (`declare Name: { new: ... }`) after the splices above.
    constructor_blocks = find_constructor_blocks(lines)
    constructor_replacements = {}
    datatypes_with_new_constructor = []
    for dt in datatypes:
        if not dt["constructors"] and not dt.get("staticProperties"):
            continue
        block = declare_datatype_constructor(dt).rstrip("\n").split("\n")
        if dt["name"] in constructor_blocks:
            start, end = constructor_blocks[dt["name"]]
            constructor_replacements[(start, end)] = block
        else:
            datatypes_with_new_constructor.append(dt)
    lines = replace_line_ranges(lines, constructor_replacements)

    merged_text = "\n".join(lines)

    # Add extern type declarations for brand-new enums; ENUM_LIST itself is fully rebuilt
    # below by prune_enum_list rather than patched incrementally here.
    if od_enums_new:
        new_enum_blocks = "\n".join(declare_enum(e).rstrip("\n") for e in od_enums_new)
        merged_text += "\n" + new_enum_blocks + "\n"

    merged_text = prune_enum_list(merged_text, dump, log=log)

    if ordered_new_classes:
        new_class_blocks = "\n".join(declare_class(k).rstrip("\n") for k in ordered_new_classes)
        merged_text += "\n" + new_class_blocks + "\n"

    if new_datatypes:
        new_dt_blocks = "\n".join(declare_datatype(dt).rstrip("\n") for dt in new_datatypes)
        merged_text += "\n" + new_dt_blocks + "\n"

    if datatypes_with_new_constructor:
        new_ctor_blocks = "\n".join(declare_datatype_constructor(dt).rstrip("\n") for dt in datatypes_with_new_constructor)
        merged_text += "\n" + new_ctor_blocks + "\n"

    log(f"Classes: {len(replaced_classes)} replaced, {len(ordered_new_classes)} appended as new")
    log(f"Enums: {len(od_enums_replaced)} replaced, {len(od_enums_new)} appended as new")
    log(f"Datatypes: {len(replaced_datatypes)} replaced, {len(new_datatypes)} appended as new "
        f"({len(datatypes_with_new_constructor)} new constructor tables)")
    if skipped_export_type:
        log(f"[warn] {len(skipped_export_type)} names already defined via 'export type' in the base file "
            f"were left untouched (not replaced or duplicated): {', '.join(sorted(skipped_export_type))}")

    # Safety net: a handful of OVERDARE doc types are referenced but have no page of their own
    # (e.g. TeleportResult) - genuine gaps in the docs, not a bug here. Stub them out rather
    # than leaving an "Unknown type" error, and say so loudly.
    base_declared_names = set(class_blocks) | export_type_names
    all_referenced = set(collect_referenced_types({"classes": dump["classes"], "datatypes": datatypes, "enums": []}))
    od_emitted_names = (
        {c["name"] for c in dump["classes"] if c["name"]}
        | {dt["name"] for dt in datatypes if dt["name"]}
        | {f"Enum{e['name']}" for e in dump["enums"]}
        | {f"Enum{e['name']}_INTERNAL" for e in dump["enums"]}
    )
    truly_missing = sorted(all_referenced - base_declared_names - od_emitted_names)
    if truly_missing:
        log(f"[warn] {len(truly_missing)} referenced type(s) have no definition anywhere (docs gap) - "
            f"stubbing as empty: {', '.join(truly_missing)}")
        stub_blocks = "\n".join(f"declare extern type {name} with end" for name in truly_missing)
        merged_text += "\n" + stub_blocks + "\n"

    lines = fix_class_ordering(merged_text.split("\n"), log=log)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump-json", help="Write the scraped raw data as JSON to this path")
    parser.add_argument("--output", help="Write generated standalone Luau definitions to this path")
    parser.add_argument("--from-json", help="Skip scraping, load raw data from a previously written --dump-json file")
    parser.add_argument("--merge-base", help="Merge scraped OVERDARE types into this existing globalTypes.d.luau-style file")
    parser.add_argument("--merged-output", help="Where to write the merged file (required with --merge-base)")
    args = parser.parse_args()

    if args.from_json:
        with open(args.from_json, "r", encoding="utf-8") as f:
            dump = json.load(f)
    else:
        dump = scrape_all(log=lambda msg: print(msg, file=sys.stderr))

    if args.dump_json:
        with open(args.dump_json, "w", encoding="utf-8") as f:
            json.dump(dump, f, indent=2)
        print(f"Wrote {args.dump_json}", file=sys.stderr)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(generate_luau(dump))
        print(f"Wrote {args.output}", file=sys.stderr)

    if args.merge_base:
        if not args.merged_output:
            parser.error("--merge-base requires --merged-output")
        with open(args.merge_base, "r", encoding="utf-8") as f:
            base_text = f.read()
        merged = merge_into_base(base_text, dump, log=lambda msg: print(msg, file=sys.stderr))
        with open(args.merged_output, "w", encoding="utf-8") as f:
            f.write(merged)
        print(f"Wrote {args.merged_output}", file=sys.stderr)


if __name__ == "__main__":
    main()
