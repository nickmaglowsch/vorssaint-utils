#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# WP-16: port the pure checks of the `build.sh --test` harness to XCTest, so
# they run on Linux against `VorssaintCore` as well.
#
# `Tests/MetricsTests.swift` is one 26 000-line `static func main()` holding a
# flat sequence of local bindings and `expect(...)` calls, cut into sections by
# `// MARK:` comments. Copying it by hand is not an option and copying it whole
# is not possible: most of it reaches into AppKit/IOKit code that stayed in
# `Sources/Vorssaint`. This tool selects the subsequence that only touches
# declarations under `Sources/VorssaintCore` (plus the standard library and the
# part of Foundation that swift-corelibs-foundation provides) and emits it as
# XCTest cases under `Tests/VorssaintCoreTests/`.
#
# It is deliberately conservative — it drops a statement it cannot prove
# portable, and it drops every later statement that depends on a dropped one —
# and it is deterministic: the same sources always produce the same files, so
# the output is committed and reviewable.
#
# Regenerate with:
#
#     python3 Tools/linux-port/port-tests.py
#
# and with `--report` for the per-section counts (docs/linux-port/TESTS.md).
#
# What it does NOT do: change `Tests/*.swift` or `build.sh --test`. The macOS
# harness keeps running the original 31 565 checks from the original files;
# this is a second, additive reader of the same text.

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import declgraph  # noqa: E402

SOURCE_ROOT = "Sources"
CORE_PREFIX = "Sources/VorssaintCore/"
TESTS_DIR = "Tests"
OUT_DIR = "Tests/VorssaintCoreTests"
GENERATED_PREFIX = "Generated"

# The entry points the harness owns. `expect`/`expectEqual`/`expectClose`/
# `expectFormat` are re-provided by the generated shim, `placeholderShape` and
# `formatSpecifiers` are pure static helpers copied verbatim into it.
HARNESS_NAMES = {
    "expect", "expectEqual", "expectClose", "expectFormat",
    "placeholderShape", "formatSpecifiers",
}

# Swift keywords and contextual keywords: never a reference to anything.
KEYWORDS = set("""
associatedtype class deinit enum extension fileprivate func import init inout
internal let open operator private precedencegroup protocol public rethrows
static struct subscript typealias var actor
break case continue default defer do else fallthrough for guard if in repeat
return switch where while
Any as catch false is nil rethrows self Self super throw throws true try
_ associativity convenience didSet dynamic final get indirect infix lazy left
mutating none nonmutating optional override postfix precedence prefix Protocol
required right set some Type unowned weak willSet async await consuming
borrowing each macro package any
""".split())

# Lower-case names the standard library provides as free functions.
FREE_FUNCTIONS = {
    "abs", "min", "max", "print", "zip", "stride", "assert", "precondition",
    "swap", "type", "repeatElement", "sqrt", "floor", "ceil", "round", "pow",
    "fabs", "exp", "log", "log2", "log10", "sin", "cos", "tan", "atan",
    "atan2", "fmod", "trunc", "hypot", "fmin", "fmax", "dump", "withUnsafeBytes",
    "sequence", "numericCast", "unsafeBitCast", "isKnownUniquelyReferenced",
}

# Upper-case names that are not declared in `Sources` and are known to exist on
# Linux with the same meaning: the standard library, and the subset of
# Foundation that swift-corelibs-foundation implements and that these checks
# use. Anything not on this list is treated as platform API and the statement
# that mentions it is dropped — that is what keeps AppKit, IOKit, CoreGraphics,
# AVFoundation and Carbon out of the Linux suite without enumerating them.
ALLOWED_TYPES = {
    # stdlib
    "String", "Substring", "Character", "Int", "Int8", "Int16", "Int32",
    "Int64", "UInt", "UInt8", "UInt16", "UInt32", "UInt64", "Double", "Float",
    "Bool", "Array", "ArraySlice", "Dictionary", "Set", "Optional", "Result",
    "Range", "ClosedRange", "Void", "AnyHashable", "Comparable", "Equatable",
    "Hashable", "Codable", "Encodable", "Decodable", "CaseIterable",
    "CustomStringConvertible", "Sequence", "Collection", "Never", "Mirror",
    "Unicode", "StaticString", "AnyIterator", "Strideable", "Numeric",
    "Identifiable", "RawRepresentable", "OptionSet", "Error", "Task",
    "Character", "UnicodeScalar", "Bundle",
    # Foundation (corelibs provides all of these)
    "Foundation", "Data", "Date", "DateComponents", "DateFormatter",
    "DateInterval", "Calendar", "TimeZone", "Locale", "NumberFormatter",
    "ByteCountFormatter", "DateComponentsFormatter", "RelativeDateTimeFormatter",
    "Measurement", "UnitDuration", "URL", "URLComponents", "URLQueryItem",
    "URLRequest", "URLResponse", "HTTPURLResponse", "URLSession",
    "URLSessionConfiguration", "FileManager", "JSONEncoder", "JSONDecoder",
    "JSONSerialization", "PropertyListDecoder", "PropertyListEncoder",
    "PropertyListSerialization", "UserDefaults", "CharacterSet", "Scanner",
    "NSRange", "IndexSet", "Notification", "NotificationCenter", "Thread",
    "RunLoop", "Timer", "OperationQueue", "Operation", "Process", "Pipe",
    "ProcessInfo", "UUID", "TimeInterval", "DispatchQueue", "DispatchTime",
    "DispatchSemaphore", "DispatchGroup", "DispatchWorkItem", "Dispatch",
}

