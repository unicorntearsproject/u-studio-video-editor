# Local dev loop -- mirrors exactly what CI runs (docs/plans/v2/11-build-test-ci-packaging.md)
# so a green `just test` locally means a green CI run.

builddir := "builddir"

# One-time (or after meson.build changes) configure.
setup:
    meson setup {{builddir}} -Dbuildtype=debug -Dtests=enabled

build:
    meson compile -C {{builddir}}

test: build
    tools/meson-test.sh -C {{builddir}} --print-errorlogs

run: build
    ./{{builddir}}/src/app/u-studio-video-editor

# Format every tracked .cpp/.h in place (excludes vendored subprojects/).
fmt:
    clang-format -i $(git ls-files '*.cpp' '*.h' ':(exclude)subprojects/*')

# Sanitizer runs (docs/audit/2026-09-23-sanitizer-report.md), each in its own
# build dir. Both should pass clean; any report is then a real finding.
# asan: ASan + UBSan, leak check on, with tests/sanitizers/lsan.supp covering
# the leaks that aren't ours (MLT loader and repository, FFmpeg worker
# threads, SDL).
# - fast_unwind_on_malloc=0: MLT modules have no frame pointers, so the
#   default unwinder stops at the first module frame and the suppressions
#   never see the libmlt frames past it.
# - LD_PRELOAD of the FFmpeg libraries MLT's avformat module links: without
#   it, Factory::close() unloads them before the leak report, so their
#   worker-thread buffers show only "<unknown module>" frames that no
#   suppression can match. verify_asan_link_order=0 allows the preload.
#   These libraries don't define malloc, so ASan's interception still works.
asan_ffmpeg := "/lib64/libavutil.so.60 /lib64/libavcodec.so.62 /lib64/libavformat.so.62 /lib64/libswscale.so.9 /lib64/libswresample.so.6 /lib64/libx264.so.165"
# The same for MLT's movit module (ADR-019), so its glsl.manager, which MLT
# never frees (docs/developer/notes/gpu.md), can be suppressed by name. Only
# for the GPU tests (names with "gpu"): preloaded, its exported
# filter_*_init functions override same-named ones in modules loaded later,
# so plus's lift_gamma_gain becomes movit's, which fails without GL (both
# export filter_lift_gamma_gain_init; the effects drop-in's brand looks use
# plus's).
asan_movit := "/usr/lib64/mlt-7/libmltmovit.so"
# The same for the avformat and frei0r modules' own leaks (the effects
# drop-in's tests; tests/sanitizers/lsan.supp).
asan_modules := "/usr/lib64/mlt-7/libmltavformat.so /usr/lib64/mlt-7/libmltfrei0r.so"
asan *tests:
    #!/usr/bin/env bash
    set -u
    [ -d builddir-asan ] || meson setup builddir-asan -Db_sanitize=address,undefined -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-asan || exit 1
    wanted="{{tests}}"
    [ -n "$wanted" ] || wanted=$(meson test -C builddir-asan --list 2>/dev/null | sed 's/^[^:]*://')
    gpu=(); rest=()
    # With movit preloaded: every test that starts a GPU session (its
    # glsl.manager is only suppressible by name then), not just *gpu* names.
    for t in $wanted; do case "$t" in *gpu*|dropin-engine|engine-hardware-decode) gpu+=("$t") ;; *) rest+=("$t") ;; esac; done
    run() { # preload, tests...
        local preload=$1; shift
        LD_PRELOAD="$preload" \
        ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:verify_asan_link_order=0:detect_stack_use_after_return=1:halt_on_error=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        LSAN_OPTIONS=suppressions={{justfile_directory()}}/tests/sanitizers/lsan.supp \
            tools/meson-test.sh -C builddir-asan -t 6 --print-errorlogs "$@"
    }
    status=0
    if [ ${#rest[@]} -gt 0 ]; then run "{{asan_ffmpeg}} {{asan_modules}}" "${rest[@]}" || status=1; fi
    if [ ${#gpu[@]} -gt 0 ]; then run "{{asan_ffmpeg}} {{asan_movit}} {{asan_modules}}" "${gpu[@]}" || status=1; fi
    exit $status

# Both sanitizer recipes take optional test names (`just tsan engine-thread`)
# to run only those tests; with none they run the whole suite.
#
# tsan: ThreadSanitizer. ignore_noninstrumented_modules hides accesses from
# uninstrumented libraries (GLib's futex-based main-context lock looks like a
# race to TSan). report_thread_leaks=0 because the only thread leaks are
# threads created inside MLT modules that are unloaded before exit, and our
# own threads are std::thread, which terminate() if not joined anyway.
tsan *tests:
    [ -d builddir-tsan ] || meson setup builddir-tsan -Db_sanitize=thread -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-tsan
    TSAN_OPTIONS=ignore_noninstrumented_modules=1:report_thread_leaks=0:second_deadlock_stack=1:halt_on_error=1 \
        tools/meson-test.sh -C builddir-tsan -t 10 --print-errorlogs {{tests}}

# The factory-policy test alone -- the "no Qt in the process" proof.
check-qt: build
    ./{{builddir}}/tests/engine/test_factory_policy

# The tester Flatpak (packaging/flatpak/): builds MLT, FFmpeg + x264 and
# the app against the GNOME runtime, then a single-file bundle at
# build-flatpak/u-studio-video-editor-<version>.flatpak. Needs flatpak-builder
# and the Flathub remote (the runtime and SDK install as --user). The first
# build downloads and compiles FFmpeg and MLT; later ones reuse the cache in
# build-flatpak/state. --runtime-repo embeds Flathub in the bundle, so
# `flatpak install --user ./file.flatpak` or a software centre can fetch the
# runtime.
version := `sed -n "s/^  version: '\(.*\)',$/\1/p" meson.build`
flatpak:
    python3 tools/check_release_notes.py
    flatpak-builder --user --force-clean --install-deps-from=flathub \
        --state-dir=build-flatpak/state \
        build-flatpak/app packaging/flatpak/com.ustudio.VideoEditor.yml
    # Nothing from a test run or this machine may ship: checked between
    # the build and the export, so a failure leaves no bundle behind.
    # --export-only then exports as a plain --repo build would, splitting
    # debug info and translations into their .Debug/.Locale refs.
    python3 tools/check_bundle_clean.py build-flatpak/app/files
    flatpak-builder --user --export-only --state-dir=build-flatpak/state --repo=build-flatpak/repo \
        build-flatpak/app packaging/flatpak/com.ustudio.VideoEditor.yml
    flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
        build-flatpak/repo build-flatpak/u-studio-video-editor-{{version}}.flatpak com.ustudio.VideoEditor
    @ls -lh build-flatpak/u-studio-video-editor-{{version}}.flatpak
    just dist build-flatpak/u-studio-video-editor-{{version}}.flatpak

# The titles drop-in's Flatpak extension (packaging/flatpak/
# com.ustudio.VideoEditor.DropIn.Titles.yml), bundled as
# build-flatpak/u-studio-video-editor-dropin-titles-<version>.flatpak. It
# builds against the app, so the app bundle from `just flatpak` (this same
# version) must be installed in the installation flatpak-builder uses: set
# FLATPAK_USER_DIR to build against a scratch installation instead of your
# own. It installs nothing (--install-deps-from would try to update the app
# from its origin; `just flatpak` has already installed the SDK).
# The titles extension bundle, built against the installed app.
flatpak-titles:
    python3 tools/check_release_notes.py
    flatpak-builder --user --force-clean --state-dir=build-flatpak/state \
        build-flatpak/titles packaging/flatpak/com.ustudio.VideoEditor.DropIn.Titles.yml
    python3 tools/check_bundle_clean.py build-flatpak/titles/files
    flatpak-builder --user --export-only --state-dir=build-flatpak/state --repo=build-flatpak/repo \
        build-flatpak/titles packaging/flatpak/com.ustudio.VideoEditor.DropIn.Titles.yml
    flatpak build-bundle --runtime --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
        build-flatpak/repo build-flatpak/u-studio-video-editor-dropin-titles-{{version}}.flatpak \
        com.ustudio.VideoEditor.DropIn.Titles
    @ls -lh build-flatpak/u-studio-video-editor-dropin-titles-{{version}}.flatpak
    just dist build-flatpak/u-studio-video-editor-dropin-titles-{{version}}.flatpak

# Copies a packaged artifact into the distribution folder with a .sha256
# sidecar. Never overwrites: an existing name gets -2, -3, ... before the
# extension (owner's rule, 2026-09-25). `just flatpak` runs it; set
# USTUDIO_DIST_DIR to copy somewhere else.
dist_dir := env_var_or_default("USTUDIO_DIST_DIR", env_var("HOME") / "projects/_software-dist/u-stu-video-editor")
dist artifact:
    #!/usr/bin/env bash
    set -euo pipefail
    src="{{artifact}}"
    mkdir -p "{{dist_dir}}"
    name=$(basename "$src"); stem="${name%.*}"; ext="${name##*.}"
    dest="{{dist_dir}}/$name"; n=2
    while [ -e "$dest" ] || [ -e "$dest.sha256" ]; do
        dest="{{dist_dir}}/$stem-$n.$ext"; n=$((n + 1))
    done
    cp --no-clobber "$src" "$dest"
    (cd "{{dist_dir}}" && sha256sum "$(basename "$dest")" > "$(basename "$dest").sha256")
    echo "dist: $dest"
    cat "$dest.sha256"

# The effects drop-in's Flatpak extension (packaging/flatpak/
# com.ustudio.VideoEditor.DropIn.Effects.yml), bundled as
# build-flatpak/u-studio-video-editor-dropin-effects-<version>.flatpak, with
# frei0r and MLT's frei0r module inside it. Built like flatpak-titles:
# against the installed app of this same version (FLATPAK_USER_DIR for a
# scratch installation).
# The effects extension bundle, built against the installed app.
flatpak-effects:
    python3 tools/check_release_notes.py
    flatpak-builder --user --force-clean --state-dir=build-flatpak/state \
        build-flatpak/effects packaging/flatpak/com.ustudio.VideoEditor.DropIn.Effects.yml
    python3 tools/check_bundle_clean.py build-flatpak/effects/files
    flatpak-builder --user --export-only --state-dir=build-flatpak/state --repo=build-flatpak/repo \
        build-flatpak/effects packaging/flatpak/com.ustudio.VideoEditor.DropIn.Effects.yml
    flatpak build-bundle --runtime --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
        build-flatpak/repo build-flatpak/u-studio-video-editor-dropin-effects-{{version}}.flatpak \
        com.ustudio.VideoEditor.DropIn.Effects
    @ls -lh build-flatpak/u-studio-video-editor-dropin-effects-{{version}}.flatpak
    just dist build-flatpak/u-studio-video-editor-dropin-effects-{{version}}.flatpak

# Publishes a packaged artifact to the public download bucket (owner,
# 2026-09-28): s3://ut-software-dist/ under its versioned name and its
# -latest name (the version replaced by "latest"), each with a .sha256
# naming that file. Uses the default AWS profile. The bucket policy makes
# every object public (ACLs are disabled), so no ACL is set. A versioned
# object that already exists with other contents is never replaced;
# -latest is, with a short Cache-Control so the CDN picks it up.
publish_bucket := env_var_or_default("USTUDIO_PUBLISH_BUCKET", "ut-software-dist")
publish artifact:
    #!/usr/bin/env bash
    set -euo pipefail
    src="{{artifact}}"; name=$(basename "$src")
    case "$name" in *"{{version}}"*) ;; *) echo "publish: $name doesn't carry version {{version}}" >&2; exit 1 ;; esac
    latest="${name/{{version}}/latest}"
    case "$name" in *.flatpak) type=application/vnd.flatpak ;; *) type=application/octet-stream ;; esac
    sum=$(sha256sum "$src" | cut -d' ' -f1)
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    printf '%s  %s\n' "$sum" "$name" > "$tmp/$name.sha256"
    printf '%s  %s\n' "$sum" "$latest" > "$tmp/$latest.sha256"
    if aws s3api head-object --bucket "{{publish_bucket}}" --key "$name" >/dev/null 2>&1; then
        aws s3 cp --quiet "s3://{{publish_bucket}}/$name.sha256" "$tmp/existing.sha256" 2>/dev/null || true
        if ! grep -q "^$sum " "$tmp/existing.sha256" 2>/dev/null; then
            echo "publish: s3://{{publish_bucket}}/$name exists with other contents; not replacing it" >&2; exit 1
        fi
        echo "publish: $name is already there (same sha256)"
    else
        aws s3 cp --only-show-errors --content-type "$type" "$src" "s3://{{publish_bucket}}/$name"
        aws s3 cp --only-show-errors --content-type text/plain "$tmp/$name.sha256" "s3://{{publish_bucket}}/$name.sha256"
    fi
    aws s3 cp --only-show-errors --content-type "$type" --cache-control max-age=300 "$src" "s3://{{publish_bucket}}/$latest"
    aws s3 cp --only-show-errors --content-type text/plain --cache-control max-age=300 "$tmp/$latest.sha256" \
        "s3://{{publish_bucket}}/$latest.sha256"
    echo "published: $name, $latest (+ .sha256), sha256 $sum"

