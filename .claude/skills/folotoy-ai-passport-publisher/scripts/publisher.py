#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import mimetypes
import os
import secrets
import stat
import sys
import time
import urllib.error
import urllib.request
import webbrowser
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


DEFAULT_BASE_URL = "https://ai-passport.folotoy.cn"
CONFIG_PATH = Path(os.getenv("FOLOTOY_PUBLISHER_CONFIG", Path.home() / ".config" / "folotoy" / "ai-passport-publisher.json"))
MAX_FIRMWARE_BYTES = 8 * 1024 * 1024
MAX_COVER_BYTES = 10 * 1024 * 1024


class PublisherError(RuntimeError):
    pass


def base_url() -> str:
    return os.getenv("FOLOTOY_AI_PASSPORT_URL", DEFAULT_BASE_URL).rstrip("/")


def api_request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, token: str | None = None, body: bytes | None = None, content_type: str | None = None) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json", "User-Agent": "folotoy-ai-passport-publisher/1.1"}
    data = body
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif content_type:
        headers["Content-Type"] = content_type
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(f"{base_url()}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            raw = response.read()
            return response.status, json.loads(raw.decode("utf-8")) if raw else {"ok": True}
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            detail = json.loads(raw.decode("utf-8")) if raw else {"detail": f"HTTP {exc.code}"}
        except json.JSONDecodeError:
            detail = {"detail": raw.decode("utf-8", errors="replace") or f"HTTP {exc.code}"}
        return exc.code, detail
    except urllib.error.URLError as exc:
        raise PublisherError(f"Cannot reach {base_url()}: {exc.reason}") from exc


def load_config(required: bool = True) -> dict[str, Any]:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError:
        if required:
            raise PublisherError("Not authorized. Run: publisher.py authorize")
        return {}
    if required and (not data.get("access_token") or data.get("base_url") != base_url()):
        raise PublisherError("Authorization is missing for this server. Run: publisher.py authorize")
    return data


def save_config(payload: dict[str, Any]) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    CONFIG_PATH.chmod(stat.S_IRUSR | stat.S_IWUSR)


def validate_esp_image(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_FIRMWARE_BYTES:
        raise PublisherError("Firmware must be non-empty and no larger than 8 MiB")
    if len(raw) < 33 or raw[0] != 0xE9:
        raise PublisherError("Firmware is not a recognizable merged ESP image")
    segment_count, flash_mode, flash_config = raw[1], raw[2], raw[3]
    if not 1 <= segment_count <= 16 or flash_mode > 3 or flash_config >> 4 > 4 or (flash_config & 0x0F) not in {0, 1, 2, 0x0F}:
        raise PublisherError("ESP image header parameters are invalid")
    offset = 24
    for _ in range(segment_count):
        if offset + 8 > len(raw):
            raise PublisherError("ESP segment header is incomplete")
        size = int.from_bytes(raw[offset + 4:offset + 8], "little")
        if size <= 0 or size > MAX_FIRMWARE_BYTES or offset + 8 + size > len(raw):
            raise PublisherError("ESP segment data is incomplete")
        offset += 8 + size
    if offset >= len(raw):
        raise PublisherError("ESP image checksum data is missing")
    return {"path": str(path.resolve()), "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "segments": segment_count}


def validate_cover(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix not in {".jpg", ".jpeg", ".png", ".webp"}:
        raise PublisherError("Cover must be JPEG, PNG, or WebP")
    size = path.stat().st_size
    if size <= 0 or size > MAX_COVER_BYTES:
        raise PublisherError("Cover must be non-empty and no larger than 10 MiB")
    return {"path": str(path.resolve()), "size": size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def validate_source_repository_url(value: str) -> str:
    repository_url = value.strip()
    try:
        parsed = urlsplit(repository_url)
        hostname = parsed.hostname or ""
        _ = parsed.port
    except ValueError as exc:
        raise PublisherError("Source repository URL is invalid") from exc
    if parsed.scheme != "https" or not hostname or parsed.username or parsed.password:
        raise PublisherError("Source repository must use a public HTTPS URL")
    if parsed.query or parsed.fragment:
        raise PublisherError("Use the repository project page without query parameters or fragments")
    normalized_host = hostname.rstrip(".").lower()
    if normalized_host == "localhost" or normalized_host.endswith((".localhost", ".local")):
        raise PublisherError("Source repository must be publicly reachable")
    try:
        address = ipaddress.ip_address(normalized_host)
    except ValueError:
        address = None
    if address and (address.is_private or address.is_loopback or address.is_link_local or address.is_reserved or address.is_multicast or address.is_unspecified):
        raise PublisherError("Source repository must be publicly reachable")
    path_parts = [part for part in parsed.path.split("/") if part]
    if len(path_parts) < 2 or any(part in {".", ".."} for part in path_parts):
        raise PublisherError("Source repository must identify a complete project path")
    return repository_url.rstrip("/")


def multipart(fields: dict[str, str], files: dict[str, Path]) -> tuple[bytes, str]:
    boundary = f"----FoloToyPublisher{secrets.token_hex(12)}"
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            value.encode("utf-8"), b"\r\n",
        ])
    for name, path in files.items():
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        safe_name = path.name.replace('"', "-")
        chunks.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"; filename="{safe_name}"\r\n'.encode(),
            f"Content-Type: {mime}\r\n\r\n".encode(),
            path.read_bytes(), b"\r\n",
        ])
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def command_authorize(_: argparse.Namespace) -> None:
    status, request = api_request("/api/agent/device-code", method="POST", payload={})
    if status != 201:
        raise PublisherError(request.get("detail", f"Authorization failed: HTTP {status}"))
    print(f"Open this URL to authorize:\n{request['verificationUriComplete']}\n")
    print(f"Code: {request['userCode']}")
    webbrowser.open(request["verificationUriComplete"])
    deadline = time.time() + int(request.get("expiresIn", 600))
    interval = max(2, int(request.get("interval", 3)))
    while time.time() < deadline:
        status, result = api_request("/api/agent/token", method="POST", payload={"device_code": request["deviceCode"]})
        if status == 200:
            save_config({
                "base_url": base_url(),
                "access_token": result["accessToken"],
                "expires_at": int(time.time()) + int(result["expiresIn"]),
                "user": result.get("user", {}),
            })
            print(json.dumps({"ok": True, "authorized": result.get("user", {}), "config": str(CONFIG_PATH)}, ensure_ascii=False, indent=2))
            return
        if status != 428 or result.get("detail") != "authorization_pending":
            raise PublisherError(result.get("detail", f"Authorization failed: HTTP {status}"))
        time.sleep(interval)
    raise PublisherError("Authorization expired before it was approved")


def bearer() -> str:
    config = load_config()
    if int(config.get("expires_at", 0)) <= int(time.time()):
        raise PublisherError("Authorization expired. Run: publisher.py authorize")
    return str(config["access_token"])


def command_whoami(_: argparse.Namespace) -> None:
    status, result = api_request("/api/agent/me", token=bearer())
    if status != 200:
        raise PublisherError(result.get("detail", f"HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def command_projects(_: argparse.Namespace) -> None:
    status, result = api_request("/api/agent/projects", token=bearer())
    if status != 200:
        raise PublisherError(result.get("detail", f"HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def command_validate(args: argparse.Namespace) -> None:
    firmware = validate_esp_image(Path(args.firmware))
    cover = validate_cover(Path(args.cover))
    print(json.dumps({"ok": True, "firmware": firmware, "cover": cover}, ensure_ascii=False, indent=2))


def command_submit(args: argparse.Namespace) -> None:
    firmware_path = Path(args.firmware)
    cover_path = Path(args.cover)
    firmware = validate_esp_image(firmware_path)
    cover = validate_cover(cover_path)
    source_url = validate_source_repository_url(args.source_url) if args.source_url else ""
    fields = {
        "title_zh": args.title_zh,
        "description_zh": args.description_zh,
        "title_en": args.title_en or "",
        "description_en": args.description_en or "",
        "github_url": source_url,
    }
    preview_fields = {**fields}
    preview_fields["source_url"] = preview_fields.pop("github_url")
    preview = {"operation": "resubmit" if args.project_id else "create", "projectId": args.project_id, "fields": preview_fields, "firmware": firmware, "cover": cover}
    if not args.confirmed:
        print(json.dumps({"ok": True, "preview": preview, "upload": False, "next": "Show this preview to the creator. After explicit approval, rerun with --confirmed."}, ensure_ascii=False, indent=2))
        return
    body, content_type = multipart(fields, {"cover": cover_path, "firmware": firmware_path})
    path = f"/api/agent/submissions/{args.project_id}/resubmit" if args.project_id else "/api/agent/submissions"
    status, result = api_request(path, method="POST", token=bearer(), body=body, content_type=content_type)
    if status not in {200, 201}:
        raise PublisherError(result.get("detail", f"Upload failed: HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="FoloToy AI Passport community firmware publisher")
    commands = root.add_subparsers(dest="command", required=True)
    commands.add_parser("authorize", help="Authorize through the official website").set_defaults(func=command_authorize)
    commands.add_parser("whoami", help="Show the authorized creator").set_defaults(func=command_whoami)
    commands.add_parser("projects", help="List the creator's submissions").set_defaults(func=command_projects)
    validate = commands.add_parser("validate", help="Validate a cover and merged firmware without uploading")
    validate.add_argument("--firmware", required=True)
    validate.add_argument("--cover", required=True)
    validate.set_defaults(func=command_validate)
    submit = commands.add_parser("submit", help="Preview or submit a new project or revision")
    submit.add_argument("--title-zh", required=True)
    submit.add_argument("--description-zh", required=True)
    submit.add_argument("--title-en", default="")
    submit.add_argument("--description-en", default="")
    submit.add_argument("--source-url", "--github-url", dest="source_url", default="", help="Optional public HTTPS Git repository project page")
    submit.add_argument("--cover", required=True)
    submit.add_argument("--firmware", required=True)
    submit.add_argument("--project-id", type=int)
    submit.add_argument("--confirmed", action="store_true", help="Upload after the creator explicitly approved the preview")
    submit.set_defaults(func=command_submit)
    return root


def main() -> int:
    try:
        args = parser().parse_args()
        args.func(args)
        return 0
    except (PublisherError, FileNotFoundError, PermissionError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