# Upper-case names from Combine. A statement that mentions one of these is
# still portable, but the file it lands in needs the conditional import.
COMBINE_TYPES = {
    "AnyCancellable", "Cancellable", "Published", "ObservableObject",
    "PassthroughSubject", "CurrentValueSubject", "AnyPublisher", "Publishers",
    "Subscribers",
}

# Statements the selection rules accept but the Linux run cannot keep, keyed
# by source file and by the line the statement starts on. Each entry needs a
# reason and the CI run that showed it; this is the escape hatch for a
# behavioural difference between Darwin Foundation and corelibs that no
# name-based rule can see. Keep it short — a growing list means the rules are
# wrong, not the checks.
EXCLUDED = {
    "Tests/MetricsTests.swift": {
        14150: "asserts /bin/launchctl, /usr/bin/hdiutil, /usr/sbin/spctl … "
               "exist on the machine running the tests; they are macOS system "
               "tools and the check is about the Mac product, not the code "
               "(run 34693146465)",
        15139: "`/tmp/Installer Mount` resolves to `/private/tmp/Installer "
               "Mount` on macOS and to itself on Linux, so the hdiutil plist "
               "round-trip compares two different paths. Takes the two other "
               "checks in the same `if let` block with it (run 34693146465)",
    },
}

IDENT = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")
BARE = re.compile(r"(?<![.\w$])([a-z_][A-Za-z0-9_]*)\b(?!\s*:)")
# `let x` with no `=` and no type after it: the Swift 5.7 shorthand binding.
# `SomeType.member` — a qualified use, the only member reference whose
# receiver type can be read off the text.
MEMBER_USE = re.compile(r"\b([A-Z][A-Za-z0-9_]*)\.([a-zA-Z_][A-Za-z0-9_]*)")
SHORTHAND = re.compile(r"\b(?:let|var)\s+([a-z_][A-Za-z0-9_]*)\s*(?=[,{)\]]|$)",
                       re.MULTILINE)
MARK = re.compile(r"^\s*// MARK:\s*(.+?)\s*$")
STATIC_ASSIGN = re.compile(r"^([A-Z][A-Za-z0-9_]*)\.([a-z][A-Za-z0-9_]*)\s*=\s*\S")


# -------------------------------------------------------------------- lexing

# Matched with `.match(text, i)`, never against `text[i:]`: slicing the tail of
# a 1 MB file once per character is quadratic and hangs the tool.
STRING_OPEN = re.compile(r'(#*)("""|")')


