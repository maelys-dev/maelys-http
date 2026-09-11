#!/bin/sh
# Answers, before a tag exists, whether this machine can sign one the release
# will accept. A tag the release refuses is a tag that is burned: the rule
# forbids moving a published tag, and no replay ever passes a signature the
# list does not name, so vX.Y.Z is consumed for nothing.
#
# The list is read from the default branch, exactly as the release reads it,
# and never from the working tree: a key added locally proves nothing.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

remote=${MAELYS_REMOTE:-origin}
branch=$(git symbolic-ref --quiet --short "refs/remotes/$remote/HEAD" 2>/dev/null || true)
branch=${branch#"$remote/"}
branch=${branch:-main}
git fetch --quiet "$remote" "$branch"
signers=$(git show "$remote/$branch:.github/release-allowed-signers")
test -n "$signers" || { echo 'check-signing-key: the allowed-signers list is empty' >&2; exit 1; }

format=$(git config --get gpg.format || echo openpgp)
test "$format" = ssh || {
    echo "check-signing-key: gpg.format is $format; the release accepts SSH signatures only" >&2
    exit 1
}
test "$(git config --get tag.gpgsign || echo false)" = true || {
    echo 'check-signing-key: tag.gpgsign is not true; an unsigned tag is refused' >&2
    exit 1
}

key=$(git config --get user.signingkey || true)
test -n "$key" || { echo 'check-signing-key: user.signingkey is unset' >&2; exit 1; }
case "$key" in
    ssh-*|sk-ssh-*|ecdsa-*) public=$key ;;
    *) test -f "$key.pub" && public=$(cat "$key.pub") || public=$(cat "$key") ;;
esac
# The blob alone identifies the key; the comment after it is free text.
blob=$(printf '%s\n' "$public" | awk '{print $1" "$2}')
test -n "$blob" || { echo 'check-signing-key: could not read a public key' >&2; exit 1; }

email=$(git config --get user.email || true)
test -n "$email" || { echo 'check-signing-key: user.email is unset' >&2; exit 1; }

printf '%s\n' "$signers" | awk -v blob="$blob" -v who="$email" '
    { line = $2" "$3 }
    line == blob && $1 == who { found = 1 }
    END { exit found ? 0 : 1 }
' || {
    echo "check-signing-key: $email with this key is not in .github/release-allowed-signers" >&2
    echo "check-signing-key: on $remote/$branch. Add it there first, in a pull request." >&2
    exit 1
}
echo "check-signing-key: $email may sign a release tag, per $remote/$branch"
