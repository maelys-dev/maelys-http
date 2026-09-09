#!/bin/sh
set -eu

version="${1:?version required}"
system_version="${2:?maelys-system version required}"
case "$version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid version' >&2; exit 1 ;;
esac
case "$system_version" in
    *[!0-9A-Za-z.-]*|'') echo 'invalid maelys-system version' >&2; exit 1 ;;
esac

archive="dist/maelys-http-$version.tar.gz"
checksum_file="$archive.sha256"
sbom="dist/maelys-http-$version.spdx.json"
test -f "$archive"
test -f "$checksum_file"

archive_sha256=$(sed -n '1s/[[:space:]].*$//p' "$checksum_file")
case "$archive_sha256" in
    *[!0-9a-f]*|'') echo 'invalid archive checksum' >&2; exit 1 ;;
esac
test "${#archive_sha256}" = 64

commit=$(git rev-parse HEAD)
created_epoch=$(git show -s --format=%ct HEAD)
created=$(python3 -c \
    'import datetime,sys; print(datetime.datetime.fromtimestamp(int(sys.argv[1]), datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))' \
    "$created_epoch")
system_pin=$(sed -n '2p' dependencies/maelys-system.pin)
case "$system_pin" in
    *[!0-9a-f]*|'') echo 'invalid maelys-system pin' >&2; exit 1 ;;
esac
test "${#system_pin}" = 40

cat >"$sbom" <<EOF
{
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "name": "maelys-http-$version",
  "documentNamespace": "https://github.com/maelys-dev/maelys-http/releases/tag/v$version/spdx/$commit",
  "creationInfo": {
    "created": "$created",
    "creators": ["Tool: maelys-http/scripts/generate-sbom.sh"]
  },
  "packages": [
    {
      "name": "maelys-http",
      "SPDXID": "SPDXRef-Package-maelys-http",
      "versionInfo": "$version",
      "packageFileName": "maelys-http-$version.tar.gz",
      "downloadLocation": "https://github.com/maelys-dev/maelys-http/releases/download/v$version/maelys-http-$version.tar.gz",
      "filesAnalyzed": false,
      "checksums": [{"algorithm": "SHA256", "checksumValue": "$archive_sha256"}],
      "licenseConcluded": "MPL-2.0",
      "licenseDeclared": "MPL-2.0",
      "copyrightText": "NOASSERTION",
      "primaryPackagePurpose": "LIBRARY",
      "externalRefs": [{
        "referenceCategory": "PACKAGE-MANAGER",
        "referenceType": "purl",
        "referenceLocator": "pkg:generic/maelys-http@$version"
      }]
    },
    {
      "name": "maelys-system",
      "SPDXID": "SPDXRef-Package-maelys-system",
      "versionInfo": "$system_version",
      "downloadLocation": "https://github.com/maelys-dev/maelys-system/archive/$system_pin.tar.gz",
      "filesAnalyzed": false,
      "licenseConcluded": "MPL-2.0",
      "licenseDeclared": "MPL-2.0",
      "copyrightText": "NOASSERTION",
      "primaryPackagePurpose": "LIBRARY"
    },
    {
      "name": "Mbed TLS",
      "SPDXID": "SPDXRef-Package-mbedtls",
      "versionInfo": "NOASSERTION",
      "comment": "Optional, resolved by the consumer. The build and the client constructor refuse anything below 3.6.7 in the 3.x line or 4.1.2 in the 4.x line.",
      "downloadLocation": "https://github.com/Mbed-TLS/mbedtls",
      "filesAnalyzed": false,
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "primaryPackagePurpose": "LIBRARY"
    }
  ],
  "relationships": [
    {
      "spdxElementId": "SPDXRef-DOCUMENT",
      "relationshipType": "DESCRIBES",
      "relatedSpdxElement": "SPDXRef-Package-maelys-http"
    },
    {
      "spdxElementId": "SPDXRef-Package-maelys-http",
      "relationshipType": "DEPENDS_ON",
      "relatedSpdxElement": "SPDXRef-Package-maelys-system"
    },
    {
      "spdxElementId": "SPDXRef-Package-mbedtls",
      "relationshipType": "OPTIONAL_DEPENDENCY_OF",
      "relatedSpdxElement": "SPDXRef-Package-maelys-http"
    }
  ]
}
EOF

python3 -m json.tool "$sbom" >/dev/null
grep -Fq "\"checksumValue\": \"$archive_sha256\"" "$sbom"
grep -Fq "\"versionInfo\": \"$system_version\"" "$sbom"
grep -Fq "$system_pin" "$sbom"
echo "$sbom"
