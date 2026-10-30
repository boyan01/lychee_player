# Releasing

Each registry a release publishes to has a version badge in the
README's Packages table; a new registry (crates.io, ...) adds one.

A release is one run of `.github/workflows/release.yml`, after a
`## VERSION` section in `flutter/CHANGELOG.md`:

```sh
gh workflow run release.yml -f version=1.0.0
```

It builds the Apple xcframeworks, both Android AARs and the Windows and
Linux FFmpeg (on Ubuntu; Windows cross-compiled), pins them in
`flutter/prebuilt.json` and `Package.swift` (which lets SwiftPM take
`sdk/apple` by URL), attests
the assets' build provenance and an SPDX SBOM of them, drafts the GitHub
release with the assets, the SBOM and the sources of FFmpeg and dav1d,
and pushes the commit `Release VERSION` and the tag
`vVERSION`. The tag starts `publish.yml`, which publishes the release and
the package on pub.dev, then both Android AARs on Maven Central.
It runs no regression and builds no fresh app: CI has
checked the commit, and `tool/release.sh check` is for changes to
packaging (AGENTS.md). Each step is a `tool/release.sh` command and runs
locally too:

```sh
tool/release.sh version 1.0.0
tool/release.sh build apple /tmp/sources && tool/release.sh pack apple /tmp/dist
tool/release.sh build android /tmp/sources && tool/release.sh pack android /tmp/dist
tool/release.sh build windows /tmp/sources && tool/release.sh pack windows /tmp/dist  # on Linux
tool/release.sh build linux /tmp/sources && tool/release.sh pack linux /tmp/dist  # on Ubuntu 22.04
tool/release.sh sources /tmp/dist && tool/release.sh pin /tmp/dist
tool/release.sh check /tmp/dist   # then git checkout the pins; in Git Bash on Windows it builds a Windows app, on Linux a Linux app
tool/release.sh sbom /tmp/dist    # lychee_player-VERSION.spdx.json
```

Anyone can check an asset against the workflow that built it:
`gh attestation verify ASSET --repo boyan01/lychee_player` (provenance;
`--predicate-type https://spdx.dev/Document/v2.3` for the SBOM).

Once per repository: a deploy key with write access whose private key is
the secret `RELEASE_DEPLOY_KEY` (the workflow's own token cannot start
`publish.yml`), a GitHub environment `pub.dev`, and automated publishing
on the package's pub.dev Admin page (repository `boyan01/lychee_player`,
tag pattern `v{{version}}`, environment `pub.dev`).

The Android publications are automatically sent to Maven Central after
GitHub and pub.dev publication succeeds. Both use the
`io.github.boyan01.lychee` group under the GitHub-verified
`io.github.boyan01` namespace. No custom domain or DNS verification is
needed. Maven coordinates do not change Kotlin package names: imports
remain `tech.soit.lychee.*`.

The Gradle `publish` tasks write only to the local repository. Each
publication includes its AAR, POM, Kotlin/native sources and generated
Dokka API documentation. `tool/release.sh pack android` includes all
eight files, and `pin` records their SHA-256 hashes. `Publish` calls the
reusable `Maven Central` workflow, which downloads those release assets, checks
their hashes, signs them and submits both AARs in one deployment.
Normal releases need no second dispatch.
Create a new release containing these publications first; older tags
whose assets use the previous coordinates or omit sources/documentation
cannot be used by this workflow.

Configure four repository secrets before running that workflow:

| Secret | Value |
| --- | --- |
| `CENTRAL_USERNAME` | The username from a Central Portal user token |
| `CENTRAL_PASSWORD` | The password from the same token, not the account password |
| `MAVEN_SIGNING_KEY` | ASCII-armored private signing key, including its header/footer |
| `MAVEN_SIGNING_PASSWORD` | The key's passphrase |

Keep a protected backup of the signing key and passphrase outside the
checkout. Publish only its public key to a
[Central-supported keyserver](https://central.sonatype.org/publish/requirements/gpg/#distributing-your-public-key).
The token needs publishing access to the verified namespace. Neither
credential belongs in Gradle properties committed to Git.

For a local check, set `MAVEN_SIGNING_KEY` and `MAVEN_SIGNING_PASSWORD`
in the process environment, then run:

```sh
python3 -I tool/maven_central_test.py
python3 -I tool/maven_central.py bundle /tmp/dist --version VERSION --output /tmp/central-bundle.zip
python3 -I tool/maven_central.py check /tmp/central-bundle.zip --version VERSION
```

Verification can use `MAVEN_PUBLIC_KEY` instead of the private key.
`--unsigned` on `bundle` and `check` is for local packaging tests only;
unsigned bundles cannot be uploaded. The helper checks both artifacts,
matching versions, all three Android ABIs, POM metadata, sources,
documentation, checksums and signatures.

Manual dispatch of `Maven Central` remains available to stage a deployment
or recover a failed Central publication independently. Its `publish` input
defaults to `false`: Central validates and stages it for inspection.
The normal release workflow explicitly sets it to `true` for automatic
publication. For a separately authorized
local upload, `tool/maven_central.py upload /tmp/central-bundle.zip
--version VERSION` has the same staging default; `--publish` opts into
automatic publication. A released version is immutable on Central.
