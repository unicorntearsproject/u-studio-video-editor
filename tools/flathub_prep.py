#!/usr/bin/env python3
"""Assemble the Flathub submission directory for a release tag.

    tools/flathub_prep.py <tag> [outdir]

Reads packaging/flatpak/<app-id>.yml (the manifest `just flatpak` builds),
swaps its local `dir` source for this repo's git URL pinned to <tag> and the
commit it points at, and writes the manifest, the shared module files, their
patches and flathub.json to outdir (default build-flathub/<app-id>/): the files that go
into the flathub/<app-id> repository, or into the new-pr submission branch.

Refuses a tag that isn't on origin: Flathub builds offline from pinned,
public sources, so an unpushed tag would fail there.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys

import check_release_notes

# USTUDIO_FLATHUB_REPO points the tag lookup (and the generated source) at
# another repository, e.g. a local clone with a test tag.
REPO_URL = os.environ.get("USTUDIO_FLATHUB_REPO", "https://github.com/unicorntearsproject/u-studio-video-editor.git")
ROOT = pathlib.Path(__file__).resolve().parent.parent
PACKAGING = ROOT / "packaging" / "flatpak"
# The app source block in the local manifest, from its `sources:` line to
# the end of the file (it is the last module). Anchored on the `type: dir`
# line so an edit that moves it fails loudly here instead of shipping a
# local source to Flathub.
DIR_SOURCE = re.compile(r"(?ms)^(    sources:\n      - type: dir\n.*)\Z")


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    tag = sys.argv[1]
    if problem := check_release_notes.check():
        print(problem, file=sys.stderr)
        return 1
    # The app's manifest; the drop-in extensions' manifests beside it
    # (com.ustudio.VideoEditor.DropIn.*.yml) aren't part of this submission.
    app_id = "com.ustudio.VideoEditor"
    manifest = PACKAGING / f"{app_id}.yml"

    remote = subprocess.run(["git", "ls-remote", "--tags", REPO_URL, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
                            capture_output=True, text=True, check=True).stdout.split("\n")
    refs = dict(reversed(line.split("\t")) for line in remote if line)
    # An annotated tag's own object is not a commit; `^{}` is what it points at.
    commit = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
    if not commit:
        print(f"tag {tag} is not on {REPO_URL}; push it first", file=sys.stderr)
        return 1

    text = manifest.read_text()
    if not DIR_SOURCE.search(text):
        print(f"{manifest.name}: the app's `type: dir` source block wasn't found at the end", file=sys.stderr)
        return 1
    git_source = (
        f"    sources:\n      - type: git\n        url: {REPO_URL}\n        tag: {tag}\n        commit: {commit}\n"
        "        x-checker-data:\n          type: git\n          tag-pattern: ^v([\\d.]+)$\n")
    # A function, not a template string: the tag pattern's `\d` must stay literal.
    text = DIR_SOURCE.sub(lambda _: git_source, text)

    out = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else ROOT / "build-flathub" / app_id
    if out.exists():
        shutil.rmtree(out)
    (out / "modules").mkdir(parents=True)
    (out / manifest.name).write_text(text)
    for module in sorted((PACKAGING / "modules").glob("*.yml")):
        shutil.copy2(module, out / "modules" / module.name)
    # Module files name their patches as ../patches/<file>, so the layout
    # (modules/ beside patches/) is kept.
    if (PACKAGING / "patches").is_dir():
        shutil.copytree(PACKAGING / "patches", out / "patches")
    # x86_64 only until an aarch64 build has been run and smoke-tested.
    (out / "flathub.json").write_text('{\n  "only-arches": ["x86_64"]\n}\n')
    print(f"{out}: {app_id} at {tag} ({commit[:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
