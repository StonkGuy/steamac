#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Older OpenVR interface versions, served by adapters onto the current (openvr.h) implementation.

A game asks for the interface versions of the openvr_api.dll it shipped with (Proton's vrclient passes the
version string through), so a runtime built against OpenVR 2.15.6 also has to answer e.g. "IVRSystem_022". Few
old versions are a prefix of the current vtable (methods were inserted in the middle), so each old version gets
an adapter class with the old method order that forwards every method whose signature is unchanged to the
current implementation by name. Methods that changed or no longer exist log their first call and return a
value-initialised result, unless runtime/legacy_overrides.h supplies a hand-written version.

Two steps:
  gen_legacy.py extract OPENVR_GIT TAG...  > third_party/openvr/legacy_interfaces.txt
      reads headers/openvr.h of each tag of a ValveSoftware/openvr clone (BSD-3-Clause) and records the
      method list of every interface version (first tag that has it)
  gen_legacy.py gen legacy_interfaces.txt openvr.h > runtime/legacy.gen.h
  gen_legacy.py check-header legacy_interfaces.txt SOME/openvr.h > check.cpp
  gen_legacy.py check-adapters legacy_interfaces.txt openvr.h > check.cpp
      test programs (tests/check_legacy.sh): vtable slot and argument count of every method against a real
      header resp. the generated old vtables, and the sizes of the types passed
