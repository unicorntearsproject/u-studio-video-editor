#!/usr/bin/env python3
"""Write the corresponding-source archive for a release.

    tools/source_archive.py <version> <commit> <state-dir> <out.tar.gz>

The Flatpaks bundle GPL code (FFmpeg with x264, frei0r, MLT), so every
release offers its source (CLAUDE.md, "Project identity"). The archive
holds this repository at <commit> under u-studio-video-editor-<version>/,
and under its third-party-sources/ every upstream source the Flatpak
manifests build: archives from flatpak-builder's download cache (checked
against the manifest's sha256) and git sources as an archive of the pinned
commit from its git mirror. SOURCES.md lists them. Our own patches and
manifests are in the repository part. Run it after the builds, which fill
<state-dir> (build-flatpak/state). Deterministic: the same inputs give the
same file.
"""
import gzip
import hashlib
import io
import pathlib
import subprocess
import sys
import tarfile

import yaml

ROOT = pathlib.Path(__file__).resolve().parent.parent
PACKAGING = ROOT / "packaging" / "flatpak"
MTIME = 0


def sources():
    """(manifest name, source dict) for every archive or git source, once each."""
    seen = set()
    manifests = sorted(PACKAGING.glob("*.yml")) + sorted((PACKAGING / "modules").glob("*.yml"))
    for manifest in manifests:
        doc = yaml.safe_load(manifest.read_text())
        modules = doc.get("modules", [doc]) if isinstance(doc, dict) else []
        for module in modules:
            if not isinstance(module, dict):
                continue  # a module file named by path; it's read on its own
            for source in module.get("sources", []):
                if source.get("type") not in ("archive", "git"):
                    continue
                key = (source.get("url"), source.get("sha256") or source.get("commit"))
                if key not in seen:
                    seen.add(key)
                    yield manifest.name, source


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git_archive(repo, commit, prefix):
    return subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", f"--prefix={prefix}", commit],
                          check=True, capture_output=True).stdout


def add_tar(out, data):
    """Every member of the tar `data`, with fixed owner and mtime."""
    with tarfile.open(fileobj=io.BytesIO(data)) as src:
        for member in src.getmembers():
            member.mtime, member.uid, member.gid, member.uname, member.gname = MTIME, 0, 0, "", ""
            out.addfile(member, src.extractfile(member) if member.isfile() else None)


def add_file(out, name, data):
    info = tarfile.TarInfo(name)
    info.size, info.mtime, info.mode = len(data), MTIME, 0o644
    out.addfile(info, io.BytesIO(data))


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    version, commit, state, out_path = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4])
    if out_path.exists():
        print(f"source-archive: {out_path} exists; not replacing it", file=sys.stderr)
        return 1
    top = f"u-studio-video-editor-{version}/"
    index = [f"# Third-party sources for U Stu Video Editor {version}\n",
             "Built into the Flatpak app and add-ons; each is under its own licence (see its files).\n",
             "| Source | Pinned | File |", "|---|---|---|"]
    entries = []
    for manifest, source in sources():
        url = source["url"]
        if source["type"] == "archive":
            name = url.rsplit("/", 1)[1]
            cached = state / "downloads" / source["sha256"] / name
            if not cached.is_file() or sha256(cached) != source["sha256"]:
                print(f"source-archive: {name} ({manifest}) isn't in {state}/downloads with its sha256; build first",
                      file=sys.stderr)
                return 1
            # GitHub tag archives are all named v<tag>.tar.gz: keep the project's name too.
            stored = name if not name.startswith("v") else f"{url.split('/')[4]}-{name}"
            entries.append(("file", cached.read_bytes(), top + "third-party-sources/" + stored))
            index.append(f"| {url} | sha256 {source['sha256']} | third-party-sources/{stored} |")
        else:
            commit_id = source["commit"]
            mirrors = [d for d in (state / "git").glob("*") if subprocess.run(
                ["git", "-C", str(d), "cat-file", "-e", commit_id + "^{commit}"], capture_output=True).returncode == 0]
            if not mirrors:
                print(f"source-archive: no git mirror in {state}/git has {url} at {commit_id}; build first",
                      file=sys.stderr)
                return 1
            name = url.rstrip("/").rsplit("/", 1)[1].removesuffix(".git") + "-" + commit_id[:12]
            entries.append(("tar", git_archive(mirrors[0], commit_id, top + f"third-party-sources/{name}/"), None))
            index.append(f"| {url} | commit {commit_id} | third-party-sources/{name}/ |")
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as out:
        add_tar(out, git_archive(ROOT, commit, top))
        for kind, data, name in entries:
            if kind == "file":
                add_file(out, name, data)
            else:
                add_tar(out, data)
        add_file(out, top + "third-party-sources/SOURCES.md", ("\n".join(index) + "\n").encode())
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "xb") as f, gzip.GzipFile(fileobj=f, mode="wb", mtime=MTIME, filename="") as gz:
        gz.write(raw.getvalue())
    print(f"source-archive: {out_path} ({len(entries)} third-party sources)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
