#!/usr/bin/env python3
"""Rescan src/ for `$mdi-foo` icon references and rewrite src/plugins/icons.js
so the generated alias map matches.

Run from webapp/ after adding or removing icon usages.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "src")
OUT_PATH = os.path.join(SRC_DIR, "plugins", "icons.js")


def kebab_to_camel(name: str) -> str:
    parts = name.split("-")
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


def main() -> None:
    names: set[str] = set()
    for root, _, files in os.walk(SRC_DIR):
        for fname in files:
            if not fname.endswith((".vue", ".js")):
                continue
            if fname == "icons.js":
                continue
            path = os.path.join(root, fname)
            with open(path) as fh:
                content = fh.read()
            names.update(re.findall(r"\$mdi-([a-z][a-z0-9-]*)", content))

    # Skip placeholder/example identifiers that may show up in comments.
    names.discard("foo")
    names.discard("bar")

    if not names:
        print("No icons referenced — leaving icons.js untouched.")
        return

    sorted_names = sorted(names)
    js_idents = [kebab_to_camel("mdi-" + n) for n in sorted_names]
    imports = ", ".join(js_idents)
    aliases = ",\n  ".join(
        f'"mdi-{n}": {kebab_to_camel("mdi-" + n)}' for n in sorted_names
    )

    out = (
        "// Auto-generated icon registry. Vuetify is configured to use the mdi-svg\n"
        "// iconset; templates reference icons via \"$mdi-foo\" aliases (with the $\n"
        "// prefix telling Vuetify to look up the alias map below).\n"
        "//\n"
        "// To add a new icon: write `icon=\"$mdi-foo\"` in a template, then re-run\n"
        "// scripts/regenerate-icons.py — that script will rescan src/ for usages\n"
        "// and rewrite this file.\n"
        f"import {{ {imports} }} from \"@mdi/js\";\n"
        "\n"
        f"export const aliases = {{\n  {aliases},\n}};\n"
    )

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w") as fh:
        fh.write(out)
    print(f"wrote {OUT_PATH} with {len(sorted_names)} icons")


if __name__ == "__main__":
    main()
