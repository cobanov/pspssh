#!/usr/bin/env bash
# Cut a release, refusing to cut a dishonest one.
#
#   tools/release.sh 1.1.0 "a title"
#   tools/release.sh --check          # just verify the invariants, change nothing
#
# ## Why this exists rather than a tag and a paragraph
#
# v1.0.0 shipped with PSPSSH_VERSION still reading "0.4.1", so the banner on the
# first screen named a build that was not the one running. That constant has one
# job — answering "did I copy the new one across?" on a device flashed by hand —
# and a stale value answers it confidently and wrongly, which is worse than
# leaving the question open.
#
# The fix is not to remember harder. It is to make the mismatch impossible to
# ship: this refuses to tag anything whose header disagrees with the tag.
set -euo pipefail

cd "$(dirname "$0")/.."

HEADER=src/core/pspssh.h

header_version() {
    sed -n 's/^#define PSPSSH_VERSION "\(.*\)"$/\1/p' "$HEADER"
}

latest_tag() {
    git describe --tags --abbrev=0 2>/dev/null || echo ""
}

fail() { echo "  refusing: $*" >&2; exit 1; }

# --check verifies the release-time invariants against the current tree, so the
# same rules can run before a release rather than only during one.
if [ "${1:-}" = "--check" ]; then
    tag="$(latest_tag)"
    version="$(header_version)"
    echo "  header  $version"
    echo "  tag     ${tag:-none}"
    [ -n "$version" ] || fail "$HEADER has no PSPSSH_VERSION"

    # The invariant is "a tag never names a version different from the header at
    # that commit" — not "the header always equals the last tag". Between
    # releases the header is deliberately ahead, which is what a version bump
    # is, and failing on that would make the check something to work around.
    #
    # So it is only decidable when this commit is itself tagged.
    if exact="$(git describe --tags --exact-match 2>/dev/null)"; then
        if [ "v$version" != "$exact" ]; then
            fail "this commit is tagged $exact and the header says $version"
        fi
        echo "  this commit is $exact, and the header agrees"
    elif [ -n "$tag" ]; then
        echo "  ahead of $tag, which is what an unreleased version bump is"
    fi
    exit 0
fi

VERSION="${1:-}"
TITLE="${2:-}"

if [ -z "$VERSION" ]; then
    echo "usage: tools/release.sh <version> [title]" >&2
    echo "       tools/release.sh --check" >&2
    exit 2
fi

case "$VERSION" in
    v*) fail "give the version without the leading v (e.g. 1.1.0)" ;;
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) fail "\"$VERSION\" is not a major.minor.patch version" ;;
esac

TAG="v$VERSION"

# The invariant this script exists for.
HEADER_VERSION="$(header_version)"
if [ "$HEADER_VERSION" != "$VERSION" ]; then
    fail "$HEADER says \"$HEADER_VERSION\" but you asked to release $VERSION.
             Update the header, commit it, and run this again — the banner has
             to name the build that is actually running."
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$BRANCH" = "main" ] || fail "releases come from main, and this is $BRANCH"

[ -z "$(git status --porcelain)" ] || fail "the working tree has uncommitted changes"

git fetch --quiet origin main
if [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]; then
    fail "local main and origin/main have diverged — push or pull first"
fi

if git rev-parse "$TAG" >/dev/null 2>&1; then
    fail "$TAG already exists"
fi

# A release nobody can build is not a release.
#
# Built from a fresh clone rather than from this working tree, and that is the
# whole point of the step. v1.1.0 shipped unbuildable: the font every glyph
# comes from was gitignored, so `git clone && tools/build-psp.sh` failed at the
# first compile. Nobody noticed because the file was sitting in the tree it had
# been written in, and "it compiles here" is not the claim that matters.
#
# The last gate rather than the first, because it is by far the slowest.
echo "==> building from a fresh clone, because \"it compiles here\" is not the claim"
CLONE="$(mktemp -d)"
trap 'rm -rf "$CLONE"' EXIT
git clone --quiet --depth 1 "file://$PWD" "$CLONE/pspssh"
( cd "$CLONE/pspssh" && tools/build-psp.sh >/dev/null ) || fail "a fresh clone does not build"

PREVIOUS="$(latest_tag)"
echo
echo "==> $TAG${TITLE:+ — $TITLE}"
if [ -n "$PREVIOUS" ]; then
    echo "    changes since $PREVIOUS:"
    git log --format='      %s' "$PREVIOUS..HEAD"
fi
echo

git tag -a "$TAG" -m "pspssh $VERSION${TITLE:+ — $TITLE}"
git push origin "$TAG"

NOTES="$(mktemp)"
trap 'rm -f "$NOTES"' EXIT
if [ -n "$PREVIOUS" ]; then
    git log --format='- %s' "$PREVIOUS..HEAD" > "$NOTES"
else
    echo "- the first release" > "$NOTES"
fi

gh release create "$TAG" \
    --title "pspssh $VERSION${TITLE:+ — $TITLE}" \
    --notes-file "$NOTES"

echo
echo "==> released $TAG"