# The release's corresponding source (the Flatpaks bundle GPL code: FFmpeg
# with x264, frei0r, MLT): this repository at HEAD plus every upstream
# source the manifests build, from build-flatpak/state, so run it after
# `just flatpak`, `flatpak-titles` and `flatpak-effects`. Written to
# build-flatpak/u-studio-video-editor-<version>-source.tar.gz and copied to
# the dist folder with its .sha256 (`just publish` uploads it like a bundle).
# The release's GPL corresponding-source archive (after the three builds).
source-archive:
    python3 tools/source_archive.py {{version}} "$(git rev-parse HEAD)" build-flatpak/state \
        build-flatpak/u-studio-video-editor-{{version}}-source.tar.gz
    just dist build-flatpak/u-studio-video-editor-{{version}}-source.tar.gz

# A release directory for software.rustybucket.ai (owner, 2026-10-08), to RBA
# Infra's spec: tools/release_dir.py has the details. From the bundles and
# the source archive `just dist` copied; signed when USTUDIO_SIGNING_KEY
# names the product key (the owner's; nothing here picks one). Never
# rewrites an existing releases/<version>/.
# The signed release directory for software.rustybucket.ai.
release-dir version:
    python3 tools/release_dir.py {{version}} "{{dist_dir}}" ${USTUDIO_SIGNING_KEY:+--key "$USTUDIO_SIGNING_KEY"}

