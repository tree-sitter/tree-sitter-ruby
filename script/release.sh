#!/usr/bin/env bash
# Prepares a release, the steps that were easy to forget by hand.
# usage: script/release.sh X.Y.Z       bump every manifest and lockfile, regenerate src/ (ABI 15 keeps the version in
#                                       parser.c), date the "## Unreleased" section of CHANGELOG.md, test, commit
#        script/release.sh pin vX.Y.Z  once the tag is pushed: pin the Neovim, Helix and Emacs setups in README.md to it
set -euo pipefail
ts=node_modules/.bin/tree-sitter

if [[ ${1:-} == pin ]]; then
    tag=${2:?usage: script/release.sh pin vX.Y.Z}
    commit=$(git rev-parse "$tag^{commit}")
    sed -i -E -e "s/(info\.revision = ')[0-9a-f]{40}(' -- )v[0-9.]+/\1$commit\2$tag/" \
        -e "s/(rev = \")[0-9a-f]{40}\"/\1$commit\"/" \
        -e "s/(tree-sitter-ruby\" \")v[0-9.]+(\"\)\))/\1$tag\2/" README.md
    git diff --stat README.md
    exit
fi

version=${1:?usage: script/release.sh X.Y.Z | script/release.sh pin vX.Y.Z}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a version: $version" >&2; exit 1; }
[[ -z $(git status --porcelain --untracked-files=no) ]] || { echo "commit your changes first" >&2; exit 1; }
grep -q '^## Unreleased$' CHANGELOG.md || { echo "CHANGELOG.md has no '## Unreleased' section" >&2; exit 1; }
previous=$(node -p 'require("./package.json").version')

"$ts" version "$version"
cargo update --workspace --offline --quiet
npm install --package-lock-only --legacy-peer-deps --ignore-scripts --silent
"$ts" generate --abi 15
sed -i "s/^## Unreleased$/## $version ($(date +%F))/" CHANGELOG.md
"$ts" test >/dev/null

git commit -q -a -F - <<EOF
release: $version

Before: $previous
After:  $version in every manifest, the lockfiles and the parser metadata; changelog dated
EOF
git show --stat --format='%h %s' HEAD
echo "next: PR and merge, then tag v$version and push it, then: script/release.sh pin v$version"