"""
import re
import subprocess
import sys

# interfaces the runtime serves (runtime/clientcore.cpp); older versions of others are not offered
SERVED = ["IVRSystem", "IVRCompositor", "IVRInput", "IVRChaperone", "IVRSettings", "IVRApplications",
          "IVRRenderModels", "IVROverlay", "IVRScreenshots", "IVROverlayView"]

MACROS = re.compile(r"\b(VR_ARRAY_COUNT|VR_ARRAY_COUNT_D|VR_OUT_STRING_COUNT|VR_OUT_ARRAY_COUNT|"
                    r"VR_OUT_BUFFER_COUNT|VR_OUT_ARRAY_CALL|VR_OUT_STRING|VR_OUT_STRUCT|VR_CLANG_ATTR)\s*\([^()]*\)")


def classes(text):
    for m in re.finditer(r"^\s*class (IVR\w+)\s*\n\s*\{(.*?)\n\s*\};", text, re.S | re.M):
        cls, body = m.group(1), m.group(2)
        vm = re.search(r"%s_Version\s*=\s*\"(\w+)\"" % cls, text[m.end():m.end() + 30000])
        yield cls, (vm.group(1) if vm else None), list(methods(body))


def split_args(s):
    depth, cur, out = 0, "", []
    for ch in s:
        if ch in "(<":
            depth += 1
        elif ch in ")>":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [a for a in out if a.strip()]


def norm_type(t):
    t = re.sub(r"\b(vr::|class |struct |enum )", "", t)
    t = t.replace("*", " * ").replace("&", " & ")
    return " ".join(t.split())


def methods(body):
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    for m in re.finditer(r"virtual\s+(.*?)\s*=\s*0\s*;", body, re.S):
        decl = MACROS.sub("", " ".join(m.group(1).split()))
        dm = re.match(r"(.*?)\b(\w+)\s*\((.*)\)\s*(const)?\s*$", decl)
        if not dm:
            raise SystemExit("cannot parse: " + decl)
        ret, name, args = dm.group(1).strip(), dm.group(2), dm.group(3).strip()
        types = []
        for a in split_args(args):
            a = re.sub(r"\s*=\s*[^,]+$", "", a.strip())
            if a == "void":
                continue
            am = re.match(r"(.*?[\w\*&\s])\s*\b(\w+)\s*$", a)  # drop the parameter name
            types.append(norm_type(am.group(1) if am and am.group(1).strip() else a))
        yield norm_type(ret), name, types, bool(dm.group(4))


def extract(git, tags):
    seen = {}
    for tag in tags:
        text = subprocess.check_output(["git", "-C", git, "show", tag + ":headers/openvr.h"], text=True, errors="replace")
        for cls, ver, ms in classes(text):
            if cls in SERVED and ver and ver not in seen:
                seen[ver] = (tag, cls, ms)
    print("# OpenVR interface method lists by version, extracted from ValveSoftware/openvr headers/openvr.h")
    print("# (BSD-3-Clause, Copyright (c) 2015, Valve Corporation) by tools/gen_legacy.py extract.")
    print("# @<version> <class> <first tag>, then one method per line: return|name|arg types (;)|const")
    for ver in sorted(seen, key=lambda v: (v.rsplit("_", 1)[0], int(v.rsplit("_", 1)[1]))):
        tag, cls, ms = seen[ver]
        print(f"@{ver} {cls} {tag}")
        for ret, name, types, const in ms:
            print(f"{ret}|{name}|{';'.join(types)}|{'const' if const else ''}")


def read_spec(path):
    out, cur = [], None
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            ver, cls, tag = line[1:].split()
            cur = (ver, cls, tag, [])
            out.append(cur)
        else:
            ret, name, types, const = line.split("|")
            cur[3].append((ret, name, [t for t in types.split(";") if t], const == "const"))
    return out


# Types an old header had that the current one dropped keep the ABI of the old method: every dropped type that
# was passed by value is an enum (32-bit), and a pointer is a pointer.
KNOWN = set()
BUILTIN = {"void", "bool", "char", "float", "double", "int", "unsigned", "const", "short", "long", "size_t"}


def learn_types(text):
    KNOWN.update(re.findall(r"\b(?:enum|struct|class|union)\s+(\w+)", text))
    KNOWN.update(re.findall(r"\btypedef\b[^;{}]*?\b(\w+)\s*(?:\[[^\]]*\])?\s*;", text))
    KNOWN.update(re.findall(r"\}\s*(\w+)\s*;", text))


def subst(t):
    def one(m):
        w = m.group(0)
        if w in BUILTIN or w in KNOWN or re.fullmatch(r"u?int\d+_t", w):
            return w
        return "void" if "*" in t else "int32_t"
    return re.sub(r"\b[A-Za-z_]\w*\b", one, t)


# versions not offered: IVRInput_003 (OpenVR 1.0.15 only) predates the action data structs
EXCLUDE = {"IVRInput_003"}


def default_value(ret):
    t = ret.strip()
    if t.endswith("*"):
        return "nullptr"
    if not t.startswith(("vr::", "uint", "int", "bool", "float", "double", "char", "unsigned")):
        t = "vr::" + t
    return t + "{}"


def gen(spec_path, header):
    text = open(header, encoding="utf-8").read()
    learn_types(text)
    current = {cls: (ver, {name: (ret, types, const) for ret, name, types, const in ms})
               for cls, ver, ms in classes(text) if cls in SERVED}
    overrides = set()
    try:
        here = __file__.rsplit("/", 1)[0]
        for m in re.finditer(r"SVR_LEGACY_OVERRIDE\((\w+),\s*(\w+)\)", open(here + "/../runtime/legacy_overrides.h").read()):
            overrides.add((m.group(1), m.group(2)))
    except OSError:
        pass
    print("// Generated by tools/gen_legacy.py from third_party/openvr/legacy_interfaces.txt; do not edit.")
    print("// Adapters serving older OpenVR interface versions with the current implementation.")
    print("#pragma once")
    print('#include "openvr.h"')
    print('#include "log.h"')
    print('#include "legacy_overrides.h"')
    print("namespace svr {")
    print("using namespace vr;")
    print("namespace legacy {")
    table, vtables, adapters = [], [], []
    for ver, cls, tag, ms in read_spec(spec_path):
        cur_ver, cur = current[cls]
        if ver == cur_ver or ver in EXCLUDE:
            continue
        # the old vtable (also used by tests to call an old version the way an old game does)
        v = [f"// {ver} (OpenVR {tag})", f"class {ver} {{", " public:"]
        for ret, name, types, const in ms:
            args = ", ".join(f"{subst(t)} a{i}" for i, t in enumerate(types))
            v.append(f"  virtual {subst(ret)} {name}({args}){' const' if const else ''} = 0;")
        v.append("};")
        vtables += v
        a = [f"class Adapt_{ver} final : public {ver} {{", " public:",
             f"  explicit Adapt_{ver}(vr::{cls} *c) : c_(c) {{}}"]
        forwarded = 0
        for ret, name, types, const in ms:
            args = ", ".join(f"{subst(t)} a{i}" for i, t in enumerate(types))
            call = ", ".join(f"a{i}" for i in range(len(types)))
            sig = f"  {subst(ret)} {name}({args}){' const' if const else ''} override"
            if (ver, name) in overrides:
                a.append(f"{sig} {{ SVR_LEGACY_BODY_{ver}_{name} }}")
                forwarded += 1
            elif name in cur and cur[name][0] == ret and cur[name][1] == types:
                a.append(f"{sig} {{ return c_->{name}({call}); }}")
                forwarded += 1
            else:
                body = f'stub_once("{ver}::{name}");'
                if ret != "void":
                    body += f" return {default_value(subst(ret))};"
                a.append(f"{sig} {{ {body} }}")
        a += [" private:", f"  vr::{cls} *c_;", "};"]
        adapters += a
        table.append((ver, cls, forwarded, len(ms)))
    print("\n".join(vtables))
    print("}  // namespace legacy")
    print()
    print("static const char *const kLegacyVersions[] = {")
    for ver, cls, fwd, n in table:
        print(f'    "{ver}",')
    print("};")
    print()
    print("#ifndef SVR_LEGACY_VTABLES_ONLY  // tests take the declarations above only")
    print("namespace legacy {")
    print("\n".join(adapters))
    print("}  // namespace legacy")
    print()
    print("// version string, interface, adapter factory over the current implementation")
    print("struct LegacyIface {")
    print("  const char *version, *iface;")
    print("  void *(*make)(void *current);")
    print("};")
    print("static const LegacyIface kLegacyIfaces[] = {")
    for ver, cls, fwd, n in table:
        print(f'    {{"{ver}", "{cls}", [](void *c) -> void * {{ return static_cast<legacy::{ver} *>('
              f"new legacy::Adapt_{ver}(static_cast<vr::{cls} *>(c))); }}}},  // {fwd}/{n} served")
    print("};")
    print("#endif")
    print("}  // namespace svr")


CHECK_PRELUDE = r"""#include <cstdio>
#include <cstring>
template <class C, class R, class... A> static long slot(R (C::*f)(A...)) {
  long w;
  memcpy(&w, &f, sizeof(w));  // Itanium C++ ABI: a pointer to a virtual member function holds 1 + vtable offset
  return (w - 1) / (long)sizeof(void *);
}
template <class C, class R, class... A> static long slot(R (C::*f)(A...) const) {
  long w;
  memcpy(&w, &f, sizeof(w));
  return (w - 1) / (long)sizeof(void *);
}
template <class C, class R, class... A> static int nargs(R (C::*)(A...)) { return sizeof...(A); }
template <class C, class R, class... A> static int nargs(R (C::*)(A...) const) { return sizeof...(A); }
static int bad;
#define SLOT(ver, cls, name, want, n) \\
  if (slot(&cls::name) != want || nargs(&cls::name) != n) \\
    bad++, printf("LAYOUT MISMATCH %s::%s slot %ld (want %d) args %d (want %d)\\n", ver, #name, slot(&cls::name), want, nargs(&cls::name), n);
#define SIZE(ver, name, arg, type) printf("SIZE %s %s %d %s %zu\\n", ver, #name, arg, #type, sizeof(type));
using namespace vr;
int main() {
""".replace("\\\\", "\\")


def pointee(t):
    t = re.sub(r"\bconst\b|[*&]", " ", t).split()
    opaque = len(t) == 1 and (t[0].startswith(("Vk", "ID3D")) or t[0] in BUILTIN or re.fullmatch(r"u?int\d+_t", t[0]))
    return t[0] if len(t) == 1 and not opaque else None


def check_header(spec_path, header):
    """A program that checks the spec against a real openvr.h: slot and argument count of every method of the
    versions that header declares, and prints the size of every struct/enum type they take."""
    text = open(header, encoding="utf-8", errors="replace").read()
    declared = {ver for cls, ver, ms in classes(text) if ver}
    print('#include "openvr.h"')
    print(CHECK_PRELUDE)
    n = 0
    for ver, cls, tag, ms in read_spec(spec_path):
        if ver not in declared:
            continue
        n += 1
        for i, (ret, name, types, const) in enumerate(ms):
            print(f'  SLOT("{ver}", vr::{cls}, {name}, {i}, {len(types)})')
            for j, t in enumerate(types):
                if pointee(t):
                    print(f'  SIZE("{ver}", {name}, {j}, {pointee(t)})')
    print(f'  printf("CHECKED {n} versions, %d mismatches\\n", bad);')
    print("  return bad != 0;\n}")


def check_adapters(spec_path, header):
    """The same for the generated old vtables (legacy.gen.h), with the sizes of the current types."""
    text = open(header, encoding="utf-8").read()
    learn_types(text)
    current = {cls: ver for cls, ver, ms in classes(text)}
    print('#include "legacy.gen.h"')
    print("void svr::stub_once(const char *) {}")
    print(CHECK_PRELUDE)
    n = 0
    for ver, cls, tag, ms in read_spec(spec_path):
        if ver == current[cls] or ver in EXCLUDE:
            continue
        n += 1
        for i, (ret, name, types, const) in enumerate(ms):
            print(f'  SLOT("{ver}", svr::legacy::{ver}, {name}, {i}, {len(types)})')
            for j, t in enumerate(types):
                if pointee(t) and pointee(t) in KNOWN:
                    print(f'  SIZE("{ver}", {name}, {j}, {pointee(t)})')
    print(f'  printf("CHECKED {n} versions, %d mismatches\\n", bad);')
    print("  return bad != 0;\n}")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "extract":
        extract(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) == 4 and sys.argv[1] == "gen":
        gen(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "check-header":
        check_header(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "check-adapters":
        check_adapters(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
