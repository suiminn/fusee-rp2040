#!/usr/bin/env python3
"""Download the standard hekate payload asset from GitHub releases."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


HEKATE_RELEASES_API = "https://api.github.com/repos/CTCaer/hekate/releases"


class HekateDownloadError(RuntimeError):
    """Raised when the hekate payload cannot be resolved or downloaded."""


def _request(url: str, token: str = "", accept: str = "application/vnd.github+json") -> Request:
    headers = {
        "Accept": accept,
        "User-Agent": "fusee-rp2040-build",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return Request(url, headers=headers)


def fetch_json(url: str, token: str = "") -> dict[str, Any]:
    try:
        with urlopen(_request(url, token), timeout=30) as response:
            return json.load(response)
    except HTTPError as error:
        raise HekateDownloadError(f"GitHub API request failed ({error.code}): {url}") from error
    except URLError as error:
        raise HekateDownloadError(f"GitHub API request failed: {error.reason}") from error
    except json.JSONDecodeError as error:
        raise HekateDownloadError(f"GitHub API returned invalid JSON: {url}") from error


def resolve_release(api_url: str, tag: str, token: str = "") -> dict[str, Any]:
    if tag:
        return fetch_json(f"{api_url}/tags/{quote(tag, safe='')}", token)
    return fetch_json(f"{api_url}/latest", token)


def find_standard_payload_asset(release: dict[str, Any]) -> dict[str, Any]:
    tag = release.get("tag_name")
    if not isinstance(tag, str) or not tag:
        raise HekateDownloadError("hekate release does not include a tag_name")

    version = tag.removeprefix("v")
    expected_name = f"hekate_ctcaer_{version}.bin"
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise HekateDownloadError(f"hekate release {tag} does not include assets")

    for asset in assets:
        if isinstance(asset, dict) and asset.get("name") == expected_name:
            download_url = asset.get("browser_download_url")
            if not isinstance(download_url, str) or not download_url:
                raise HekateDownloadError(f"asset {expected_name} does not include a download URL")
            return asset

    asset_names = ", ".join(str(asset.get("name")) for asset in assets if isinstance(asset, dict))
    raise HekateDownloadError(
        f"hekate release {tag} does not include {expected_name}"
        + (f"; available assets: {asset_names}" if asset_names else "")
    )


def same_file_contents(left: Path, right: Path) -> bool:
    if not right.exists() or left.stat().st_size != right.stat().st_size:
        return False
    return hashlib.sha256(left.read_bytes()).digest() == hashlib.sha256(right.read_bytes()).digest()


def download_file(url: str, output: Path, token: str = "", digest: str = "") -> bool:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        dir=output.parent,
        prefix=f".{output.name}.",
        suffix=".tmp",
        delete=False,
    ) as handle:
        temp_file = Path(handle.name)

    try:
        with urlopen(_request(url, token, "application/octet-stream"), timeout=120) as response:
            with temp_file.open("wb") as handle:
                shutil.copyfileobj(response, handle)
        verify_digest(temp_file, digest)
        if same_file_contents(temp_file, output):
            output.touch()
            return False
        temp_file.replace(output)
        return True
    except HTTPError as error:
        raise HekateDownloadError(f"payload download failed ({error.code}): {url}") from error
    except URLError as error:
        raise HekateDownloadError(f"payload download failed: {error.reason}") from error
    finally:
        temp_file.unlink(missing_ok=True)


def verify_digest(path: Path, digest: str) -> None:
    if not digest:
        return
    if not digest.startswith("sha256:"):
        print(f"warning: unsupported hekate asset digest format: {digest}", file=sys.stderr)
        return

    expected = digest.removeprefix("sha256:")
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual.lower() != expected.lower():
        raise HekateDownloadError(
            f"payload digest mismatch: expected sha256:{expected}, got sha256:{actual}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="path to write the hekate payload")
    parser.add_argument("--tag", default="", help="hekate release tag; omit for the latest release")
    parser.add_argument("--api-url", default=HEKATE_RELEASES_API, help=argparse.SUPPRESS)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""

    try:
        release = resolve_release(args.api_url.rstrip("/"), args.tag, token)
        asset = find_standard_payload_asset(release)
        download_url = str(asset["browser_download_url"])
        digest = str(asset.get("digest") or "")

        updated = download_file(download_url, args.output, token, digest)

        print(
            f"{'Downloaded' if updated else 'Using cached'} {asset['name']} from hekate {release['tag_name']} "
            f"to {args.output.as_posix()}"
        )
        return 0
    except HekateDownloadError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
