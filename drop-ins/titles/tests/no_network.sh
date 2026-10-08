#!/bin/sh
# ADR-020, T7's acceptance: the editor and U-Stu Titles link no network
# library, and the editor's Flatpak asks for no network; u-studio-share,
# the helper, does link one.
#
# Checked on what our programs link themselves (readelf's NEEDED), plus
# libsoup, our own network library, anywhere in what they load. Not
# libcurl everywhere: Fedora's libadwaita links libappstream (the About
# dialog's release notes), which links libcurl, so every libadwaita program
# maps it without calling it (docs/developer/notes/titles.md).
# usage: no_network.sh <source root> <program or library>... [-- <u-studio-share>]
set -u
ROOT=$1; shift
command -v ldd >/dev/null && command -v readelf >/dev/null || { echo "no ldd/readelf: skipped"; exit 77; }
fail=0
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    if readelf -d "$1" | grep NEEDED | grep -Ei 'libsoup|libcurl|libnghttp|libwebsockets|libsecret'; then
        echo "FAIL: $1 links a network library"; fail=1
    fi
    if ldd "$1" | grep -i 'libsoup'; then
        echo "FAIL: $1 loads libsoup"; fail=1
    fi
    shift
done
if [ "${1:-}" = "--" ] && [ -n "${2:-}" ]; then
    ldd "$2" | grep -q 'libsoup-3' || { echo "FAIL: $2 doesn't link libsoup (is this the helper?)"; fail=1; }
fi
if grep -Eq '^\s*-\s*--share=network' "$ROOT/packaging/flatpak/com.ustudio.VideoEditor.yml"; then
    echo "FAIL: the editor's Flatpak asks for the network"; fail=1
fi
[ $fail -eq 0 ] && echo ok
exit $fail
