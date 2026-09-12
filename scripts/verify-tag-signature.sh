#!/bin/sh
# Verifies that the tag naming this checkout was signed by a key this
# repository allows, and that the commit it names landed on the default
# branch. The socle's release workflow proves the tag is annotated, verified
# by GitHub and equal to VERSION; none of that says *who* may publish, since
# GitHub reports any key any account registered. This is the allowlist.
#
# usage: scripts/verify-tag-signature.sh [TAG]
# TAG defaults to v$(cat VERSION), which the socle has already proven equal
# to the tag being released, so a workflow_dispatch replay resolves it the
# same way a tag push does.
#
# The allowed-signers list is read from the default branch, never from the
# tagged commit. Read from the tag, it would authorise itself: whoever can
# push adds a key, tags that commit with it, and the check passes. On the
# default branch a key is added only through a reviewed, checked pull
# request.
#
# The tag's commit is not asked of GitHub here, because that needs an API
# token the release does not hand to this step, and because the question is
# already answered: main requires signed commits and the last check below
# proves the commit is an ancestor of it. A tag on a commit that never
# landed is refused whatever signed it.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

tag=${1:-}
if [ -z "$tag" ]; then
    tag="v$(sed -n '1p' VERSION)"
fi
case "$tag" in
    v*) ;;
    *) echo "verify-tag-signature: TAG must be vX.Y.Z: $tag" >&2; exit 64 ;;
esac

# Off a runner this runs on a working branch as often as on a tag: the socle's
# `cut` replays it on main before writing VERSION, where VERSION still names
# the previous release and its tag points at an older commit. That is a normal
# state, not a refusal, and the two cases below say so. On a runner the release
# checked the tag out itself, so either one is a broken checkout.
not_ours() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "verify-tag-signature: $1" >&2
        exit 65
    fi
    echo "verify-tag-signature: $2; skipped (run scripts/check-signing-key.sh before creating a tag)"
    exit 0
}

if ! git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    not_ours "$tag is not in this checkout; the release cannot verify what it publishes" \
             "no $tag here"
fi

test "$(git cat-file -t "$tag")" = tag || {
    echo "verify-tag-signature: $tag is not an annotated tag" >&2; exit 65
}
if [ "$(git rev-list -n 1 "$tag")" != "$(git rev-parse HEAD)" ]; then
    not_ours "$tag does not name the checked-out commit" \
             "$tag names an earlier commit, so it is a published release and not this checkout"
fi

# gpg.ssh.allowedSignersFile governs SSH signatures only, and git picks its
# backend from the signature header rather than from gpg.format, so an
# OpenPGP tag would bypass the list instead of being measured against it.
# Refuse anything but SSH rather than verify nothing.
git cat-file tag "$tag" | grep -q '^-----BEGIN SSH SIGNATURE-----' || {
    echo "verify-tag-signature: $tag is not SSH-signed; the allowed-signers list governs SSH alone" >&2
    exit 65
}

# awk, not sed: the fields of ls-remote are tab-separated, and a bracket
# expression spelling that tab as \t is read literally by a POSIX sed.
branch=$(git ls-remote --symref origin HEAD |
    awk '/^ref: refs\/heads\// { sub("refs/heads/", "", $2); print $2; exit }')
test -n "$branch" || { echo "verify-tag-signature: cannot read the default branch of origin" >&2; exit 69; }
git fetch --quiet origin "refs/heads/$branch"
default_commit=$(git rev-parse FETCH_HEAD)

signers=$(mktemp)
trap 'rm -f "$signers"' EXIT HUP INT TERM
git show "$default_commit:.github/release-allowed-signers" >"$signers"
test -s "$signers" || {
    echo "verify-tag-signature: .github/release-allowed-signers is empty on $branch" >&2; exit 65
}

git -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$signers" verify-tag "$tag"

git merge-base --is-ancestor "$(git rev-parse HEAD)" "$default_commit" || {
    echo "verify-tag-signature: $tag names a commit that never landed on $branch" >&2; exit 65
}

echo "verify-tag-signature: $tag is signed by an allowed key and names a commit on $branch"