def scrub(text, with_open_strings=False):
    """Blank comments and string literals, keeping every offset and newline.

    With `with_open_strings`, also returns one flag per line: true when the
    newline that ends that line falls *inside* a multi-line string literal, so
    the statement splitter knows the next line is not a new statement (the
    body of a `\"\"\"` literal is full of lines that start at the statement's
    own indentation with `{`).

    `declgraph.strip_noise` does the same job, but its string scanner is not
    interpolation-aware: in `"… (\\(x.joined(separator: ", ")))"` it ends the
    literal at the quote *inside* the interpolation and then eats the real
    closing parenthesis as a literal of its own. That is harmless for a
    name index and fatal for a bracket-depth statement splitter (it merged
    the last 13 000 lines of MetricsTests into one statement), so this tool
    carries its own lexer with a mode stack: code, string, interpolation.
    """
    out = list(text)
    n = len(text)
    i = 0
    stack = [["code", 0]]
    open_strings = [False] * (text.count("\n") + 1)
    line = 0

    def blank(start, end):
        nonlocal line
        inside = stack[-1][0] == "string"
        for k in range(start, min(end, n)):
            if out[k] == "\n":
                open_strings[line] = inside
                line += 1
            else:
                out[k] = " "

    while i < n:
        top = stack[-1]
        char = text[i]
        if top[0] in ("code", "interp"):
            if text.startswith("//", i):
                j = text.find("\n", i)
                j = n if j < 0 else j
                blank(i, j)
                i = j
                continue
            if text.startswith("/*", i):
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
                blank(i, j)
                i = j
                continue
            opener = STRING_OPEN.match(text, i)
            if opener:
                hashes = len(opener.group(1))
                quote = opener.group(2)
                width = hashes + len(quote)
                blank(i, i + width)
                i += width
                stack.append(["string", hashes, quote])
                continue
            if top[0] == "interp":
                if char == "(":
                    top[1] += 1
                elif char == ")":
                    if top[1] == 0:
                        blank(i, i + 1)
                        stack.pop()
                        i += 1
                        continue
                    top[1] -= 1
            if char == "\n":
                open_strings[line] = False
                line += 1
            i += 1
            continue
        hashes, quote = top[1], top[2]
        closer = quote + "#" * hashes
        escape = "\\" + "#" * hashes
        if text.startswith(escape + "(", i):
            blank(i, i + len(escape) + 1)
            i += len(escape) + 1
            stack.append(["interp", 0])
            continue
        if text.startswith(escape, i):
            blank(i, i + len(escape) + 1)
            i += len(escape) + 1
            continue
        if text.startswith(closer, i):
            blank(i, i + len(closer))
            i += len(closer)
            stack.pop()
            continue
        if char == "\n" and quote == '"':
            # An unterminated single-line literal: the lexer lost sync rather
            # than the source being wrong. Recover at the line end.
            stack.pop()
            continue
        blank(i, i + 1)
        i += 1
    if with_open_strings:
        return "".join(out), open_strings
    return "".join(out)


# ------------------------------------------------------------------ indexes

def declaration_index():
    """name -> 'core' | 'other', over Sources/."""
    graph = declgraph.Graph(SOURCE_ROOT)
    kind = {}
    for name, owners in graph.owner.items():
        if all(owner.startswith(CORE_PREFIX) for owner in owners):
            kind[name] = "core"
        else:
            # Declared (also) outside the core: not available to the Linux
            # test target, even if a core file happens to declare the name too.
            kind[name] = "other"
    return kind


EXTENSION = re.compile(r"\bextension\s+([A-Z][A-Za-z0-9_]*)[^{]*\{")
MEMBER = re.compile(r"\b(?:static\s+|class\s+|private\s+|public\s+|final\s+)*"
                    r"(?:func|var|let)\s+([a-zA-Z_][A-Za-z0-9_]*)")


def mac_member_index(kind):
    """Members a *non-core* file adds to a core type, by extension.

    `ScratchpadSupport` is in the core; `ScratchpadSupport.markdownPreview` is
    declared in `Sources/Vorssaint/Services/QuickTools/ScratchpadSupport+Mac.swift`,
    which the Linux test target does not compile. A rule that only looks at
    type names cannot see that — run 34692949935 failed on exactly this
    ("type 'ScratchpadSupport' has no member 'markdownPreview'") — so the
    member names of those extensions are collected and treated as Mac-only.
    """
    members = {}
    for dirpath, dirnames, filenames in os.walk(SOURCE_ROOT):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith(".swift"):
                continue
            path = os.path.join(dirpath, name)
            if path.startswith(CORE_PREFIX):
                continue
            raw = open(path, encoding="utf-8", errors="replace").read()
            if "extension " not in raw:
                continue
            text = scrub(raw)
            for match in EXTENSION.finditer(text):
                if kind.get(match.group(1)) != "core":
                    continue
                depth = 1
                i = match.end()
                while i < len(text) and depth:
                    if text[i] == "{":
                        depth += 1
                    elif text[i] == "}":
                        depth -= 1
                    i += 1
                members.setdefault(match.group(1), set()).update(
                    MEMBER.findall(text[match.end():i]))
    return members


# ------------------------------------------------------------------ chunking

