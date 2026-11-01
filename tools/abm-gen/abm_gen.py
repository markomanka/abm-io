#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""abm-gen: writes abmio::TypeTraits specialisations from C++ headers.

See docs/GENERATOR.md. Usage:

  abm_gen.py --out generated.h [--side units.yaml] [--include header.h ...]
             [--compile-commands build/compile_commands.json] header.h ... [-- flags]

Agent and behaviour classes are those whose body contains the agent or
behaviour macro (by default BioDynaMo's BDM_AGENT_HEADER and
BDM_BEHAVIOR_HEADER); unmarked base classes are described as roots. Data
members are visited in declaration order and named without a trailing
underscore; members marked transient with //! are left out.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass, field

import clang.cindex as ci

ROLES = {"checkpoint": "Role::kCheckpoint", "exchange": "Role::kExchange", "export": "Role::kExport"}


class GeneratorError(Exception):
    """A problem in the input, reported with what and where."""


@dataclass
class Member:
    field: str   # the field name
    member: str  # the C++ member name
    unit: str = ""
    roles: list = None  # None: all roles


@dataclass
class Type:
    name: str              # qualified name
    base: str              # qualified base name, or "" for none
    version: str
    members: list = field(default_factory=list)


def system_include_dirs(cxx):
    """The C++ compiler's own include directories, which libclang needs."""
    try:
        out = subprocess.run([cxx, "-E", "-x", "c++", "-", "-v"], input="",
                             capture_output=True, text=True, check=False).stderr
    except OSError:
        return []
    dirs, inside = [], False
    for line in out.splitlines():
        if line.startswith("#include <...> search starts here"):
            inside = True
        elif line.startswith("End of search list"):
            break
        elif inside:
            dirs.append(line.strip().replace(" (framework directory)", ""))
    return dirs


def flags_from_compile_commands(path):
    """Include paths, definitions and standard from the first compile command."""
    with open(path) as f:
        entries = json.load(f)
    if not entries:
        return []
    entry = entries[0]
    args = entry.get("arguments") or shlex.split(entry["command"])
    kept, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
        elif a in ("-o", "-c"):
            skip = a == "-o"
        elif a.startswith(("-I", "-D", "-isystem", "-std=", "-U")):
            kept.append(a)
        elif a == "-isystem":
            kept.append(a)
    return kept


def qualified_name(cursor):
    parts = []
    c = cursor
    while c is not None and c.kind != ci.CursorKind.TRANSLATION_UNIT:
        if c.spelling:
            parts.append(c.spelling)
        c = c.semantic_parent
    return "::".join(reversed(parts))


def macro_arguments(cursor):
    """The arguments of a macro use, as written."""
    tokens = [t.spelling for t in cursor.get_tokens()]
    if len(tokens) < 2 or tokens[1] != "(":
        return []
    args, depth, current = [], 0, []
    for t in tokens[2:]:
        if t in ("(", "<", "["):
            depth += 1
        elif t in (")", ">", "]"):
            if depth == 0 and t == ")":
                args.append(" ".join(current))
                break
            depth -= 1
        if t == "," and depth == 0:
            args.append(" ".join(current))
            current = []
        elif not (t == ")" and depth < 0):
            current.append(t)
    return args


