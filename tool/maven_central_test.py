#!/usr/bin/env python3
"""Check Central bundle integrity and explicit publication with disposable data."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "maven_central", Path(__file__).with_name("maven_central.py"))
central = importlib.util.module_from_spec(spec)
spec.loader.exec_module(central)
VERSION = "1.0.0-beta.3"


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as target:
        for name, data in entries.items():
            target.writestr(name, data)
    return output.getvalue()


class CentralTest(unittest.TestCase):
    def setUp(self):
        # Short paths also fit macOS's Unix socket limit for gpg-agent.
        self.temporary = tempfile.TemporaryDirectory(prefix="lychee-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "bundle.zip"
        self.environment = mock.patch.dict(os.environ, {"PATH": os.environ["PATH"]}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        for artifact, name in central.filenames(VERSION):
            if name.endswith(".pom"):
                dependency = (f"<dependencies><dependency><groupId>tech.soit.lychee</groupId>"
                              f"<artifactId>lychee</artifactId><version>{VERSION}</version>"
                              "</dependency></dependencies>" if artifact == "lychee-ffmpeg" else "")
                data = (f'<project xmlns="http://maven.apache.org/POM/4.0.0">'
                        f"<groupId>tech.soit.lychee</groupId><artifactId>{artifact}</artifactId>"
                        f"<version>{VERSION}</version><packaging>aar</packaging>"
                        "<name>Lychee</name><description>Media player</description>"
                        "<url>https://github.com/boyan01/lychee_player</url>"
                        "<licenses><license><name>MIT</name><url>https://opensource.org/license/mit</url>"
                        "</license></licenses><developers><developer><id>test</id><name>Test</name>"
                        "</developer></developers><scm><connection>scm:git:https://example.org/repo.git"
                        f"</connection><url>https://example.org/repo</url></scm>{dependency}</project>").encode()
            elif name.endswith(".aar"):
                library = "liblychee.so" if artifact == "lychee" else "liblychee_ffmpeg.so"
                data = archive({f"jni/{abi}/{library}": b"fixture" for abi in
                                ("armeabi-v7a", "arm64-v8a", "x86_64")})
            elif name.endswith("-sources.jar"):
                data = archive({"Lychee.kt": b"// fixture", "native/include/lychee/lychee.h": b"// fixture"})
            else:
                data = archive({"index.html": b"<html>API documentation</html>"})
            (self.directory / name).write_bytes(data)

    def bundle(self):
        with contextlib.redirect_stdout(io.StringIO()):
            central.bundle(self.directory, VERSION, self.output, unsigned=True)

    def rewrite(self, name, data, duplicate=False):
        with zipfile.ZipFile(self.output) as source:
            entries = {entry.filename: source.read(entry) for entry in source.infolist()}
        if not duplicate:
            entries[name] = data
        self.output.write_bytes(archive(entries))
        if duplicate:
            with warnings.catch_warnings(), zipfile.ZipFile(self.output, "a") as target:
                warnings.simplefilter("ignore", UserWarning)
                target.writestr(name, data)

    def test_complete_unsigned_bundle(self):
        self.bundle()
        with contextlib.redirect_stdout(io.StringIO()):
            central.check_bundle(self.output, VERSION, unsigned=True)
        with zipfile.ZipFile(self.output) as result:
            self.assertEqual(len(result.namelist()), 40)
            self.assertTrue(all(name.startswith("tech/soit/lychee/") for name in result.namelist()))

    def test_corrupted_artifact_is_rejected(self):
        self.bundle()
        name = f"tech/soit/lychee/lychee/{VERSION}/lychee-{VERSION}.aar"
        self.rewrite(name, b"changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            central.check_bundle(self.output, VERSION, unsigned=True)

    def test_unexpected_or_duplicate_file_is_rejected(self):
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate):
                self.bundle()
                name = (f"tech/soit/lychee/lychee/{VERSION}/lychee-{VERSION}.aar"
                        if duplicate else "unexpected.txt")
                with zipfile.ZipFile(self.output) as source:
                    data = source.read(name) if duplicate else b"unexpected"
                self.rewrite(name, data, duplicate)
                with self.assertRaisesRegex(ValueError, "unexpected, missing or duplicate"):
                    central.check_bundle(self.output, VERSION, unsigned=True)

    def test_incomplete_native_publication_is_rejected_before_signing(self):
        path = self.directory / f"lychee-{VERSION}.aar"
        path.write_bytes(archive({"jni/arm64-v8a/liblychee.so": b"fixture"}))
        with self.assertRaisesRegex(ValueError, "missing armeabi-v7a"):
            central.bundle(self.directory, VERSION, self.output)

    def test_mismatched_extension_version_is_rejected(self):
        path = self.directory / f"lychee-ffmpeg-{VERSION}.pom"
        path.write_text(path.read_text().replace(
            f"<artifactId>lychee</artifactId><version>{VERSION}</version>",
            "<artifactId>lychee</artifactId><version>0.1.0</version>"))
        with self.assertRaisesRegex(ValueError, "matching base dependency"):
            self.bundle()

    def test_snapshot_and_path_versions_are_rejected(self):
        for version in ("1.0.0-SNAPSHOT", "../1.0.0", "1.0.0/a", "v1.0.0"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                list(central.filenames(version))

    def test_unsigned_bundle_cannot_reach_upload(self):
        self.bundle()
        with mock.patch.object(central, "request") as request:
            with self.assertRaises(KeyError):
                central.upload(self.output, VERSION, publish=False)
            request.assert_not_called()

    def test_automatic_publication_requires_explicit_opt_in(self):
        self.output.write_bytes(b"fixture")
        os.environ.update(CENTRAL_USERNAME="fixture", CENTRAL_PASSWORD="fixture")
        deployment = "00000000-0000-4000-8000-000000000001"
        for publish, expected in ((False, "USER_MANAGED"), (True, "AUTOMATIC")):
            with self.subTest(publish=publish), mock.patch.object(central, "check_bundle"), \
                    mock.patch.object(central, "request", side_effect=[deployment.encode(), json.dumps(
                        {"deploymentState": "PUBLISHED" if publish else "VALIDATED"}).encode()]) as request, \
                    contextlib.redirect_stdout(io.StringIO()):
                central.upload(self.output, VERSION, publish)
                self.assertIn("publishingType=" + expected, request.call_args_list[0].args[0])

    @unittest.skipUnless(shutil.which("gpg"), "GnuPG is required for signature verification")
    def test_signatures_are_verified_and_tampering_is_rejected(self):
        # Use a disposable key, never a developer's configured signing identity.
        home = self.directory / "gnupg"
        home.mkdir(mode=0o700)
        self.addCleanup(central.stop_agent, self.directory)
        central.run_gpg(home, "--pinentry-mode", "loopback", "--passphrase", "",
                        "--quick-generate-key", "Lychee disposable signing test", "ed25519", "sign", "1d")
        os.environ["MAVEN_SIGNING_KEY"] = central.run_gpg(home, "--armor", "--export-secret-keys").decode()
        with contextlib.redirect_stdout(io.StringIO()):
            central.bundle(self.directory, VERSION, self.output)
            central.check_bundle(self.output, VERSION)
        name = f"tech/soit/lychee/lychee/{VERSION}/lychee-{VERSION}.pom.asc"
        self.rewrite(name, b"invalid signature")
        with self.assertRaisesRegex(ValueError, "GPG operation failed"):
            central.check_bundle(self.output, VERSION)


if __name__ == "__main__":
    unittest.main()