class Unit:
    """One top-level statement of the harness body, with its lead comments."""

    def __init__(self, start, lines, stripped, comments):
        self.start = start            # 1-based line number of the first code line
        self.lines = lines            # raw source lines (code only)
        self.stripped = "\n".join(stripped)
        self.comments = comments      # raw comment lines immediately above
        self.declared = declared_names(self.stripped)
        self.refs = set(IDENT.findall(self.stripped))
        # A bare lower-case identifier: not reached through a dot, not an
        # argument label, not a `$0`. If nothing in scope declares it, the
        # statement names something this target does not have — a top-level
        # helper from another Tests file, or a Darwin typealias such as
        # `pid_t` — and must not be emitted.
        self.bare = set(BARE.findall(self.stripped))
        # `if let x, let y {` — the shorthand optional binding. It reads like a
        # declaration and is really a *use* of an outer name: emitting it while
        # the statement that declared `x` was dropped produced "cannot find
        # 'regularBareApp' in scope" (run 34692708425).
        self.member_uses = set(MEMBER_USE.findall(self.stripped))
        self.shorthand = set(SHORTHAND.findall(self.stripped))
        self.declared -= self.shorthand
        self.complete = not (
            (NEEDS_BRACE.match(self.stripped) and "{" not in self.stripped)
            or DANGLING.search(self.stripped.rstrip()))
        self.checks = len(re.findall(r"(?<![.\w])expect(?:Equal|Close|Format)?\s*\(",
                                     self.stripped))

    @property
    def text(self):
        return "\n".join(self.comments + self.lines)


DECL_LET_VAR = re.compile(r"\b(?:let|var|inout)\s+([a-z_][A-Za-z0-9_]*)")
DECL_TUPLE = re.compile(r"\b(?:let|var)\s*\(([^)]*)\)")
DECL_FUNC = re.compile(r"\bfunc\s+([a-zA-Z_][A-Za-z0-9_]*)")
DECL_TYPE = re.compile(r"\b(?:struct|enum|class|actor|protocol|typealias)\s+([A-Z][A-Za-z0-9_]*)")
DECL_FOR = re.compile(r"\bfor\s+(?:try\s+|await\s+)?([a-z_][A-Za-z0-9_]*)\s+in\b")
DECL_FOR_TUPLE = re.compile(r"\bfor\s*\(([^)]*)\)\s+in\b")


DECL_CLOSURE = re.compile(r"[{(]\s*((?:[a-zA-Z_][A-Za-z0-9_]*)"
                          r"(?:\s*,\s*[a-zA-Z_][A-Za-z0-9_]*)*)\s+in\b")
DECL_CASE_LET = re.compile(r"\bcase\s+(?:\.[A-Za-z0-9_]+\s*)?\(?\s*let\s+([a-z_][A-Za-z0-9_]*)")
DECL_PARAMS = re.compile(r"\bfunc\s+[a-zA-Z_][A-Za-z0-9_]*\s*(?:<[^>]*>)?\s*\(")


def parameter_names(text):
    """Internal parameter names of every `func` declared in `text`.

    `func window(_ owner: pid_t, alpha: CGFloat = 1)` declares `owner` and
    `alpha`; the body then uses them bare, so without this they would look
    like references to something the target does not have.
    """
    names = set()
    for match in DECL_PARAMS.finditer(text):
        depth = 1
        i = match.end()
        while i < len(text) and depth:
            if text[i] in "([{":
                depth += 1
            elif text[i] in ")]}":
                depth -= 1
            i += 1
        signature = text[match.end():i - 1]
        depth = 0
        chunk = []
        chunks = []
        for char in signature:
            if char in "([{<":
                depth += 1
            elif char in ")]}>":
                depth -= 1
            if char == "," and depth == 0:
                chunks.append("".join(chunk))
                chunk = []
                continue
            chunk.append(char)
        chunks.append("".join(chunk))
        for piece in chunks:
            head = piece.split(":")[0].strip()
            words = re.findall(r"[a-zA-Z_][A-Za-z0-9_]*|_", head)
            if not words:
                continue
            names.add(words[-1])
    names.discard("_")
    return names


def declared_names(text):
    names = set()
    names.update(DECL_LET_VAR.findall(text))
    names.update(DECL_FUNC.findall(text))
    names.update(DECL_TYPE.findall(text))
    names.update(DECL_FOR.findall(text))
    names.update(DECL_CASE_LET.findall(text))
    names.update(parameter_names(text))
    for group in DECL_CLOSURE.findall(text):
        names.update(re.findall(r"[a-zA-Z_][A-Za-z0-9_]*", group))
    for group in DECL_TUPLE.findall(text) + DECL_FOR_TUPLE.findall(text):
        names.update(re.findall(r"[a-z_][A-Za-z0-9_]*", group))
    names.discard("in")
    return names


