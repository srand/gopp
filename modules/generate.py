#!/usr/bin/env python3
"""
Writes the module interfaces in modules/: one per public header, named after
its path (<gopp/net/http.hpp> is gopp.net.http; template is a keyword, so
the text/template headers are gopp.text.tmpl), and gopp, which imports
them all.

An interface imports std.compat and the modules of the go++ headers that its
header includes, and wraps the header in `export extern "C++"`. So that the
header doesn't include them too, its #includes must be one block within
#ifndef GOPP_MODULE.

Run from the repository root after adding a header or changing what a header
includes: python3 modules/generate.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE = os.path.join(ROOT, "include")
MODULES = os.path.join(ROOT, "modules")


def headers():
    for dirpath, _, files in os.walk(os.path.join(INCLUDE, "gopp")):
        for f in files:
            if f.endswith(".hpp") and f != "gopp.hpp":
                yield os.path.relpath(os.path.join(dirpath, f), INCLUDE)


def module_name(header):
    return header[:-len(".hpp")].replace(os.sep, ".").replace("template", "tmpl")


def gopp_includes(header):
    """ The go++ headers that a header includes, checking that it's guarded. """
    lines = open(os.path.join(INCLUDE, header)).read().split("\n")
    includes = [i for i, line in enumerate(lines) if line.startswith("#include")]
    if not includes:
        return []
    first, last = includes[0], includes[-1]
    if (first == 0 or lines[first - 1] != "#ifndef GOPP_MODULE"
            or last + 1 >= len(lines) or lines[last + 1] != "#endif"):
        sys.exit(f"{header}: the #includes must be one block within #ifndef GOPP_MODULE")
    return [m.group(1) for line in lines[first:last + 1] if (m := re.match(r"#include <(gopp/.*)>", line))]


def main():
    hs = sorted(headers())
    written = set()
    for h in hs:
        m = module_name(h)
        imports = "".join(f"export import {module_name(d)};\n" for d in gopp_includes(h))
        path = os.path.join(MODULES, m + ".cppm")
        with open(path, "w") as f:
            f.write(f"// Module {m}: <{h}>. Written by generate.py.\n"
                    "module;\n"
                    "#define GOPP_MODULE\n"
                    f"export module {m};\n"
                    "\n"
                    "import std.compat;\n"
                    f"{imports}"
                    "\n"
                    "export extern \"C++\" {\n"
                    f"#include <{h}>\n"
                    "}\n")
        written.add(path)

    path = os.path.join(MODULES, "gopp.cppm")
    with open(path, "w") as f:
        f.write("// Module gopp: all of go++. Written by generate.py.\n"
                "export module gopp;\n"
                "\n")
        f.write("".join(f"export import {module_name(h)};\n" for h in hs))
    written.add(path)

    for f in os.listdir(MODULES):
        path = os.path.join(MODULES, f)
        if f.endswith(".cppm") and path not in written:
            print(f"removing {f}: no such header")
            os.remove(path)


if __name__ == "__main__":
    main()
