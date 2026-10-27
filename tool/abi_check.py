#!/usr/bin/env python3
"""Checks lychee.h against older ones: an ABI change needs a bump.

  abi_check.py          the working tree's header against the latest v* tag
                        reachable from HEAD and against where this branch
                        left main (so uncommitted edits count)
  abi_check.py BASE     against the latest v* tag and where HEAD left BASE
                        (CI: the push's or pull request's base; none when
                        BASE is empty, all zeros or not related)

Fails when the header changed the ABI since one of them and
LYCHEE_API_VERSION is not higher than that one's (or has no "// N:"
history line). Against the release alone a break would pass once anything
since had raised the version; against the base each change needs its own
bump, and a push with no base still needs one over the release.

A break is any of: a struct, typedef, function, enum constant or macro
gone; a struct field moved, retyped or gone, or a new one before the last
old one; a struct that grew without starting with struct_size, or shrank;
a function's or typedef's type changed; an enum constant's or macro's
value changed. Additions (new declarations, new fields at the end of a
struct that starts with struct_size) are changes too, since callers test
the version to use them. The breaks are listed even when the version was
bumped, as the release's callers break.

clang (CLANG, default clang) reads both headers (-ast-dump=json) and
compiles a probe against each that prints the sizes and field offsets of
the structs and the values of the enum constants and macros, on the host's
ABI. Needs git, python3 and clang, nothing else.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HEADER = "core/include/lychee/lychee.h"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLANG = os.environ.get("CLANG", "clang")
# Macros that are not part of the ABI.
IGNORED_MACROS = {"LYCHEE_LYCHEE_H_", "LYCHEE_EXPORT"}


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          **kwargs).stdout


def api(include_dir):
    """The declarations of include_dir/lychee/lychee.h, with their layout."""
    header = os.path.join(include_dir, "lychee", "lychee.h")
    ast = json.loads(run([CLANG, "-x", "c", "-std=c99", "-fsyntax-only",
                          "-Xclang", "-ast-dump=json", header]))
    structs, typedefs, functions, constants = {}, {}, {}, []
    for decl in ast.get("inner", []):
        name = decl.get("name", "")
        if not name.lower().startswith("lychee_"):
            continue
        kind = decl["kind"]
        if kind == "RecordDecl" and decl.get("completeDefinition"):
            structs[name] = [(f["name"], f["type"]["qualType"])
                             for f in decl.get("inner", [])
                             if f["kind"] == "FieldDecl"]
        elif kind == "TypedefDecl":
            typedefs[name] = decl["type"]["qualType"]
        elif kind == "FunctionDecl":
            functions[name] = decl["type"]["qualType"]
        elif kind == "EnumDecl":
            constants += [c["name"] for c in decl.get("inner", [])
                          if c["kind"] == "EnumConstantDecl"]
    macros = {}
    for line in run([CLANG, "-x", "c", "-std=c99", "-E", "-dM",
                     header]).splitlines():
        m = re.match(r"#define (LYCHEE_\w+)(?:\s+(.*))?$", line)
        if m and m.group(1) not in IGNORED_MACROS:
            macros[m.group(1)] = (m.group(2) or "").strip()

    # The layout and values as the compiler sees them.
    lines = []
    for struct, fields in structs.items():
        lines.append(f'printf("size {struct} %zu\\n", sizeof(struct {struct}));')
        for field, _ in fields:
            lines.append(f'printf("offset {struct}.{field} %zu\\n", '
                         f'offsetof(struct {struct}, {field}));')
    for name in constants + [m for m, v in macros.items()
                             if re.fullmatch(r"[-+()\w\s<|]*", v) and v]:
        lines.append(f'printf("value {name} %lld\\n", (long long)({name}));')
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w") as f:
            f.write("#include <stddef.h>\n#include <stdio.h>\n"
                    "#include <lychee/lychee.h>\n"
                    "int main(void) {\n  " + "\n  ".join(lines) +
                    "\n  return 0;\n}\n")
        binary = os.path.join(tmp, "probe")
        run([CLANG, "-std=c99", "-I", include_dir, probe, "-o", binary])
        layout = {}
        for line in run([binary]).splitlines():
            what, key, number = line.split(" ")
            layout[(what, key)] = int(number)
    return {"structs": structs, "typedefs": typedefs, "functions": functions,
            "constants": constants, "macros": macros, "layout": layout,
            "text": open(header).read()}


def compare(old, new):
    """(breaks, additions): lists of human-readable lines."""
    breaks, additions = [], []
    lay_old, lay_new = old["layout"], new["layout"]

    for struct, fields in old["structs"].items():
        if struct not in new["structs"]:
            breaks.append(f"struct {struct} is gone")
            continue
        new_fields = dict(new["structs"][struct])
        for field, type_ in fields:
            key = ("offset", f"{struct}.{field}")
            if field not in new_fields:
                breaks.append(f"{struct}.{field} is gone")
            elif new_fields[field] != type_:
                breaks.append(f"{struct}.{field}: {type_} -> {new_fields[field]}")
            elif lay_old[key] != lay_new[key]:
                breaks.append(f"{struct}.{field} moved: offset "
                              f"{lay_old[key]} -> {lay_new[key]}")
        old_names = {f for f, _ in fields}
        names = [f for f, _ in new["structs"][struct]]
        added = [f for f in names if f not in old_names]
        # struct_size tells which fields a caller has only if new ones come
        # last, even where one fits into padding without moving the others.
        kept = [i for i, f in enumerate(names) if f in old_names]
        inserted = [f for i, f in enumerate(names)
                    if f in added and kept and i < kept[-1]]
        if inserted:
            breaks.append(f"{struct}: {', '.join(inserted)} before the end")
            added = [f for f in added if f not in inserted]
        size_old = lay_old[("size", struct)]
        size_new = lay_new[("size", struct)]
        grows = fields and fields[0][0] == "struct_size"
        if size_new < size_old or (size_new > size_old and not grows):
            breaks.append(f"sizeof({struct}): {size_old} -> {size_new}")
        if added:
            additions.append(f"{struct}: {', '.join(added)} "
                             f"(size {size_old} -> {size_new})")
    for struct in new["structs"].keys() - old["structs"].keys():
        additions.append(f"struct {struct}")

    for kind in ("typedefs", "functions"):
        label = kind[:-1]
        for name, type_ in old[kind].items():
            if name not in new[kind]:
                breaks.append(f"{label} {name} is gone")
            elif new[kind][name] != type_:
                breaks.append(f"{label} {name}: {type_} -> {new[kind][name]}")
        for name in sorted(new[kind].keys() - old[kind].keys()):
            additions.append(f"{label} {name}: {new[kind][name]}")

    for name in old["constants"]:
        key = ("value", name)
        if name not in new["constants"]:
            breaks.append(f"enum constant {name} is gone")
        elif lay_old[key] != lay_new[key]:
            breaks.append(f"{name}: {lay_old[key]} -> {lay_new[key]}")
    for name in new["constants"]:
        if name not in old["constants"]:
            additions.append(f"enum constant {name} = {lay_new[('value', name)]}")

    for name, value in old["macros"].items():
        if name == "LYCHEE_API_VERSION":
            continue
        if name not in new["macros"]:
            breaks.append(f"macro {name} is gone")
        elif new["macros"][name] != value:
            breaks.append(f"macro {name}: {value} -> {new['macros'][name]}")
    for name in sorted(new["macros"].keys() - old["macros"].keys()):
        additions.append(f"macro {name} {new['macros'][name]}")
    return breaks, additions


def git(*args):
    """git's output, or None if it fails."""
    try:
        return run(["git", *args]).strip()
    except subprocess.CalledProcessError:
        return None