# Tokens that can open a line which continues the statement above it rather
# than starting a new one, even at the statement indentation. `where` is the
# one that cost a CI run: `for name in xs.sorted()` / `where NSImage(…) == nil {`
# is one statement written on two lines, and splitting it emitted a for-each
# with no body (run 34692708425, "expected '{' to start the body of for-each
# loop").
CONTINUATIONS = ("else", "while", "where", "catch", "in ",
                 ".", ")", "]", ",", "}", "?", ":",
                 "+", "-", "*", "/", "%", "&&", "||", "??",
                 "==", "!=", "<", ">", "=")

# A unit that opens one of these must contain a brace: otherwise the splitter
# cut a statement in half and emitting it would not compile.
NEEDS_BRACE = re.compile(r"^\s*(?:for|if|guard|while|switch|do|repeat|func|"
                         r"struct|enum|class|actor|extension)\b")
DANGLING = re.compile(r"(?:[=+\-*/%<>!&|^,?:.]|\b(?:where|in|try|return|else|case))\s*$")


def split_units(lines, first, last, indent):
    """Cut lines[first:last] (0-based, half-open) into top-level statements.

    A unit starts on a line at exactly `indent` columns and ends when every
    bracket it opened is closed and the next code line starts a new statement.
    Comments and string literals are neutralised by declgraph.strip_noise, so
    braces inside them cannot confuse the depth count.
    """
    scrubbed, open_strings = scrub("\n".join(lines), with_open_strings=True)
    stripped_all = scrubbed.split("\n")
    units = []
    pending_comments = []
    marks = {}   # index into `units` -> MARK title (a mark seen before that unit)
    i = first
    while i < last:
        raw = lines[i]
        if not raw.strip():
            pending_comments = []
            i += 1
            continue
        if len(raw) - len(raw.lstrip()) != indent:
            # Every statement in the harness body starts at exactly `indent`,
            # and a deeper line is consumed by the statement above it. Landing
            # here means the splitter lost the thread — never drop the line
            # silently, that would generate code that is missing a piece.
            raise SystemExit("port-tests: orphan line %d: %r" % (i + 1, raw))
        if raw.lstrip().startswith("//"):
            mark = MARK.match(raw)
            if mark:
                marks.setdefault(len(units), mark.group(1))
                pending_comments = []
            else:
                pending_comments.append(raw)
            i += 1
            continue
        start = i
        depth = 0
        body = []
        body_stripped = []
        while i < last:
            body.append(lines[i])
            body_stripped.append(stripped_all[i])
            for char in stripped_all[i]:
                if char in "([{":
                    depth += 1
                elif char in ")]}":
                    depth -= 1
            inside_literal = open_strings[i] if i < len(open_strings) else False
            i += 1
            if depth > 0 or inside_literal:
                continue
            # Peek: a continuation line keeps the statement open.
            j = i
            while j < last and not lines[j].strip():
                j += 1
            if j < last:
                nxt = lines[j].lstrip()
                nxt_indent = len(lines[j]) - len(nxt)
                deeper = nxt_indent > indent
                same = nxt_indent == indent and nxt.startswith(CONTINUATIONS) \
                    and not nxt.startswith("//")
                if deeper or same:
                    # Pull in the blank lines too, so the emitted text is the
                    # original text.
                    while i < j:
                        body.append(lines[i])
                        body_stripped.append(stripped_all[i])
                        i += 1
                    continue
            break
        units.append(Unit(start + 1, body, body_stripped, pending_comments))
        pending_comments = []
    return units, marks


def harness_body(path):
    """(lines, first, last, indent) of the statement list to read."""
    lines = open(path, encoding="utf-8").read().split("\n")
    for index, line in enumerate(lines):
        if re.match(r"^\s*(?:static func main\(\)|static func run\(expect:|func \w+Checks\()", line):
            indent = len(line) - len(line.lstrip()) + 4
            # The body ends at the matching close brace: the first line with
            # the opening statement's own indent that is exactly `}`.
            outer = " " * (indent - 4) + "}"
            for end in range(index + 1, len(lines)):
                if lines[end] == outer:
                    return lines, index + 1, end, indent
    return lines, 0, 0, 0


# ------------------------------------------------------------- portability