# Drop-in configurations (ADR-013/014, doc 15 "Gating"): the full suite with
# every drop-in built in, or every one as a loadable module (each in its own
# build dir). The default build (`just test`) has them all disabled. Only
# the drop-ins whose folder is in the tree are enabled (meson refuses an
# option for a missing folder); a build dir made before a new drop-in
# landed needs `meson configure` or deleting.
dropins-builtin:
    [ -d builddir-dropins-builtin ] || meson setup builddir-dropins-builtin -Dtests=enabled $(for d in drop-ins/*/meson.build; do d=${d#drop-ins/}; printf -- '-Ddropin_%s=builtin ' "${d%/meson.build}"; done)
    meson compile -C builddir-dropins-builtin  # meson test builds only the tests' own dependencies, not u-studio-render
    tools/meson-test.sh -C builddir-dropins-builtin --print-errorlogs

dropins-module:
    [ -d builddir-dropins-module ] || meson setup builddir-dropins-module -Dtests=enabled $(for d in drop-ins/*/meson.build; do d=${d#drop-ins/}; printf -- '-Ddropin_%s=module ' "${d%/meson.build}"; done)
    meson compile -C builddir-dropins-module  # meson test builds only the tests' own dependencies, not u-studio-render
    tools/meson-test.sh -C builddir-dropins-module --print-errorlogs
