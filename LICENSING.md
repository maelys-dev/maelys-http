# Licensing

Copyright 2026 David Bromberg.

The socle wrote this file once because it was missing. It belongs to this
repository now: state below what each part is licensed under, and name every
document this repository engages publicly. `bin/maelys-platform docs` reads
this file to tell a public engagement from prose that migrates elsewhere.

## Source code: MPL-2.0

The source code of maelys-http is available under the Mozilla Public License
2.0. The complete terms are in [`LICENSE`](LICENSE).

The MPL applies file by file. A program that links this code, statically or
otherwise, keeps its own license (section 3.3 of the MPL); only a modified
covered file must remain available in Source Code Form under MPL-2.0.

## Installed agent texts: CC-BY-4.0

The managed `maelys-release` blocks of `AGENTS.md` and `CLAUDE.md`, and
`.claude/skills/maelys-release/SKILL.md`, are installed by
`maelys-release adopt` from the maelys-release distribution. Its
`share/agents/` texts are licensed under CC-BY-4.0 with attribution to
David Bromberg since maelys-release v0.28.0. Blocks installed before that
version were copied under CC0-1.0 and that grant stands for those copies;
the notices identifying copyright, source and license arrive with the next
adoption. Retain them, and indicate your changes when sharing an
adaptation. This applies to the installed blocks, not to what this
repository writes outside them.

## Redistributed material

This repository redistributes none. The release publishes one source archive
of this tree and nothing else: Maelys System is pinned and built by the
consumer, and Mbed TLS is resolved by the consumer through `pkg-config`, so
neither reaches an artifact published here. The Homebrew formula names this
repository's archive and declares `libmaelys-sys` and `mbedtls` as
dependencies Homebrew fetches itself.

## Documents engaged publicly

- [`docs/provenance.md`](docs/provenance.md), named by [`NOTICE`](NOTICE) as
  where the exact repositories, commits and extracted ideas behind the
  retained MIT notices are recorded. `NOTICE` ships inside the released source
  archive and its attribution chain has to be followable by whoever holds that
  archive, so this document stays here. It is engaged by `NOTICE` rather than
  by `LICENSING.md` itself, and is named here because this is the file the
  classification reads.

Everything else under `docs/` that is prose belongs in
`maelys-docs/maelys-http/`.