def classify(units, kind, boundaries=(), excluded=(), mac_members=None):
    """Walk the units in order, deciding which can run on Linux.

    `available` holds the local names introduced by units that were kept. A
    dropped unit poisons every available name it touches, because it may have
    been the statement that gave the name its expected value.
    """
    mac_members = mac_members or {}
    available = set(HARNESS_NAMES)
    effects = {}
    decisions = []
    for index, unit in enumerate(units):
        if index in boundaries:
            # Each `// MARK:` section becomes its own XCTestCase method, so a
            # local binding cannot reach across the boundary: forget them, or
            # the generated code would name variables it never declares.
            available = set(HARNESS_NAMES)
            effects = {}
        reason = None
        if unit.start in excluded:
            reason = "excluded:" + excluded[unit.start]
        elif any(member in mac_members.get(owner, ())
                 for owner, member in unit.member_uses):
            reason = "mac-member:" + sorted(
                "%s.%s" % (owner, member) for owner, member in unit.member_uses
                if member in mac_members.get(owner, ()))[0]
        elif not unit.complete:
            # The splitter cut this statement in half; emitting either half
            # would not compile. Drop it (and, through the poisoning below,
            # everything that depended on it).
            reason = "incomplete:line%d" % unit.start
        for name in sorted(unit.refs | unit.shorthand) if reason is None else []:
            if name in KEYWORDS or name in unit.declared or name in available:
                continue
            if name in FREE_FUNCTIONS or name in ALLOWED_TYPES or name in COMBINE_TYPES:
                continue
            owner = kind.get(name)
            if owner == "core":
                continue
            if owner == "other":
                reason = "mac:" + name
                break
            if name[0].isupper():
                reason = "platform:" + name
                break
            if name in unit.bare:
                reason = "unknown-name:" + name
                break
            # A lower-case name reached through a dot or used as an argument
            # label: it belongs to a receiver that was already checked.
        if reason is None:
            available |= unit.declared
            for declared in unit.declared:
                effects[declared] = unit.refs | unit.declared
            decisions.append((unit, True, None))
        else:
            # Poison every local the dropped statement touched — and, one step
            # further, everything the *declaration* of such a local touches.
            # `expect(SwitcherSupport.…(targetIsMinimized: minimizeIntentMinimized(true)))`
            # drops for a Mac symbol, and the counter it would have bumped
            # lives inside `minimizeIntentMinimized`, not in this statement:
            # without this step the later `expect(minimizeIntentMinimizedReads
            # == 1)` survives and fails on Linux (run 34693146465).
            poisoned = unit.refs & available
            for name in list(poisoned):
                poisoned |= effects.get(name, set()) & available
            available -= (poisoned - HARNESS_NAMES)
            decisions.append((unit, False, reason))
    return decisions


# ------------------------------------------------------------------- output

def camel(title):
    parts = re.findall(r"[A-Za-z0-9]+", title)
    out = "".join(p[:1].upper() + p[1:] for p in parts)
    if out and out[0].isdigit():
        out = "N" + out
    return out or "Section"


HEADER = """// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// GENERATED by Tools/linux-port/port-tests.py — do not edit.
// Source: {source} (WP-16, docs/linux-port/TESTS.md).
// Regenerate: python3 Tools/linux-port/port-tests.py

import Foundation
import XCTest
@testable import VorssaintCore
{combine}
"""

COMBINE_IMPORT = """#if canImport(Darwin)
import Combine
#else
import OpenCombine
#endif
"""

CASE = """
final class {cls}: XCTestCase {{
    func {method}() {{
        var checks = 0
        func expect(_ condition: Bool, _ message: @autoclosure () -> String) {{
            checks += 1
            XCTAssertTrue(condition, message())
        }}
        func expectEqual(_ actual: String, _ expected: String, _ label: String) {{
            checks += 1
            XCTAssertEqual(actual, expected, label)
        }}
        func expectClose(_ actual: Double, _ expected: Double, _ label: String,
                         tol: Double = 0.0001) {{
            checks += 1
            XCTAssertEqual(actual, expected, accuracy: tol, label)
        }}
        func expectFormat(_ format: String, _ expected: [String], _ label: String) {{
            checks += 1
            XCTAssertEqual(GeneratedSupport.formatSpecifiers(in: format), expected, label)
        }}
        func placeholderShape(_ value: String) -> [String] {{
            GeneratedSupport.placeholderShape(value)
        }}
        func formatSpecifiers(in format: String) -> [String] {{
            GeneratedSupport.formatSpecifiers(in: format)
        }}

{body}

        print("[generated-checks] {label} \\(checks)")
    }}
}}
"""