def check(ref, new):
    """Prints the ABI changes since ref; 1 if they need a bump, else 0."""
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "lychee"))
        with open(os.path.join(tmp, "lychee", "lychee.h"), "w") as f:
            f.write(run(["git", "show", f"{ref}:{HEADER}"]))
        old = api(tmp)
    breaks, additions = compare(old, new)

    version_old = old["layout"][("value", "LYCHEE_API_VERSION")]
    version_new = new["layout"][("value", "LYCHEE_API_VERSION")]
    print(f"abi_check: {HEADER} against {ref} (API {version_old}), "
          f"now API {version_new}")
    for line in breaks:
        print(f"  break: {line}")
    for line in additions:
        print(f"  added: {line}")
    problems = []
    if version_new < version_old:
        problems.append(f"LYCHEE_API_VERSION went back from {version_old}")
    elif (breaks or additions) and version_new == version_old:
        problems.append(f"the ABI changed but LYCHEE_API_VERSION is still "
                        f"{version_old}: raise it, with a history line")
    if version_new > version_old and not re.search(
            rf"^// {version_new}: ", new["text"], re.MULTILINE):
        problems.append(f"no history line (// {version_new}: ...) above "
                        f"LYCHEE_API_VERSION")
    for problem in problems:
        print(f"FAIL abi against {ref}: {problem}")
    if not problems:
        print(f"PASS abi against {ref} ({len(breaks)} breaks, "
              f"{len(additions)} additions)")
    return 1 if problems else 0


def main():
    os.chdir(ROOT)
    refs = []
    release = git("describe", "--tags", "--abbrev=0", "--match", "v*", "HEAD")
    if release:
        refs.append(release)
    else:
        print("abi_check: no v* tag reachable from HEAD")
    base = sys.argv[1] if len(sys.argv) > 1 else "main"
    if base.strip("0"):
        fork = git("merge-base", base, "HEAD")
        if fork and git("cat-file", "-e", f"{fork}:{HEADER}") is not None:
            refs.append(fork[:12])
    new = api(os.path.join(ROOT, "core", "include"))
    failed = 0
    for ref in refs:
        failed |= check(ref, new)
    return failed


if __name__ == "__main__":
    sys.exit(main())
