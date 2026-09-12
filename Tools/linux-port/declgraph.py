#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Declaration graph over the Swift sources (docs/linux-port, WP-11).
#
# WP-00 (spikes/00-swift-core.md § 8, condition 1) found that 3052 of the 4173
# diagnostics in the core spike were `cannot find X in scope`: the candidate
# list in WORK_PACKAGES.md is a *feature* list, and the app has no module
# boundaries, so any subset of it is full of holes. This tool builds the
# missing information — which file declares which name, and which files
# reference it — so WP-11 can move dependency-closed sets instead.
#
# No third-party dependencies; python3 only. It is a lexical tool, not a
# parser: it strips comments and string literals, then matches declaration
# keywords and bare identifiers. That is deliberately conservative — it
# over-reports references (a local variable that happens to share a type's
# name counts), so a closure it calls closed really is closed, while a file it
# drags in may occasionally be unnecessary.
#
# Usage:
#   declgraph.py decls                             every declaration, by file
#   declgraph.py closure FILE...                   closure + unresolved names
#   declgraph.py closure --from-file LIST
#   declgraph.py order FILE...                     topological move order
#   declgraph.py order --from-file LIST
#   declgraph.py rdeps NAME                        who references NAME
#   declgraph.py why FILE --in FILE...             why a file is in a closure
#
# `closure` and `order` take the files you *propose* to move; `closure` prints
# the transitive set of files they need and the names nothing in Sources
# declares, `order` prints that closure split into closed sets (strongly
# connected components condensed and topologically sorted), one set per line,
# in an order where every set only depends on sets already listed.

import argparse
import os
import re
import sys
from collections import defaultdict


# ---------------------------------------------------------------- lexing

