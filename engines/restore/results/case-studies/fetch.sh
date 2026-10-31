#!/bin/sh
# Downloads the released altsql-restore (releases/latest, or the tag in
# $ALTSQL_TAG) and the public sample databases the case studies use, and
# checks every download against a SHA-256.
#
#   sh fetch.sh            # into ./work (ignored by the case-study scripts' git adds)
#
# Needs: curl, tar, sha256sum. Writes work/bin/altsql-restore, work/bin/altsql-db,
# work/data/*, and work/versions.txt.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work="$here/work"
mkdir -p "$work/dl" "$work/bin" "$work/data"
cd "$work/dl"

arch=$(uname -m); os=$(uname -s | tr A-Z a-z)
case "$arch" in x86_64|amd64) arch=amd64 ;; aarch64|arm64) arch=arm64 ;; esac
api=https://api.github.com/repos/AltSql/altsql/releases
if [ -n "${ALTSQL_TAG:-}" ]; then rel="$api/tags/$ALTSQL_TAG"; else rel="$api/latest"; fi
tag=$(curl -fsSL "$rel" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
[ -n "$tag" ] || { echo "could not read the release tag" >&2; exit 2; }
base="https://github.com/AltSql/altsql/releases/download/$tag"
pkg="altsql_${os}_${arch}.tar.gz"
curl -fsSL -o SHA256SUMS "$base/SHA256SUMS"
curl -fsSL -o "$pkg" "$base/$pkg"
grep " $pkg\$" SHA256SUMS | sha256sum -c -
rm -rf pkg && mkdir pkg && tar -xzf "$pkg" -C pkg
for p in altsql-restore altsql-db; do
  f=$(find pkg -type f -name "$p" | head -1)
  [ -n "$f" ] || { echo "$p missing from $pkg" >&2; exit 2; }
  cp "$f" "$work/bin/$p"; chmod +x "$work/bin/$p"
done

# Public sample databases, pinned to fixed commits so the bytes never change.
CHINOOK=https://raw.githubusercontent.com/lerocha/chinook-database/master/ChinookDatabase/DataSources
PAGILA=https://raw.githubusercontent.com/devrimgunduz/pagila/master
get() { # url file sha256
  [ -f "$work/data/$2" ] || curl -fsSL -o "$work/data/$2" "$1"
  echo "$3  $work/data/$2" | sha256sum -c -
}
. "$here/data.sha256"

{
  echo "release:        $tag ($pkg)"
  echo "release SHA-256: $(sha256sum "$pkg" | cut -d' ' -f1)"
  echo "altsql-restore SHA-256: $(sha256sum "$work/bin/altsql-restore" | cut -d' ' -f1)"
  echo "fetched:        $(date -u '+%Y-%m-%d %H:%M UTC')"
  (cd "$work/data" && sha256sum *)
} > "$work/versions.txt"
cat "$work/versions.txt"
