# Releasing

No release is published from the initial H1/H2 implementation branch.

The intended release flow follows the Maelys build-once discipline:

1. merge a versioned release-preparation commit;
2. build, run sanitizers/fuzz seeds and package on each target;
3. record SHA-256 digests of the tested archives;
4. create and verify a signed Git tag at that exact commit;
5. promote the already-tested bytes; never rebuild under the tag;
6. publish checksums, SBOM/provenance and signatures together.

Release tags and commits must be signed. A release workflow will be enabled only
after the public API review and repository protections are in place.