def reindent(text, spaces):
    out = []
    for line in text.split("\n"):
        if not line.strip():
            out.append("")
        else:
            out.append(" " * spaces + line[8:] if line.startswith(" " * 8) else " " * spaces + line.lstrip())
    return "\n".join(out)


def generate(sources, verbose=False):
    kind = declaration_index()
    mac_members = mac_member_index(kind)
    report = []
    files = {}
    used_names = set()
    for source in sources:
        lines, first, last, indent = harness_body(source)
        if last <= first:
            continue
        units, marks = split_units(lines, first, last, indent)
        decisions = classify(units, kind, boundaries=set(marks),
                             excluded=EXCLUDED.get(source, {}),
                             mac_members=mac_members)
        # Group into sections.
        sections = []
        current = {"title": "Prelude", "decisions": []}
        for index, decision in enumerate(decisions):
            if index in marks:
                if current["decisions"]:
                    sections.append(current)
                current = {"title": marks[index], "decisions": []}
            current["decisions"].append(decision)
        if current["decisions"]:
            sections.append(current)

        # Global state the harness sets once and every later section inherits
        # (`MetricFormat.locale` is pinned at the top of MetricsTests and read
        # by sections thousands of lines further down). XCTest gives no order
        # between classes, so each generated case replays the assignments that
        # were in force where its section begins.
        carried = []
        for section in sections:
            section["carried"] = list(carried)
            for unit, kept_flag, _why in section["decisions"]:
                if not kept_flag:
                    continue   # its right-hand side is not portable either
                assign = STATIC_ASSIGN.match(unit.stripped.strip())
                if assign and len(unit.lines) == 1 and not unit.declared:
                    owner, prop = assign.group(1), assign.group(2)
                    if kind.get(owner) == "core":
                        carried = [c for c in carried
                                   if not c[0].startswith("%s.%s " % (owner, prop))]
                        carried.append((unit.lines[0].strip(), owner))

        for section in sections:
            kept = [d for d in section["decisions"] if d[1]]
            dropped = [d for d in section["decisions"] if not d[1]]
            kept_checks = sum(d[0].checks for d in kept)
            drop_checks = sum(d[0].checks for d in dropped)
            report.append({
                "source": source, "title": section["title"],
                "kept_units": len(kept), "dropped_units": len(dropped),
                "kept_checks": kept_checks, "dropped_checks": drop_checks,
                "reasons": sorted({d[2].split(":")[0] for d in dropped}),
                "why": [(d[2], d[0].start, d[0].checks) for d in dropped],
            })
            if kept_checks == 0:
                continue
            base = camel(section["title"])
            name = base
            suffix = 2
            while name in used_names:
                name = "%s%d" % (base, suffix)
                suffix += 1
            used_names.add(name)
            referenced = set().union(*[d[0].refs for d in kept]) if kept else set()
            prologue = [text for text, owner in section["carried"]
                        if owner in referenced]
            body = ""
            if prologue:
                body += "        // Carried over: state an earlier section of "
                body += "%s set and this one inherits.\n" % os.path.basename(source)
                body += "\n".join("        " + line for line in prologue) + "\n\n"
            body += "\n\n".join(reindent(d[0].text, 8) for d in kept)
            needs_combine = any(d[0].refs & COMBINE_TYPES for d in kept)
            files[name] = {
                "source": source,
                "body": CASE.format(cls="Generated%sTests" % name,
                                    method="test%s" % name,
                                    label=name, body=body),
                "combine": needs_combine,
            }
    return files, report


SUPPORT = '''// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Hand-written support for the generated cases (WP-16). `formatSpecifiers`
// and `placeholderShape` are copied verbatim from Tests/MetricsTests.swift,
// where they are private static helpers of the harness struct.

import Foundation

enum GeneratedSupport {
    /// The placeholders a format string carries, sorted, so two languages can
    /// be compared without caring about the order they read in.
    static func placeholderShape(_ value: String) -> [String] {
        formatSpecifiers(in: value).sorted()
    }

    static func formatSpecifiers(in format: String) -> [String] {
        var specifiers: [String] = []
        var index = format.startIndex
        while index < format.endIndex {
            guard format[index] == "%" else {
                index = format.index(after: index)
                continue
            }
            index = format.index(after: index)
            if index < format.endIndex, format[index] == "%" {
                index = format.index(after: index)
                continue
            }
            while index < format.endIndex {
                let character = format[index]
                if character.isLetter || character == "@" {
                    specifiers.append(String(character))
                    index = format.index(after: index)
                    break
                }
                index = format.index(after: index)
            }
        }
        return specifiers
    }
}
'''


