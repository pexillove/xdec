#!/usr/bin/env python3
"""Report Mach-O imports missing from xdec's automatically selected iOS types."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path


IMPORT_RE = re.compile(r"import '([^']+)'")


def run(*args: str) -> str:
    completed = subprocess.run(
        args, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    return completed.stdout


def selected_layers(imports: set[str]) -> list[str]:
    layers = ["ios-sdk"]
    if any(name.startswith("_SecTask") for name in imports):
        layers.append("ios-sdk-security")
    if any(name.startswith("__os_log_") or name.startswith("_os_log_") for name in imports):
        layers.append("ios-sdk-oslog")
    if any(
        name == "_MGCopyAnswer"
        or name == "_notify_post"
        or name.startswith("_container_")
        or name == "_ptrace"
        for name in imports
    ):
        layers.append("ios-sdk-spi")
    return layers


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument(
        "--xdec", type=Path, default=Path("build/bin/xdec.exe"), help="xdec executable"
    )
    args = parser.parse_args()

    relocations = run(str(args.xdec), "relocs", str(args.image), "1000000")
    imports = {match.group(1) for match in IMPORT_RE.finditer(relocations)}
    layers = selected_layers(imports)

    with tempfile.TemporaryDirectory(prefix="xdec-import-types-") as directory:
        output = Path(directory) / "types.json"
        run(str(args.xdec), "types", "parse", *layers, "-o", str(output))
        database = json.loads(output.read_text(encoding="utf-8"))

    declarations = {item["name"] for item in database["declarations"]}
    missing = sorted(imports - declarations)
    print(
        f"{args.image}: {len(imports)} unique imports, "
        f"{len(imports) - len(missing)} typed, {len(missing)} missing"
    )
    for name in missing:
        print(name)
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