def strip_noise(text):
    """Remove comments and string literals, keeping offsets (and line count).

    Swift block comments nest, string literals can be raw (#"..."#) or
    multi-line, and `\\(...)` interpolation contains real code. The
    interpolation is kept (it can reference a type); everything else in a
    literal becomes spaces so that offsets, and therefore line numbers, do not
    move.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            depth = 1
            j = i + 2
            while j < n and depth:
                if text.startswith("/*", j):
                    depth += 1
                    j += 2
                elif text.startswith("*/", j):
                    depth -= 1
                    j += 2
                else:
                    j += 1
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        if c == "#" and re.match(r'#+"', text[i:]):
            hashes = len(text[i:]) - len(text[i:].lstrip("#"))
            pounds = "#" * hashes
            if text.startswith(pounds + '"""', i):
                opener, closer = pounds + '"""', '"""' + pounds
            else:
                opener, closer = pounds + '"', '"' + pounds
            j = text.find(closer, i + len(opener))
            j = n if j < 0 else j + len(closer)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        if c == '"':
            if text.startswith('"""', i):
                j = text.find('"""', i + 3)
                j = n if j < 0 else j + 3
                out.append(_blank_literal(text[i:j]))
                i = j
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] in ('"', "\n"):
                    break
                j += 1
            j = min(j + 1, n)
            out.append(_blank_literal(text[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _blank_literal(chunk):
    """Blank a string literal but keep `\\(...)` interpolation contents."""
    result = []
    i = 0
    n = len(chunk)
    while i < n:
        if chunk.startswith("\\(", i):
            depth = 1
            j = i + 2
            while j < n and depth:
                if chunk[j] == "(":
                    depth += 1
                elif chunk[j] == ")":
                    depth -= 1
                j += 1
            result.append("  " + chunk[i + 2:j - 1] + " ")
            i = j
            continue
        result.append(chunk[i] if chunk[i] == "\n" else " ")
        i += 1
    return "".join(result)


# ------------------------------------------------------------ declarations

# `enum|struct|class|actor|protocol` followed by a name. `final class`,
# `public enum`, `@MainActor final class` and nested forms all reduce to this
# because the keyword and the name are adjacent. `class` is also a modifier
# (`class func`, `class var`); those are excluded by requiring the name to
# start with an upper-case letter, which every type in this repo does.
_DECL = re.compile(r"\b(?:enum|struct|class|actor|protocol)\s+([A-Z][A-Za-z0-9_]*)")
_TYPEALIAS = re.compile(r"\btypealias\s+([A-Z][A-Za-z0-9_]*)")
_EXTENSION = re.compile(r"\bextension\s+([A-Z][A-Za-z0-9_]*)")
# Top-level functions are declarations too; the core spike hit
# `cannot find 'X' in scope` for a couple of these, so they count.
_TOPLEVEL_FUNC = re.compile(r"^func\s+([a-zA-Z_][A-Za-z0-9_]*)", re.MULTILINE)
_IDENT = re.compile(r"\b([A-Z][A-Za-z0-9_]*)\b")
# A top-level function is only referenced by being *called*, and never through
# a dot. Matching the bare name instead made every `sectionTitle:` argument
# label in a Strings literal look like a use of `func sectionTitle` in
# UI/Theme.swift, which pinned four files to the UI layer that do not touch it.
_LOWER_CALL = re.compile(r"(?<![.\w])([a-z][A-Za-z0-9_]*)\s*\(")
_IMPORT = re.compile(
    r"^\s*(?:@[A-Za-z_]+\s+)?import\s+([A-Za-z_][A-Za-z0-9_.]*)", re.MULTILINE)

# Apple / stdlib name prefixes. Nothing that is not declared under the root
# enters the graph anyway; this list only keeps the `unresolved` report
# readable by separating "Apple API we do not own" from "a hole in the set".
STDLIB_PREFIXES = ("NS", "CG", "CF", "CA", "CI", "AV", "UT", "IO", "SC", "TIS",
                   "UC", "LM", "OS", "MTL", "VN", "kCG", "kUC", "kTIS")


class Graph:
    def __init__(self, root, exclude=()):
        self.root = root
        self.files = []
        self.declares = {}       # file -> set(names)
        self.extends = {}        # file -> set(names)
        self.references = {}     # file -> set(names)
        self.imports = {}        # file -> set(modules)
        self.lines = {}          # file -> int
        self.owner = defaultdict(set)   # name -> set(files declaring it)
        self._scan(exclude)

    def _scan(self, exclude):
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames.sort()
            for name in sorted(filenames):
                if not name.endswith(".swift"):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), ".")
                if any(rel.startswith(e) for e in exclude):
                    continue
                self._scan_file(rel)
        for path in self.files:
            for name in self.declares[path]:
                self.owner[name].add(path)

    def _scan_file(self, path):
        with open(path, encoding="utf-8", errors="replace") as handle:
            raw = handle.read()
        text = strip_noise(raw)
        declares = set(_DECL.findall(text))
        declares.update(_TYPEALIAS.findall(text))
        declares.update(_TOPLEVEL_FUNC.findall(text))
        extends = set(_EXTENSION.findall(text))
        refs = set(_IDENT.findall(text)) | set(_LOWER_CALL.findall(text))
        self.files.append(path)
        self.declares[path] = declares
        self.extends[path] = extends
        # An extension is both a reference to the extended type and, for the
        # purpose of a move, a hard requirement on the file that declares it.
        self.references[path] = (refs | extends) - declares
        self.imports[path] = set(_IMPORT.findall(text))
        self.lines[path] = raw.count("\n") + (0 if raw.endswith("\n") else 1)

    # -------------------------------------------------------------- queries

    def deps(self, path):
        """Files `path` needs, and the names that resolve to nothing."""
        needed = set()
        unresolved = set()
        for name in self.references[path]:
            owners = self.owner.get(name)
            if not owners:
                if name[:1].isupper():
                    unresolved.add(name)
                continue
            needed.update(owners - {path})
        return needed, unresolved

    def closure(self, seeds):
        seen = set(seeds)
        queue = list(seeds)
        unresolved = defaultdict(set)
        while queue:
            path = queue.pop()
            needed, missing = self.deps(path)
            for name in missing:
                unresolved[name].add(path)
            for dep in needed:
                if dep not in seen:
                    seen.add(dep)
                    queue.append(dep)
        return seen, unresolved

    def subgraph(self, paths):
        paths = set(paths)
        edges = {}
        for path in paths:
            needed, _ = self.deps(path)
            edges[path] = (needed & paths) - {path}
        return edges


# ------------------------------------------------------- SCC + topo order

def strongly_connected(edges):
    """Tarjan, iterative. Emits each component after everything it points at."""
    index = {}
    low = {}
    on_stack = {}
    stack = []
    result = []
    counter = [0]
    for root in sorted(edges):
        if root in index:
            continue
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack[root] = True
        work = [(root, iter(sorted(edges[root])))]
        while work:
            node, children = work[-1]
            advanced = False
            for child in children:
                if child not in index:
                    index[child] = low[child] = counter[0]
                    counter[0] += 1
                    stack.append(child)
                    on_stack[child] = True
                    work.append((child, iter(sorted(edges[child]))))
                    advanced = True
                    break
                if on_stack.get(child):
                    low[node] = min(low[node], index[child])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                component = []
                while True:
                    top = stack.pop()
                    on_stack[top] = False
                    component.append(top)
                    if top == node:
                        break
                result.append(sorted(component))
    return result


def move_order(graph, paths):
    """Closed sets, in an order where each only needs the sets before it.

    Edges here point from a file to the files it depends on, and Tarjan emits
    a component only after every component it points at, so the emission order
    already is the move order.
    """
    return strongly_connected(graph.subgraph(paths))


# ------------------------------------------------------------------- CLI

def load_seeds(args):
    seeds = list(args.files)
    if args.from_file:
        with open(args.from_file, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#"):
                    seeds.append(line)
    missing = [s for s in seeds if not os.path.isfile(s)]
    if missing:
        sys.exit("no such file: " + ", ".join(missing))
    return [os.path.relpath(s, ".") for s in seeds]


def main():
    parser = argparse.ArgumentParser(
        description="declaration graph over the Swift sources (WP-11)")
    parser.add_argument("--root", default="Sources")
    parser.add_argument("--exclude", action="append", default=[],
                        help="path prefix to skip (repeatable)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("decls")
    p.add_argument("--names-only", action="store_true")

    for name in ("closure", "order"):
        p = sub.add_parser(name)
        p.add_argument("files", nargs="*")
        p.add_argument("--from-file")
        p.add_argument("--quiet", action="store_true")

    p = sub.add_parser("rdeps")
    p.add_argument("name")

    p = sub.add_parser("why")
    p.add_argument("target")
    p.add_argument("--in", dest="seeds", nargs="+", required=True)

    args = parser.parse_args()
    graph = Graph(args.root, exclude=tuple(args.exclude))

    if args.command == "decls":
        for path in graph.files:
            names = sorted(graph.declares[path])
            if args.names_only:
                for name in names:
                    print(name)
            else:
                print("%s\t%d\t%s" % (path, graph.lines[path], " ".join(names)))
        return

    if args.command == "rdeps":
        for path in graph.files:
            if args.name in graph.references[path]:
                print(path)
        return

    if args.command == "why":
        seeds = [os.path.relpath(s, ".") for s in args.seeds]
        prev = {}
        frontier = list(seeds)
        seen = set(seeds)
        while frontier:
            nxt = []
            for path in frontier:
                needed, _ = graph.deps(path)
                for dep in sorted(needed):
                    if dep in seen:
                        continue
                    seen.add(dep)
                    prev[dep] = path
                    nxt.append(dep)
            frontier = nxt
        target = os.path.relpath(args.target, ".")
        if target not in seen:
            print("not in the closure")
            return
        chain = [target]
        while chain[-1] in prev:
            chain.append(prev[chain[-1]])
        for a, b in zip(chain[1:], chain[:-1]):
            shared = sorted(graph.references[a] & graph.declares[b])
            print("%s -> %s   (%s)" % (a, b, ", ".join(shared) or "?"))
        return

    seeds = load_seeds(args)
    closed, unresolved = graph.closure(seeds)

    if args.command == "closure":
        extra = sorted(closed - set(seeds))
        print("seeds:    %d files, %d lines"
              % (len(seeds), sum(graph.lines[p] for p in seeds)))
        print("closure:  %d files, %d lines"
              % (len(closed), sum(graph.lines[p] for p in closed)))
        if not args.quiet:
            print("\n--- dragged in by the closure (%d) ---" % len(extra))
            for path in extra:
                print(path)
        interesting = {n: fs for n, fs in unresolved.items()
                       if not n.startswith(STDLIB_PREFIXES)}
        print("\n--- unresolved names: %d (%d outside the Apple prefixes) ---"
              % (len(unresolved), len(interesting)))
        if not args.quiet:
            for name in sorted(unresolved):
                print("%-44s %s" % (name, " ".join(sorted(unresolved[name])[:3])))
        return

    if args.command == "order":
        for i, component in enumerate(move_order(graph, closed), 1):
            total = sum(graph.lines[p] for p in component)
            print("set %-3d %d files %6d lines" % (i, len(component), total))
            for path in component:
                print("        " + path)
        return


if __name__ == "__main__":
    main()