def verify():
    """Re-read the emitted files and check them as a whole.

    No Swift compiler is available where this tool runs, so this is the
    strongest local check there is: every generated file must have balanced
    brackets, and every name its body uses must resolve to something the
    Linux test target has — a local declared in the same method, a
    declaration under Sources/VorssaintCore, or the allow-listed standard
    library and Foundation surface.
    """
    kind = declaration_index()
    problems = []
    files = sorted(name for name in os.listdir(OUT_DIR)
                   if name.startswith(GENERATED_PREFIX) and name.endswith(".swift"))
    for name in files:
        path = os.path.join(OUT_DIR, name)
        text = open(path, encoding="utf-8").read()
        # Attributes (`@testable`, `@autoclosure`, `@escaping`) are not names.
        body = re.sub(r"@[A-Za-z_][A-Za-z0-9_]*", " ", scrub(text))
        depth = 0
        for char in body:
            if char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
                if depth < 0:
                    problems.append("%s: unbalanced bracket" % name)
                    break
        if depth != 0:
            problems.append("%s: %d bracket(s) left open" % (name, depth))
        # Shorthand `if let x` binds an outer name; it declares nothing new,
        # so blank those occurrences before reading the declarations out (a
        # name can be both: declared with `let x = …` and rebound later).
        declared = declared_names(SHORTHAND.sub(" ", body)) | HARNESS_NAMES
        for ref in sorted(set(IDENT.findall(body))):
            if ref in KEYWORDS or ref in declared or ref in FREE_FUNCTIONS:
                continue
            if ref in ALLOWED_TYPES or ref in COMBINE_TYPES:
                continue
            if kind.get(ref) == "core":
                continue
            if ref in ("XCTest", "XCTAssertTrue", "XCTAssertEqual", "XCTestCase",
                       "GeneratedSupport", "OpenCombine", "Darwin", "Combine",
                       "VorssaintCore"):
                continue
            if ref in set(BARE.findall(body)) or ref[0].isupper():
                problems.append("%s: unresolved %s" % (name, ref))
    for problem in problems:
        print(problem)
    print("%d file(s) checked, %d problem(s)" % (len(files), len(problems)))
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true",
                        help="print the per-section counts and write nothing")
    parser.add_argument("--sources", nargs="*", default=None)
    parser.add_argument("--verify", action="store_true",
                        help="re-check the emitted files without writing")
    parser.add_argument("--why", action="store_true",
                        help="rank the symbols that keep checks on macOS")
    args = parser.parse_args()

    sources = args.sources or sorted(
        os.path.join(TESTS_DIR, name) for name in os.listdir(TESTS_DIR)
        if name.endswith(".swift"))
    files, report = generate(sources)

    if args.verify:
        raise SystemExit(verify())

    if args.why:
        tally = {}
        for row in report:
            for reason, line, checks in row["why"]:
                entry = tally.setdefault(reason, [0, 0, line])
                entry[0] += 1
                entry[1] += checks
        for reason, (units, checks, line) in sorted(
                tally.items(), key=lambda kv: -kv[1][1])[:60]:
            print("%-44s units=%-5d checks=%-6d first@%d" % (reason, units, checks, line))
        return

    if args.report:
        total_k = total_d = 0
        print("%-52s %7s %7s  %s" % ("section", "linux", "macOS", "why dropped"))
        for row in report:
            total_k += row["kept_checks"]
            total_d += row["dropped_checks"]
            print("%-52s %7d %7d  %s" % (row["title"][:52], row["kept_checks"],
                                         row["dropped_checks"], ",".join(row["reasons"])))
        print("%-52s %7d %7d" % ("TOTAL", total_k, total_d))
        return

    if os.path.isdir(OUT_DIR):
        for name in sorted(os.listdir(OUT_DIR)):
            if name.startswith(GENERATED_PREFIX) and name.endswith(".swift"):
                os.remove(os.path.join(OUT_DIR, name))
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "GeneratedSupport.swift"), "w", encoding="utf-8") as handle:
        handle.write(SUPPORT)
    for name in sorted(files):
        entry = files[name]
        path = os.path.join(OUT_DIR, "%s%s.swift" % (GENERATED_PREFIX, name))
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(HEADER.format(source=entry["source"],
                                       combine=COMBINE_IMPORT if entry["combine"] else ""))
            handle.write(entry["body"])
    print("wrote %d generated case files to %s" % (len(files), OUT_DIR))


if __name__ == "__main__":
    main()
