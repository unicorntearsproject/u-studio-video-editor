#!/usr/bin/env python3
"""Make the release directory for software.rustybucket.ai.

    tools/release_dir.py <version> <dist-dir> [--key FINGERPRINT] [--released YYYY-MM-DD]

From the bundles and source archive `just dist` put in <dist-dir> (our
names), writes <dist-dir>/releases/<version>/ to RBA Infra's spec
(2026-10-08), flat, nothing else in it:

  u-studio-video-editor[-dropin-titles|-dropin-effects]-<v>-linux-x86_64.flatpak
  u-studio-video-editor-<v>-source.tar.gz   (required whenever a Flatpak ships)
  u-studio-video-editor-<v>-SHA256SUMS      (the files above, bare names)
  u-studio-video-editor-latest.json         (the signed manifest)
  <file>.asc for every file above, each holding exactly one signature

--key is the product key (its primary, or a signing subkey of it); the
manifest records the PRIMARY fingerprint. Without --key everything but the
.asc files is written, the manifest's key_fingerprint is empty, and the
directory is NOT ready to hand over. A published version is never
rewritten: an existing directory is refused. Every input must match its
.sha256 sidecar first.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

PRODUCT = "u-studio-video-editor"
REPO = "https://github.com/unicorntearsproject/u-studio-video-editor.git"
BASE_URL = "https://software.rustybucket.ai/" + PRODUCT
# SemVer 2.0 without build metadata (RBA's rule).
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(-(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*)?$")
# manifest key -> (our dist name, their name, arch); the source last.
ENTRIES = [
    ("linux-flatpak", PRODUCT + "-{v}.flatpak", PRODUCT + "-{v}-linux-x86_64.flatpak", "x86_64"),
    ("linux-flatpak-titles", PRODUCT + "-dropin-titles-{v}.flatpak", PRODUCT + "-dropin-titles-{v}-linux-x86_64.flatpak",
     "x86_64"),
    ("linux-flatpak-effects", PRODUCT + "-dropin-effects-{v}.flatpak",
     PRODUCT + "-dropin-effects-{v}-linux-x86_64.flatpak", "x86_64"),
    ("source", PRODUCT + "-{v}-source.tar.gz", PRODUCT + "-{v}-source.tar.gz", "noarch"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fail(message):
    print("release-dir: " + message, file=sys.stderr)
    sys.exit(1)


def primary_fingerprint(key):
    """The primary key's fingerprint for a key given by its primary or subkey fingerprint."""
    out = subprocess.run(["gpg", "--batch", "--with-colons", "--fingerprint", "--fingerprint", key],
                         capture_output=True, text=True)
    if out.returncode != 0:
        fail(f"no OpenPGP key {key} in this keyring")
    fprs = [line.split(":")[9] for line in out.stdout.splitlines() if line.startswith("fpr:")]
    return fprs[0].upper()


def sign(key, path):
    """A detached armoured signature with exactly one signature, verified."""
    subprocess.run(["gpg", "--batch", "--yes", "--armor", "--detach-sign", "--local-user", key + "!",
                    "--output", str(path) + ".asc", str(path)], check=True)
    status = subprocess.run(["gpg", "--batch", "--status-fd", "1", "--verify", str(path) + ".asc", str(path)],
                            capture_output=True, text=True)
    if status.returncode != 0 or status.stdout.count("[GNUPG:] VALIDSIG") != 1:
        fail(f"the signature of {path.name} doesn't verify as exactly one good signature")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version")
    parser.add_argument("dist", type=pathlib.Path)
    parser.add_argument("--key", default="")
    parser.add_argument("--released", default=datetime.date.today().isoformat())
    args = parser.parse_args()
    v = args.version
    if not SEMVER.match(v):
        fail(f"{v} isn't SemVer 2.0 without build metadata")
    out = args.dist / "releases" / v
    if out.exists():
        fail(f"{out} exists; a published release is never rewritten")

    present = []
    for key, ours, theirs, arch in ENTRIES:
        src = args.dist / ours.format(v=v)
        if not src.is_file():
            continue
        side = pathlib.Path(str(src) + ".sha256")
        if not side.is_file() or side.read_text().split()[0] != sha256(src):
            fail(f"{src.name} doesn't match its .sha256 (or has none)")
        present.append((key, src, theirs.format(v=v), arch))
    keys = {p[0] for p in present}
    if not keys - {"source"}:
        fail(f"no bundles for {v} in {args.dist} (just dist first)")
    if "source" not in keys:
        fail(f"{PRODUCT}-{v}-source.tar.gz is missing: it's required with every Flatpak (just source-archive)")

    primary = primary_fingerprint(args.key) if args.key else ""
    out.mkdir(parents=True)
    files = {}
    for key, src, name, arch in present:
        dest = out / name
        shutil.copyfile(src, dest)
        url = f"{BASE_URL}/{v}/{name}"
        files[key] = {"arch": arch, "name": name, "url": url, "signature_url": url + ".asc",
                      "size": dest.stat().st_size, "sha256": sha256(dest)}
    sums = out / f"{PRODUCT}-{v}-SHA256SUMS"
    sums.write_text("".join(f"{f['sha256']}  {f['name']}\n" for f in files.values()))
    manifest = {"schema": 1, "version": v, "released": args.released, "key_fingerprint": primary, "files": files}
    # The tag's commit, when the tag is already on the public repo (it must exist before publishing).
    # An annotated tag lists its own object first and the commit as <ref>^{}; a lightweight one only the commit.
    refs = dict(reversed(line.split("\t")) for line in subprocess.run(
        ["git", "ls-remote", REPO, f"refs/tags/v{v}^{{}}", f"refs/tags/v{v}"],
        capture_output=True, text=True).stdout.splitlines() if "\t" in line)
    tag = refs.get(f"refs/tags/v{v}^{{}}") or refs.get(f"refs/tags/v{v}")
    if tag:
        manifest["commit"] = tag
    (out / f"{PRODUCT}-latest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run(["sha256sum", "-c", "--quiet", sums.name], cwd=out, check=True)

    if args.key:
        for path in sorted(p for p in out.iterdir() if p.suffix != ".asc"):
            sign(args.key, path)
        print(f"release-dir: {out} (signed by {primary})")
    else:
        print(f"release-dir: {out} UNSIGNED: no .asc files and an empty key_fingerprint; not ready to hand over")
    if not tag:
        print(f"release-dir: tag v{v} isn't on {REPO} yet; RBA's publish refuses without it")
    for p in sorted(out.iterdir()):
        print(f"  {p.name}  {p.stat().st_size}")


if __name__ == "__main__":
    main()