class Generator:
    def __init__(self, flags, agent_macro="BDM_AGENT_HEADER",
                 behavior_macro="BDM_BEHAVIOR_HEADER", strip_suffix="_"):
        self.flags = flags
        self.macros = {agent_macro, behavior_macro}
        self.strip_suffix = strip_suffix
        self.types = {}     # qualified name -> Type
        self.order = []     # qualified names, in discovery order
        self.lines = {}     # file -> source lines
        self.marks = {}     # class USR -> macro arguments

    def _source_line(self, filename, line):
        if filename not in self.lines:
            with open(filename, encoding="utf-8", errors="replace") as f:
                self.lines[filename] = f.read().splitlines()
        return self.lines[filename][line - 1]

    def _field_name(self, member):
        s = self.strip_suffix
        return member[: -len(s)] if s and member.endswith(s) and len(member) > len(s) else member

    def parse(self, header):
        index = ci.Index.create()
        options = (ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
                   | ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
        tu = index.parse(header, args=["-x", "c++"] + self.flags, options=options)
        errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errors:
            raise GeneratorError("\n".join(f"{header}: {d.location.line}:{d.location.column}: "
                                           f"{d.spelling}" for d in errors[:10]))
        path = os.path.realpath(header)
        # Macro uses in every file the parser saw, so that the bases of a model's
        # classes keep their own versions even when defined in other headers.
        uses = [(os.path.realpath(c.location.file.name), c.location.offset, macro_arguments(c))
                for c in tu.cursor.get_children()
                if c.kind == ci.CursorKind.MACRO_INSTANTIATION and c.spelling in self.macros
                and c.location.file]
        files = {f for f, _, _ in uses} | {path}
        defs = [c for c in tu.cursor.walk_preorder()
                if c.kind in (ci.CursorKind.CLASS_DECL, ci.CursorKind.STRUCT_DECL,
                              ci.CursorKind.CLASS_TEMPLATE)
                and c.location.file and os.path.realpath(c.location.file.name) in files]
        marked = []
        for f, offset, args in uses:
            inner = [c for c in defs if os.path.realpath(c.location.file.name) == f
                     and c.extent.start.offset < offset < c.extent.end.offset]
            if not inner:
                continue
            cls = max(inner, key=lambda c: c.extent.start.offset)  # the innermost
            if cls.kind == ci.CursorKind.CLASS_TEMPLATE:
                if f == path:
                    raise GeneratorError(f"{qualified_name(cls)}: class templates are not "
                                         "supported")
                continue
            if len(args) != 3:
                raise GeneratorError(f"{qualified_name(cls)}: expected three macro arguments "
                                     f"(class, base, version), got {args}")
            self.marks[cls.get_usr()] = args
            if f == path:
                marked.append(cls)
        for cls in marked:
            self._add(cls)

    def _add(self, cls):
        """Describes a class after its base classes; unmarked classes are roots."""
        name = qualified_name(cls)
        if name in self.types:
            return
        args = self.marks.get(cls.get_usr())
        written_base, version = (args[1].replace(" ", ""), args[2]) if args else ("", "1")
        bases = [c for c in cls.get_children() if c.kind == ci.CursorKind.CXX_BASE_SPECIFIER]
        base = ""
        if bases:
            decl = bases[0].type.get_declaration()
            base = qualified_name(decl)
            if written_base and not (base == written_base or base.endswith("::" + written_base)):
                raise GeneratorError(f"{name}: the macro names base {written_base}, "
                                     f"but the class derives from {base}")
            if decl.is_definition():
                self._add(decl)
        elif written_base:
            raise GeneratorError(f"{name}: the macro names base {written_base}, "
                                 "but the class has no base class")
        t = Type(name, base, version)
        filename = cls.location.file.name
        for c in cls.get_children():
            if c.kind != ci.CursorKind.FIELD_DECL:
                continue
            line = self._source_line(filename, c.location.line)
            if "//!" in line[c.location.column - 1:]:
                continue  # transient: never stored or sent
            t.members.append(Member(self._field_name(c.spelling), c.spelling))
        self.types[name] = t
        self.order.append(name)

    def apply_side_file(self, side):
        for type_name, fields in (side or {}).items():
            if type_name not in self.types:
                raise GeneratorError(f"side file: no described type {type_name}")
            members = {m.field: m for m in self.types[type_name].members}
            for field_name, props in (fields or {}).items():
                if field_name not in members:
                    raise GeneratorError(f"side file: {type_name} has no field {field_name}")
                props = props or {}
                unknown = set(props) - {"unit", "roles"}
                if unknown:
                    raise GeneratorError(f"side file: {type_name}.{field_name}: unknown "
                                         f"keys {sorted(unknown)}")
                m = members[field_name]
                m.unit = str(props.get("unit", ""))
                if "roles" in props:
                    roles = props["roles"] or []
                    bad = [r for r in roles if r not in ROLES]
                    if bad:
                        raise GeneratorError(f"side file: {type_name}.{field_name}: unknown "
                                             f"roles {bad}; use {sorted(ROLES)}")
                    m.roles = roles

    def ordered(self):
        """Base types before the types derived from them, otherwise in discovery order."""
        done, out = set(), []

        def visit(name):
            if name in done:
                return
            base = self.types[name].base
            if base in self.types:
                visit(base)
            done.add(name)
            out.append(self.types[name])

        for name in self.order:
            visit(name)
        return out

    def render(self, sources, includes):
        lines = [f"// Generated by abm-gen from {', '.join(sources)}; do not edit.",
                 "#pragma once"]
        lines += [f'#include "{h}"' for h in includes]
        lines += ['#include "abmio/schema.h"', "", "namespace abmio {"]
        for t in self.ordered():
            lines += ["", "template <>", f"struct TypeTraits<{t.name}> {{",
                      f'  static constexpr const char* kName = "{t.name}";',
                      f"  static constexpr uint32_t kVersion = {t.version};",
                      f"  using Base = {t.base or 'void'};", "  template <class V>"]
            if not t.members:
                lines += ["  static void Visit(V&) {}", "};"]
                continue
            lines.append("  static void Visit(V& v) {")
            for m in t.members:
                spec = f'Field("{m.field}")'
                if m.unit:
                    spec += f'.Unit("{m.unit}")'
                if m.roles is not None:
                    spec += f".Roles({' | '.join(ROLES[r] for r in m.roles) or 'Role::kNone'})"
                lines.append(f"    v({spec}, &{t.name}::{m.member});")
            lines += ["  }", "};"]
        lines += ["", "}  // namespace abmio", ""]
        return "\n".join(lines)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser(description="Writes abmio::TypeTraits specialisations "
                                            "from C++ headers (docs/GENERATOR.md).")
    p.add_argument("headers", nargs="+")
    p.add_argument("--out", required=True)
    p.add_argument("--side", help="YAML file with units and roles per field")
    p.add_argument("--include", action="append", default=None,
                   help="header to include in the output (default: each input header's name)")
    p.add_argument("--compile-commands")
    p.add_argument("--agent-macro", default="BDM_AGENT_HEADER")
    p.add_argument("--behavior-macro", default="BDM_BEHAVIOR_HEADER")
    p.add_argument("--strip-suffix", default="_")
    p.add_argument("--std", default="c++17")
    p.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    a = p.parse_args(argv)
    flags = [f"-std={a.std}"] + [f"-isystem{d}" for d in system_include_dirs(a.cxx)]
    if a.compile_commands:
        flags += flags_from_compile_commands(a.compile_commands)
    flags += extra
    gen = Generator(flags, a.agent_macro, a.behavior_macro, a.strip_suffix)
    try:
        for h in a.headers:
            gen.parse(h)
        if a.side:
            import yaml
            with open(a.side) as f:
                gen.apply_side_file(yaml.safe_load(f))
    except GeneratorError as e:
        print(f"abm-gen: {e}", file=sys.stderr)
        return 1
    text = gen.render([os.path.basename(h) for h in a.headers],
                      a.include if a.include is not None else [os.path.basename(h) for h in a.headers])
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
