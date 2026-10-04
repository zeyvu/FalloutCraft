#!/usr/bin/env python3
"""FalloutCraft source preprocessor (the same rules as gradle/preprocess.gradle, for checking).

The mod's code lives once, in fabric/src, written for the primary target (Fabric, Minecraft 26.x)
so that build compiles it as it is. The other target (NeoForge, Minecraft 1.21.1) gets a copy made
with these rules:

    //#if EXPR / //#elif EXPR / //#else / //#endif    (EXPR: names, !, &&, ||, parentheses)
        lines in branches that don't apply are blanked; in the branch that applies, lines written
        as "//$$ code" become "code". (In the primary build the other targets' code is just
        comments.)
    renames: whole-word replacements per Minecraft version (1.21.1: Identifier -> ResourceLocation).
    overlays: a file with the same path under a version's overlay folder replaces the shared one
        (an empty overlay file is ignored).

Names: FABRIC, NEOFORGE (the loader), MC_26 (26.x), MC_1_21_1.

    python tools/preprocess.py check                 checks the primary rules in fabric/src
    python tools/preprocess.py gen TARGET OUT        writes TARGET's sources into OUT
                                                     (TARGET: fabric-26, neoforge-1.21.1)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SHARED = ["fabric/src/main/java", "fabric/src/client/java"]

# Minecraft 1.21.1: files left out (phase 1; see the list's comments).
EXCLUDE_1_21_1 = [l.strip() for l in open(os.path.join(ROOT, "versions/1.21.1/exclude.txt"), encoding="utf-8")
                  if l.strip() and not l.startswith("#")]
TARGETS = {
    "fabric-26": {"defines": {"FABRIC", "MC_26"}, "renames": {}, "overlays": [], "exclude": []},
    "neoforge-1.21.1": {
        "defines": {"NEOFORGE", "MC_1_21_1"}, "renames": {"Identifier": "ResourceLocation"},
        "overlays": ["versions/1.21.1/common/src/main/java", "versions/1.21.1/common/src/client/java"],
        # NeoForge's Entity patch removes the call EntitySwimMixin wraps (it swims from the water our
        # fluid mixin reports).
        "exclude": ["dev/skycraft/fabric/", "dev/skycraft/client/fabric/", "dev/skycraft/mixin/EntitySwimMixin.java"] + EXCLUDE_1_21_1,
    },
}
PRIMARY = TARGETS["fabric-26"]["defines"]

DIRECTIVE = re.compile(r"^\s*//#(if|elif|else|endif)\b\s*(.*?)\s*$")
UNCOMMENT = re.compile(r"^(\s*)//\$\$ ?(.*)$")


def evaluate(expr, defines):
    tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*|&&|\|\||!|\(|\)", expr)
    if "".join(tokens) != re.sub(r"\s+", "", expr):
        raise ValueError("bad expression: " + expr)
    pos = 0

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def take():
        nonlocal pos
        pos += 1
        return tokens[pos - 1]

    def p_or():
        v = p_and()
        while peek() == "||":
            take()
            v = p_and() or v
        return v

    def p_and():
        v = p_not()
        while peek() == "&&":
            take()
            v = p_not() and v
        return v

    def p_not():
        if peek() == "!":
            take()
            return not p_not()
        if peek() == "(":
            take()
            v = p_or()
            if take() != ")":
                raise ValueError("missing ) in " + expr)
            return v
        name = take()
        if name is None or not re.match(r"[A-Za-z_]", name):
            raise ValueError("bad expression: " + expr)
        return name in defines

    v = p_or()
    if pos != len(tokens):
        raise ValueError("bad expression: " + expr)
    return v


def process(text, defines, renames, path="?", check=False):
    """Returns the processed text; with check=True, raises if text isn't valid as-is for the primary target."""
    out = []
    stack = []  # (parent_active, active, taken)
    active = True
    problems = []
    for n, line in enumerate(text.split("\n"), 1):
        m = DIRECTIVE.match(line)
        if m:
            kind, expr = m.group(1), m.group(2)
            if kind == "if":
                v = evaluate(expr, defines)
                stack.append((active, active and v, v))
                active = active and v
            elif kind == "elif":
                if not stack:
                    raise ValueError(f"{path}:{n}: #elif without #if")
                parent, _, taken = stack[-1]
                v = (not taken) and evaluate(expr, defines)
                stack[-1] = (parent, parent and v, taken or v)
                active = parent and v
            elif kind == "else":
                if not stack:
                    raise ValueError(f"{path}:{n}: #else without #if")
                parent, _, taken = stack[-1]
                stack[-1] = (parent, parent and not taken, True)
                active = parent and not taken
            else:
                if not stack:
                    raise ValueError(f"{path}:{n}: #endif without #if")
                active = stack.pop()[0]
            out.append("")
            continue
        u = UNCOMMENT.match(line)
        if check:
            if active and u:
                problems.append(f"{path}:{n}: '//$$' line in a branch the primary build uses")
            if not active and line.strip() and not line.strip().startswith("//"):
                problems.append(f"{path}:{n}: live code in a branch the primary build doesn't use (prefix it with //$$)")
        if not active:
            out.append("")
            continue
        if u:
            line = u.group(1) + u.group(2)
        for a, b in renames.items():
            line = re.sub(r"\b" + re.escape(a) + r"\b", b, line)
        out.append(line)
    if stack:
        raise ValueError(f"{path}: unclosed #if")
    if problems:
        raise ValueError("\n".join(problems))
    return "\n".join(out)


def sources(target):
    """{relative path: absolute path} of a target's Java sources, overlays winning."""
    cfg = TARGETS[target]
    files = {}
    for root in SHARED + cfg["overlays"]:
        base = os.path.join(ROOT, root)
        for dirpath, _, names in os.walk(base):
            for name in names:
                if name.endswith(".java"):
                    full = os.path.join(dirpath, name)
                    rel = os.path.relpath(full, base).replace(os.sep, "/")
                    if root not in SHARED and not open(full, encoding="utf-8").read().strip():
                        continue  # an empty overlay file is no overlay (the shared file is used)
                    files[rel] = full
    return {rel: full for rel, full in files.items() if not any(rel.startswith(e) for e in cfg["exclude"])}


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "check":
        bad = 0
        for root in SHARED:
            base = os.path.join(ROOT, root)
            for dirpath, _, names in os.walk(base):
                for name in names:
                    if name.endswith(".java"):
                        p = os.path.join(dirpath, name)
                        try:
                            process(open(p, encoding="utf-8").read(), PRIMARY, {}, p, check=True)
                        except ValueError as e:
                            print(e)
                            bad += 1
        print("ok" if bad == 0 else f"{bad} file(s) with problems")
        sys.exit(1 if bad else 0)
    if len(sys.argv) == 4 and sys.argv[1] == "gen":
        target, out = sys.argv[2], sys.argv[3]
        cfg = TARGETS[target]
        for rel, full in sources(target).items():
            text = process(open(full, encoding="utf-8").read(), cfg["defines"], cfg["renames"], full)
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "w", encoding="utf-8").write(text)
        return
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
