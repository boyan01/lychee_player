#!/usr/bin/env python3
"""Validate, sign and upload the two Android SDK publications as one bundle."""

import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile


GROUP_ID = "io.github.boyan01.lychee"
GROUP_PATH = GROUP_ID.replace(".", "/")
ARTIFACTS = ("lychee", "lychee-ffmpeg")
BASE_URL = "https://central.sonatype.com/api/v1/publisher"
NAMESPACE = {"m": "http://maven.apache.org/POM/4.0.0"}


def filenames(version):
    if version.endswith("-SNAPSHOT") or not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("Expected a release version such as 1.0.0-beta.3")
    for artifact in ARTIFACTS:
        for suffix in (".aar", ".pom", "-sources.jar", "-javadoc.jar"):
            yield artifact, f"{artifact}-{version}{suffix}"


def validate_publication(files, version):
    """Check the contract before consuming a signing key or opening the network."""
    for artifact, name in filenames(version):
        data = files[name]
        if name.endswith(".pom"):
            root = ET.fromstring(data)
            value = lambda path: root.findtext("m:" + path.replace("/", "/m:"), namespaces=NAMESPACE)
            for field, expected in (("groupId", GROUP_ID),
                                    ("artifactId", artifact), ("version", version),
                                    ("packaging", "aar")):
                if value(field) != expected:
                    raise ValueError(f"{name}: incorrect {field}")
            for field in ("name", "description", "url", "licenses/license/name",
                          "licenses/license/url", "developers/developer/id",
                          "developers/developer/name", "scm/connection", "scm/url"):
                if not value(field):
                    raise ValueError(f"{name}: missing {field}")
            if artifact == "lychee-ffmpeg":
                dependencies = root.findall("m:dependencies/m:dependency", NAMESPACE)
                if not any(d.findtext("m:groupId", namespaces=NAMESPACE) == GROUP_ID
                           and d.findtext("m:artifactId", namespaces=NAMESPACE) == "lychee"
                           and d.findtext("m:version", namespaces=NAMESPACE) == version
                           for d in dependencies):
                    raise ValueError(f"{name}: missing matching base dependency")
        else:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                entries = archive.namelist()
                if name.endswith(".aar"):
                    for abi in ("armeabi-v7a", "arm64-v8a", "x86_64"):
                        library = "liblychee.so" if artifact == "lychee" else "liblychee_ffmpeg.so"
                        if f"jni/{abi}/{library}" not in entries:
                            raise ValueError(f"{name}: missing {abi} native library")
                elif name.endswith("-sources.jar"):
                    if not any(n.endswith(".kt") for n in entries) or "native/include/lychee/lychee.h" not in entries:
                        raise ValueError(f"{name}: missing Kotlin or C sources")
                elif "index.html" not in entries:
                    raise ValueError(f"{name}: missing generated API documentation")


