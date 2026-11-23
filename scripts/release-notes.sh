#!/bin/sh
# Print release notes for a tag: a short intro plus the commits since the
# previous v* tag. Usage: scripts/release-notes.sh <tag>
set -e

tag="$1"
[ -n "$tag" ] || { echo "usage: $0 <tag>" >&2; exit 1; }

prev=$(git describe --tags --abbrev=0 --match 'v*' "${tag}^" 2>/dev/null || true)

echo "## lvm $tag"
echo
echo "Prebuilt binaries, libraries and headers are attached for Linux, macOS and Windows."
echo "SHA-256 checksums are in \`SHA256SUMS\`."
echo
if [ -n "$prev" ]; then
    echo "### Changes since $prev"
    range="$prev..$tag"
else
    echo "### Changes"
    range="$tag"
fi
echo
git log --no-merges --format='- %s (%h)' "$range"
