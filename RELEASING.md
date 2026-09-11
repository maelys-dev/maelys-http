# Releasing

Maelys HTTP uses a build-once release flow. The tag identifies source, the
verification matrix produces one canonical source archive, and publication
promotes those same bytes without rebuilding them.

## Repository controls

- Protect `main` and require review for workflow and release-key changes.
- Restrict creation and deletion of `v*` tags to release maintainers.
- Keep maintainer SSH public keys in `.github/release-allowed-signers`; rotate a
  key through a separately reviewed, signed commit before using it on a tag. The
  release reads that file from the default branch, never from the tagged commit:
  read from the tag it would authorise itself, since whoever can push adds a key,
  tags that commit with it and passes.
- Require signed commits on `main`. The release workflow checks GitHub's
  verification result for the exact commit named by the tag.

These server-side rules are part of the trust boundary: a workflow cannot
protect itself from an actor allowed to replace both the release ref and its
workflow definition.

## Release sequence

1. Merge a reviewed version-preparation commit with `VERSION`, the public
   version macros and `CHANGELOG.md` aligned.
2. Run `scripts/check-signing-key.sh`. A tag the release refuses is a tag that
   is burned: this rule forbids moving a published tag, and no replay ever
   passes a signature the allowlist does not name, so the version is consumed
   for nothing. The script answers before the tag exists.
3. Create an annotated SSH-signed `v<version>` tag at that exact commit and
   push it without moving or recreating an existing release tag.
4. The `authenticity` job verifies the tag against the allowlist of the
   default branch, refuses a signature that is not SSH, since
   `gpg.ssh.allowedSignersFile` governs no other kind, checks that the tag's
   commit is GitHub-verified and an ancestor of the default branch, and rejects
   lightweight, mismatched or off-branch tags.
5. The verification matrix runs the source, install and physical TLS suites on
   x86_64, arm64 and macOS. The x86_64 job creates the deterministic archives,
   their checksums, the Homebrew formula and an SPDX 2.3 SBOM exactly once.
6. The publish job downloads that workflow artifact, rechecks its SHA-256
   digests, creates signed GitHub build-provenance and SBOM attestations, and
   saves their Sigstore bundles beside the archives for offline verification.
7. Only those promoted files are attached to the GitHub release; the publish
   job contains no package-build command.

Never bypass a failed authenticity, pin, Mbed TLS security-floor or checksum
check. A distribution build may define
`MAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES` only after verifying that
its maintained package carries every applicable upstream security fix.