def stop_agent(directory):
    subprocess.run(["gpgconf", "--homedir", str(directory / "gnupg"), "--kill", "gpg-agent"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def run_gpg(home, *arguments, data=None):
    result = subprocess.run(["gpg", "--homedir", str(home), "--batch", *arguments],
                            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # GPG output may contain a key identity. Keep credential setup out of logs.
        raise ValueError("GPG operation failed; check the signing key and its passphrase")
    return result.stdout


def keyring(home):
    key = os.environ.get("MAVEN_SIGNING_KEY")
    public_key = os.environ.get("MAVEN_PUBLIC_KEY")
    if not key and not public_key:
        raise ValueError("Set MAVEN_SIGNING_KEY or MAVEN_PUBLIC_KEY to verify signatures")
    run_gpg(home, "--import", data=(key or public_key).encode())


def sign_files(files, directory):
    if not os.environ.get("MAVEN_SIGNING_KEY"):
        raise ValueError("MAVEN_SIGNING_KEY is required to create a signed bundle")
    home = directory / "gnupg"
    home.mkdir(mode=0o700)
    keyring(home)
    listing = run_gpg(home, "--with-colons", "--list-secret-keys").decode()
    fingerprint = next((line.split(":")[9] for line in listing.splitlines()
                        if line.startswith("fpr:")), None)
    if not fingerprint:
        raise ValueError("The signing key contains no secret key")
    for name, data in list(files.items()):
        path = directory / name
        path.write_bytes(data)
        signature = run_gpg(home, "--pinentry-mode", "loopback", "--passphrase-fd", "0",
                            "--local-user", fingerprint, "--armor", "--detach-sign",
                            "--output", "-", str(path),
                            data=(os.environ.get("MAVEN_SIGNING_PASSWORD", "") + "\n").encode())
        files[name + ".asc"] = signature
        signature_path = directory / (name + ".asc")
        signature_path.write_bytes(signature)
        run_gpg(home, "--verify", str(signature_path), str(path))


def bundle(source, version, output, unsigned=False):
    files = {name: (source / name).read_bytes() for _, name in filenames(version)}
    validate_publication(files, version)
    if not unsigned:
        with tempfile.TemporaryDirectory(prefix="lychee-sign-") as temporary:
            try:
                sign_files(files, Path(temporary))
            finally:
                stop_agent(Path(temporary))
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for artifact, name in filenames(version):
            target = f"{GROUP_PATH}/{artifact}/{version}/{name}"
            data = files[name]
            archive.writestr(target, data)
            for algorithm in ("md5", "sha1", "sha256", "sha512"):
                digest = hashlib.new(algorithm, data, usedforsecurity=False).hexdigest()
                archive.writestr(target + "." + algorithm, digest)
            if not unsigned:
                archive.writestr(target + ".asc", files[name + ".asc"])
    print(f"Created {'unsigned test' if unsigned else 'signed'} bundle: {output}")


def check_bundle(path, version, unsigned=False):
    files = {}
    signatures = {}
    with zipfile.ZipFile(path) as archive:
        expected = set()
        for artifact, name in filenames(version):
            target = f"{GROUP_PATH}/{artifact}/{version}/{name}"
            data = archive.read(target)
            files[name] = data
            expected.add(target)
            for algorithm in ("md5", "sha1", "sha256", "sha512"):
                expected.add(target + "." + algorithm)
                expected_digest = hashlib.new(algorithm, data, usedforsecurity=False).hexdigest()
                if archive.read(target + "." + algorithm).decode().strip() != expected_digest:
                    raise ValueError(f"Invalid checksum: {name}")
            if not unsigned:
                expected.add(target + ".asc")
                signatures[name] = archive.read(target + ".asc")
        if len(archive.namelist()) != len(expected) or set(archive.namelist()) != expected:
            raise ValueError("Bundle contains unexpected, missing or duplicate files")
    validate_publication(files, version)
    if not unsigned:
        with tempfile.TemporaryDirectory(prefix="lychee-verify-") as temporary:
            directory = Path(temporary)
            home = directory / "gnupg"
            home.mkdir(mode=0o700)
            try:
                keyring(home)
                for name, data in files.items():
                    artifact = directory / name
                    signature = directory / (name + ".asc")
                    artifact.write_bytes(data)
                    signature.write_bytes(signatures[name])
                    run_gpg(home, "--verify", str(signature), str(artifact))
            finally:
                stop_agent(directory)
    print("Validated both publications, documentation, sources, checksums" +
          (" (unsigned test only)" if unsigned else " and signatures"))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, url):
        # Publishing credentials belong only to the fixed Central API host.
        return None


def request(endpoint, token, data, content_type):
    request = urllib.request.Request(BASE_URL + endpoint, data=data, method="POST",
                                     headers={"Authorization": "Bearer " + token,
                                              "Content-Type": content_type})
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=120) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise ValueError(f"Central returned HTTP {error.code}; inspect the deployment in the Portal") from None


def upload(path, version, publish):
    check_bundle(path, version)
    username = os.environ.get("CENTRAL_USERNAME")
    password = os.environ.get("CENTRAL_PASSWORD")
    if not username or not password:
        raise ValueError("CENTRAL_USERNAME and CENTRAL_PASSWORD are required")
    token = base64.b64encode(f"{username}:{password}".encode()).decode()
    boundary = "lychee-" + uuid.uuid4().hex
    data = (f'--{boundary}\r\nContent-Disposition: form-data; name="bundle"; '
            f'filename="central-bundle.zip"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
    data += path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    query = urllib.parse.urlencode({"name": f"lychee-{version}",
                                    "publishingType": "AUTOMATIC" if publish else "USER_MANAGED"})
    deployment = request("/upload?" + query, token, data, "multipart/form-data; boundary=" + boundary).decode().strip()
    if not re.fullmatch(r"[0-9a-fA-F-]{36}", deployment):
        raise ValueError("Upload response was unexpected; inspect the Portal before retrying")
    print("Central deployment: " + deployment)
    for _ in range(120):
        status = json.loads(request("/status?id=" + deployment, token, b"", "application/json"))
        state = status.get("deploymentState")
        if state in ("PUBLISHED", "VALIDATED"):
            if publish and state != "PUBLISHED":
                time.sleep(5)
                continue
            print("Central state: " + state)
            return
        if state == "FAILED":
            raise ValueError("Central validation failed: " + json.dumps(status.get("errors", {})))
        time.sleep(5)
    raise ValueError("Central is still processing; inspect deployment " + deployment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("bundle", "check", "upload"))
    parser.add_argument("path", type=Path, help="flat release directory for bundle; ZIP for check/upload")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--unsigned", action="store_true", help="local validation only; cannot be uploaded")
    parser.add_argument("--publish", action="store_true", help="make an upload publish automatically after validation")
    args = parser.parse_args()
    try:
        list(filenames(args.version))
        if args.command == "bundle":
            if not args.output:
                parser.error("bundle requires --output")
            bundle(args.path, args.version, args.output, args.unsigned)
        elif args.command == "check":
            check_bundle(args.path, args.version, args.unsigned)
        else:
            if args.unsigned:
                parser.error("unsigned bundles cannot be uploaded")
            upload(args.path, args.version, args.publish)
    except (ValueError, OSError, KeyError, ET.ParseError, zipfile.BadZipFile) as error:
        parser.exit(1, f"maven_central: {error}\n")


if __name__ == "__main__":
    main()
