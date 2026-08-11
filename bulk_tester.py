#!/usr/bin/env python3
r"""
End-to-end test harness for StickersFTW BotServer.

The harness:
  1. Starts a fake Telegram Bot API server.
  2. Generates a 120-sticker Telegram set with varied formats and metadata.
  3. Launches the StickersFTW executable with --server pointed at the fake API.
  4. Drives the public StickersFTW REST API.
  5. Verifies the behavior of the current C++ implementation:
       - manifest JSON fields
       - optional size / emoji / thumbnail serialization
       - public file_unique_id lookup
       - WebP, TGS, WebM, JPEG, and unknown MIME detection
       - exact binary forwarding
       - metadata and binary caches
       - 400 / 404 / 429 / 500 behavior
       - Telegram retry_after cooldown
       - graceful shutdown
       - push (uploadStickerFile / createNewStickerSet / addStickerToSet):
         creating a new set, appending to an existing one, requiring a
         title for brand-new sets, rejecting a user who hasn't started the
         bot, rejecting missing fields, and round-tripping a pushed sticker
         back through the normal download path

Standard-library only.

Usage:
  python stickersftw_tester_large.py path\to\StickersFTW.exe
  python stickersftw_tester_large.py ./StickersFTW --show-server-log
  python stickersftw_tester_large.py ./StickersFTW --wait-debugger
  python stickersftw_tester_large.py ./StickersFTW --relaxed-schema
  python stickersftw_tester_large.py ./StickersFTW --exe-arg=value
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Optional


TEST_TOKEN = "123456:STICKERS_FTW_TEST_TOKEN"

TEST_PACK = "StickersFTWLargePack_by_FakeBot"
SECOND_PACK = "StickersFTWSecondPack_by_FakeBot"
MISSING_PACK = "MissingPack_by_FakeBot"
MALFORMED_JSON_PACK = "MalformedJsonPack_by_FakeBot"
INVALID_STICKER_PACK = "InvalidStickerPack_by_FakeBot"
RATE_LIMITED_PACK = "RateLimitedPack_by_FakeBot"
AFTER_RATE_LIMIT_PACK = "AfterRateLimit_by_FakeBot"

# Push (createNewStickerSet/addStickerToSet/uploadStickerFile) test users.
# Telegram requires the owning user to have already DM'd the bot; the fake
# API rejects PUSH_UNSTARTED_USER_ID with PEER_ID_INVALID to simulate that.
PUSH_VALID_USER_ID = "555555555"
PUSH_UNSTARTED_USER_ID = "999999999"
START_LINK_USER_ID = 777777777
PLAIN_START_USER_ID = 888888888

STICKER_COUNT = 120

EMOJIS = (
    "🙂",
    "✨",
    "🎞️",
    "🐈",
    "🧪",
    "💻",
    "🛠️",
    "📦",
    "😡",
    "☺️",
    "🌙",
    "🎵",
)

MIME_BY_KIND = {
    "webp": "image/webp",
    "tgs": "application/x-tgsticker",
    "webm": "video/webm",
    "jpeg": "image/jpeg",
    "unknown": "application/octet-stream",
}


def deterministic_bytes(label: str, length: int) -> bytes:
    """Generate stable, nontrivial bytes without random-module global state."""
    if length < 0:
        raise ValueError("length must be nonnegative")

    output = bytearray()
    counter = 0
    while len(output) < length:
        output.extend(
            hashlib.sha256(f"{label}:{counter}".encode("utf-8")).digest()
        )
        counter += 1
    return bytes(output[:length])


def make_webp(label: str, payload_size: int) -> bytes:
    payload = deterministic_bytes(label, payload_size)
    # The C++ detector checks only RIFF at 0 and WEBP at 8.
    riff_size = len(payload) + 12
    return b"RIFF" + riff_size.to_bytes(4, "little") + b"WEBP" + payload


def make_tgs(label: str, item_index: int, text_size: int) -> bytes:
    # Valid gzip containing JSON-like Lottie data. The C++ detector checks gzip.
    document = {
        "v": "5.7.4",
        "fr": 30 + (item_index % 31),
        "ip": 0,
        "op": 30 + (item_index % 60),
        "w": 512,
        "h": 512,
        "nm": label,
        "layers": [],
        "test_payload": deterministic_bytes(
            f"{label}:json", text_size
        ).hex(),
    }
    return gzip.compress(
        json.dumps(document, separators=(",", ":")).encode("utf-8"),
        compresslevel=6,
    )


def make_webm(label: str, payload_size: int) -> bytes:
    # EBML magic followed by deterministic data. Enough for the C++ detector.
    return b"\x1A\x45\xDF\xA3" + deterministic_bytes(label, payload_size)


def make_jpeg(label: str, payload_size: int) -> bytes:
    # SOI + APP0/JFIF + deterministic payload + EOI.
    return (
        b"\xFF\xD8\xFF\xE0"
        b"\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
        + deterministic_bytes(label, payload_size)
        + b"\xFF\xD9"
    )


def make_unknown(label: str, payload_size: int) -> bytes:
    # Must not accidentally begin with any supported magic.
    return b"UNKNOWN!" + deterministic_bytes(label, payload_size)


@dataclass(frozen=True)
class FileFixture:
    file_id: str
    file_unique_id: str
    path: str
    data: bytes
    kind: str

    @property
    def mime_type(self) -> str:
        return MIME_BY_KIND[self.kind]


@dataclass(frozen=True)
class StickerFixture:
    public_id: str
    file_id: str
    width: int
    height: int
    kind: str
    emoji: Optional[str]
    expose_file_size: bool
    thumbnail_file_id: Optional[str]
    thumbnail_public_id: Optional[str]


def build_fixture_data() -> tuple[
    list[StickerFixture],
    dict[str, FileFixture],
    dict[str, str],
]:
    stickers: list[StickerFixture] = []
    files: dict[str, FileFixture] = {}
    path_to_file_id: dict[str, str] = {}

    for index in range(STICKER_COUNT):
        kind = ("webp", "tgs", "webm")[index % 3]
        public_id = f"{kind}_unique_{index:03d}"
        file_id = f"{kind}_file_{index:03d}"
        path = f"stickers/{index:03d}.{kind}"

        # Produce a range of payload sizes, including files large enough to
        # exercise binary forwarding without making the test painfully slow.
        if kind == "webp":
            data = make_webp(
                f"sticker:{index}",
                1024 + ((index * 7919) % 65536),
            )
        elif kind == "tgs":
            data = make_tgs(
                f"sticker:{index}",
                index,
                512 + ((index * 1543) % 8192),
            )
        else:
            data = make_webm(
                f"sticker:{index}",
                2048 + ((index * 6151) % 98304),
            )

        files[file_id] = FileFixture(
            file_id=file_id,
            file_unique_id=f"{file_id}_telegram_unique",
            path=path,
            data=data,
            kind=kind,
        )
        path_to_file_id[path] = file_id

        thumbnail_file_id: Optional[str] = None
        thumbnail_public_id: Optional[str] = None

        # Give half the stickers a thumbnail. Alternate JPEG and WebP thumbs.
        if index % 2 == 0:
            thumbnail_kind = "jpeg" if index % 4 == 0 else "webp"
            thumbnail_file_id = f"thumb_file_{index:03d}"
            thumbnail_public_id = f"thumb_unique_{index:03d}"
            extension = "jpg" if thumbnail_kind == "jpeg" else "webp"
            thumb_path = f"thumbnails/{index:03d}.{extension}"

            if thumbnail_kind == "jpeg":
                thumb_data = make_jpeg(
                    f"thumbnail:{index}",
                    256 + ((index * 97) % 4096),
                )
            else:
                thumb_data = make_webp(
                    f"thumbnail:{index}",
                    256 + ((index * 131) % 4096),
                )

            files[thumbnail_file_id] = FileFixture(
                file_id=thumbnail_file_id,
                file_unique_id=thumbnail_public_id,
                path=thumb_path,
                data=thumb_data,
                kind=thumbnail_kind,
            )
            path_to_file_id[thumb_path] = thumbnail_file_id

        # Exercise rectangular stickers while keeping one side exactly 512.
        if index % 2 == 0:
            width = 512
            height = 128 + ((index * 37) % 385)
        else:
            width = 128 + ((index * 53) % 385)
            height = 512

        stickers.append(
            StickerFixture(
                public_id=public_id,
                file_id=file_id,
                width=width,
                height=height,
                kind=kind,
                # Omit emoji regularly.
                emoji=None if index % 7 == 0 else EMOJIS[index % len(EMOJIS)],
                # Omit Telegram's optional file_size regularly.
                expose_file_size=index % 11 != 0,
                thumbnail_file_id=thumbnail_file_id,
                thumbnail_public_id=thumbnail_public_id,
            )
        )

    # Extra files not referenced by the normal pack. These let the fake API
    # test unknown MIME behavior and upstream getFile failures independently.
    unknown_file_id = "unknown_file_id"
    unknown_path = "misc/unknown.bin"
    unknown_data = make_unknown("unknown", 8192)
    files[unknown_file_id] = FileFixture(
        file_id=unknown_file_id,
        file_unique_id="unknown_telegram_unique",
        path=unknown_path,
        data=unknown_data,
        kind="unknown",
    )
    path_to_file_id[unknown_path] = unknown_file_id

    return stickers, files, path_to_file_id


STICKERS, FILES, PATH_TO_FILE_ID = build_fixture_data()
STICKER_BY_PUBLIC_ID = {sticker.public_id: sticker for sticker in STICKERS}


def telegram_sticker_object(sticker: StickerFixture) -> dict[str, Any]:
    obj: dict[str, Any] = {
        "file_id": sticker.file_id,
        "file_unique_id": sticker.public_id,
        "type": "regular",
        "width": sticker.width,
        "height": sticker.height,
        "is_animated": sticker.kind == "tgs",
        "is_video": sticker.kind == "webm",
    }

    if sticker.emoji is not None:
        obj["emoji"] = sticker.emoji

    if sticker.expose_file_size:
        obj["file_size"] = len(FILES[sticker.file_id].data)

    if sticker.thumbnail_file_id is not None:
        thumbnail = FILES[sticker.thumbnail_file_id]
        obj["thumbnail"] = {
            "file_id": thumbnail.file_id,
            "file_unique_id": sticker.thumbnail_public_id,
            "width": 128,
            "height": 128,
            "file_size": len(thumbnail.data),
        }

    return obj


def sticker_set_payload(name: str) -> dict[str, Any]:
    return {
        "ok": True,
        "result": {
            "name": name,
            "title": "Stickers FTW — 120 Sticker Integration Pack",
            "sticker_type": "regular",
            "stickers": [
                telegram_sticker_object(sticker) for sticker in STICKERS
            ],
        },
    }


def invalid_sticker_set_payload(name: str) -> dict[str, Any]:
    # The current parser rejects a sticker with an empty file_unique_id.
    return {
        "ok": True,
        "result": {
            "name": name,
            "title": "Invalid Sticker Pack",
            "stickers": [
                {
                    "file_id": "invalid_file",
                    "file_unique_id": "",
                    "width": 512,
                    "height": 512,
                    "is_animated": False,
                    "is_video": False,
                }
            ],
        },
    }


class FakeTelegramState:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.calls: Counter[tuple[str, str]] = Counter()
        # Sticker sets and files created via the push endpoints
        # (uploadStickerFile / createNewStickerSet / addStickerToSet),
        # keyed by their full "<name>_by_<bot_username>" Telegram name.
        self.created_sets: dict[str, dict[str, Any]] = {}
        self.uploaded_files: dict[str, bytes] = {}
        self.pending_updates: list[dict[str, Any]] = []
        self.sent_messages: list[dict[str, Any]] = []
        self.next_update_id = 1000

    def hit(self, operation: str, key: str = "") -> None:
        with self._lock:
            self.calls[(operation, key)] += 1

    def count(self, operation: str, key: str = "") -> int:
        with self._lock:
            return self.calls[(operation, key)]

    def snapshot(self) -> Counter[tuple[str, str]]:
        with self._lock:
            return self.calls.copy()

    def register_upload(self, file_id: str, data: bytes) -> None:
        with self._lock:
            self.uploaded_files[file_id] = data

    def get_uploaded(self, file_id: str) -> Optional[bytes]:
        with self._lock:
            return self.uploaded_files.get(file_id)

    def create_set(
        self,
        name: str,
        title: str,
        owner: str,
        sticker: dict[str, Any],
    ) -> None:
        with self._lock:
            self.created_sets[name] = {
                "title": title,
                "owner": owner,
                "stickers": [sticker],
            }

    def append_sticker(self, name: str, sticker: dict[str, Any]) -> bool:
        with self._lock:
            if name not in self.created_sets:
                return False
            self.created_sets[name]["stickers"].append(sticker)
            return True

    def get_created_set(self, name: str) -> Optional[dict[str, Any]]:
        with self._lock:
            data = self.created_sets.get(name)
            if data is None:
                return None
            return {
                "title": data["title"],
                "owner": data["owner"],
                "stickers": list(data["stickers"]),
            }

    def enqueue_private_message(self, user_id: int, text: str) -> int:
        with self._lock:
            update_id = self.next_update_id
            self.next_update_id += 1
            self.pending_updates.append(
                {
                    "update_id": update_id,
                    "message": {
                        "message_id": update_id,
                        "from": {
                            "id": user_id,
                            "is_bot": False,
                            "first_name": "Test User",
                        },
                        "chat": {
                            "id": user_id,
                            "type": "private",
                            "first_name": "Test User",
                        },
                        "text": text,
                    },
                }
            )
            return update_id

    def get_updates(self, offset: int) -> list[dict[str, Any]]:
        with self._lock:
            self.pending_updates = [
                item
                for item in self.pending_updates
                if int(item["update_id"]) >= offset
            ]
            return list(self.pending_updates[:100])

    def record_sent_message(self, message: dict[str, Any]) -> None:
        with self._lock:
            self.sent_messages.append(message)

    def sent_messages_snapshot(self) -> list[dict[str, Any]]:
        with self._lock:
            return list(self.sent_messages)


def parse_multipart_formdata(body: bytes, content_type: str) -> dict[str, bytes]:
    """Minimal multipart/form-data parser for the fake API's own inbound
    requests (StickersFTW calls Telegram's uploadStickerFile this way).
    Standard-library only, so this can't reach for a real multipart lib."""
    match = re.search(r'boundary="?([^";]+)"?', content_type)
    if not match:
        return {}

    boundary = ("--" + match.group(1)).encode("utf-8")
    fields: dict[str, bytes] = {}

    for part in body.split(boundary):
        part = part.strip(b"\r\n")
        if not part or part == b"--":
            continue
        if b"\r\n\r\n" not in part:
            continue

        header_blob, content = part.split(b"\r\n\r\n", 1)
        content = content[: -2] if content.endswith(b"\r\n") else content
        name_match = re.search(
            rb'name="([^"]+)"', header_blob
        )
        if not name_match:
            continue
        fields[name_match.group(1).decode("utf-8")] = content

    return fields


class FakeTelegramHandler(BaseHTTPRequestHandler):
    server_version = "FakeTelegramBotAPI/2.0"
    protocol_version = "HTTP/1.1"

    @property
    def state(self) -> FakeTelegramState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args: object) -> None:
        if getattr(self.server, "verbose", False):
            sys.stderr.write("[fake-telegram] " + (fmt % args) + "\n")

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(
            payload,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_bytes(
        self,
        status: int,
        body: bytes,
        content_type: str = "application/octet-stream",
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _parse_bot_method(self) -> tuple[Optional[str], Optional[str]]:
        parsed = urllib.parse.urlsplit(self.path)
        parts = parsed.path.strip("/").split("/")
        if len(parts) != 2 or not parts[0].startswith("bot"):
            return None, None
        return parts[0][3:], parts[1]

    def _check_token(self, token: Optional[str]) -> bool:
        if token == TEST_TOKEN:
            return True

        self._send_json(
            401,
            {
                "ok": False,
                "error_code": 401,
                "description": "Unauthorized",
            },
        )
        return False

    def do_POST(self) -> None:
        token, method = self._parse_bot_method()
        if not self._check_token(token):
            return

        if method == "getMe":
            self.state.hit("getMe")
            self._send_json(
                200,
                {
                    "ok": True,
                    "result": {
                        "id": 123456,
                        "is_bot": True,
                        "first_name": "StickersFTW Test Bot",
                        "username": "StickersFTWTestBot",
                    },
                },
            )
            return

        if method == "getUpdates":
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            params = urllib.parse.parse_qs(body.decode("utf-8"))
            offset = int(params.get("offset", ["0"])[0])
            self.state.hit("getUpdates", str(offset))
            updates = self.state.get_updates(offset)
            if not updates:
                # Real getUpdates blocks until an update or the requested
                # timeout. A short pause prevents the fake from becoming a
                # CPU-burning busy loop without slowing the test suite.
                time.sleep(0.05)
            self._send_json(200, {"ok": True, "result": updates})
            return

        if method == "sendMessage":
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            params = urllib.parse.parse_qs(body.decode("utf-8"))
            chat_id = params.get("chat_id", [""])[0]
            text = params.get("text", [""])[0]
            reply_markup_raw = params.get("reply_markup", ["{}"])[0]
            reply_markup = json.loads(reply_markup_raw)
            self.state.hit("sendMessage", chat_id)
            self.state.record_sent_message(
                {
                    "chat_id": chat_id,
                    "text": text,
                    "reply_markup": reply_markup,
                }
            )
            self._send_json(
                200,
                {
                    "ok": True,
                    "result": {
                        "message_id": self.state.count("sendMessage", chat_id),
                        "chat": {"id": int(chat_id), "type": "private"},
                        "text": text,
                    },
                },
            )
            return

        if method == "uploadStickerFile":
            self.state.hit("uploadStickerFile")
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            fields = parse_multipart_formdata(
                body, self.headers.get("Content-Type", "")
            )
            user_id = fields.get("user_id", b"").decode("utf-8", "replace")
            sticker_format = fields.get("sticker_format", b"").decode(
                "utf-8", "replace"
            )
            sticker_bytes = fields.get("sticker", b"")

            if user_id == PUSH_UNSTARTED_USER_ID:
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: PEER_ID_INVALID",
                    },
                )
                return

            if not sticker_bytes or sticker_format not in ("static", "video"):
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: STICKER_PNG_NOPNG",
                    },
                )
                return

            file_id = f"pushed_file_{self.state.count('uploadStickerFile')}"
            self.state.register_upload(file_id, sticker_bytes)
            self._send_json(
                200,
                {
                    "ok": True,
                    "result": {
                        "file_id": file_id,
                        "file_unique_id": file_id + "_unique",
                        "file_size": len(sticker_bytes),
                        "file_path": f"push/{file_id}",
                    },
                },
            )
            return

        if method == "createNewStickerSet":
            self.state.hit("createNewStickerSet")
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            params = urllib.parse.parse_qs(body.decode("utf-8"))
            user_id = params.get("user_id", [""])[0]
            name = params.get("name", [""])[0]
            title = params.get("title", [""])[0]
            stickers_raw = params.get("stickers", ["[]"])[0]

            if user_id == PUSH_UNSTARTED_USER_ID:
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: PEER_ID_INVALID",
                    },
                )
                return

            if self.state.get_created_set(name) is not None:
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: STICKERSET_NAME_OCCUPIED",
                    },
                )
                return

            stickers = json.loads(stickers_raw)
            if not stickers:
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: STICKERS_EMPTY",
                    },
                )
                return

            self.state.create_set(name, title, user_id, stickers[0])
            self._send_json(200, {"ok": True, "result": True})
            return

        if method == "addStickerToSet":
            self.state.hit("addStickerToSet")
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            params = urllib.parse.parse_qs(body.decode("utf-8"))
            user_id = params.get("user_id", [""])[0]
            name = params.get("name", [""])[0]
            sticker_raw = params.get("sticker", ["{}"])[0]

            if user_id == PUSH_UNSTARTED_USER_ID:
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: PEER_ID_INVALID",
                    },
                )
                return

            sticker = json.loads(sticker_raw)
            if not self.state.append_sticker(name, sticker):
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: STICKERSET_INVALID",
                    },
                )
                return

            self._send_json(200, {"ok": True, "result": True})
            return

        self._send_json(
            404,
            {
                "ok": False,
                "error_code": 404,
                "description": "Unknown method",
            },
        )

    def do_GET(self) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        parts = parsed.path.strip("/").split("/")

        # Telegram file endpoint:
        # /file/bot<TOKEN>/<file_path>
        if (
            len(parts) >= 3
            and parts[0] == "file"
            and parts[1].startswith("bot")
        ):
            token = parts[1][3:]
            if not self._check_token(token):
                return

            file_path = "/".join(parts[2:])
            self.state.hit("downloadFile", file_path)

            if file_path.startswith("push/"):
                pushed_file_id = file_path[len("push/") :]
                pushed_data = self.state.get_uploaded(pushed_file_id)
                if pushed_data is None:
                    self._send_json(
                        404,
                        {
                            "ok": False,
                            "error_code": 404,
                            "description": "File not found",
                        },
                    )
                    return
                self._send_bytes(200, pushed_data)
                return

            file_id = PATH_TO_FILE_ID.get(file_path)

            if file_id is None:
                self._send_json(
                    404,
                    {
                        "ok": False,
                        "error_code": 404,
                        "description": "File not found",
                    },
                )
                return

            self._send_bytes(200, FILES[file_id].data)
            return

        token, method = self._parse_bot_method()
        if not self._check_token(token):
            return

        query = urllib.parse.parse_qs(parsed.query)

        if method == "getStickerSet":
            name = query.get("name", [""])[0]
            self.state.hit("getStickerSet", name)

            created = self.state.get_created_set(name)
            if created is not None:
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "result": {
                            "name": name,
                            "title": created["title"],
                            "sticker_type": "regular",
                            "stickers": [
                                {
                                    "file_id": sticker["sticker"],
                                    "file_unique_id": sticker["sticker"]
                                    + "_unique",
                                    "type": "regular",
                                    "width": 512,
                                    "height": 512,
                                    "is_animated": False,
                                    "is_video": sticker.get("format")
                                    == "video",
                                    "emoji": (
                                        sticker.get("emoji_list") or [None]
                                    )[0],
                                    "file_size": len(
                                        self.state.get_uploaded(
                                            sticker["sticker"]
                                        )
                                        or b""
                                    ),
                                }
                                for sticker in created["stickers"]
                            ],
                        },
                    },
                )
                return

            if name == MISSING_PACK:
                # Ordinary Telegram errors can omit parameters entirely.
                self._send_json(
                    400,
                    {
                        "ok": False,
                        "error_code": 400,
                        "description": "Bad Request: STICKERSET_INVALID",
                    },
                )
                return

            if name == RATE_LIMITED_PACK:
                self._send_json(
                    429,
                    {
                        "ok": False,
                        "error_code": 429,
                        "description": "Too Many Requests: retry after 1",
                        "parameters": {"retry_after": 1},
                    },
                )
                return

            if name == MALFORMED_JSON_PACK:
                self._send_bytes(
                    200,
                    b"{ definitely not valid json",
                    "application/json",
                )
                return

            if name == INVALID_STICKER_PACK:
                self._send_json(200, invalid_sticker_set_payload(name))
                return

            self._send_json(200, sticker_set_payload(name))
            return

        if method == "getFile":
            file_id = query.get("file_id", [""])[0]
            self.state.hit("getFile", file_id)
            fixture = FILES.get(file_id)

            if fixture is not None:
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "result": {
                            "file_id": file_id,
                            "file_unique_id": fixture.file_unique_id,
                            "file_size": len(fixture.data),
                            "file_path": fixture.path,
                        },
                    },
                )
                return

            pushed_data = self.state.get_uploaded(file_id)
            if pushed_data is not None:
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "result": {
                            "file_id": file_id,
                            "file_unique_id": file_id + "_unique",
                            "file_size": len(pushed_data),
                            "file_path": f"push/{file_id}",
                        },
                    },
                )
                return

            self._send_json(
                400,
                {
                    "ok": False,
                    "error_code": 400,
                    "description": (
                        "Bad Request: wrong file identifier/"
                        "HTTP URL specified"
                    ),
                },
            )
            return

        self._send_json(
            404,
            {
                "ok": False,
                "error_code": 404,
                "description": "Unknown method",
            },
        )


class FakeTelegramServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        state: FakeTelegramState,
        verbose: bool,
    ) -> None:
        super().__init__(address, FakeTelegramHandler)
        self.state = state
        self.verbose = verbose


@dataclass
class HttpResponse:
    status: int
    headers: dict[str, str]
    body: bytes

    @property
    def content_type(self) -> str:
        return (
            self.headers.get("content-type", "")
            .split(";", 1)[0]
            .strip()
            .lower()
        )

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8"))


@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str = ""


def http_get(url: str, timeout: float = 5.0) -> HttpResponse:
    request = urllib.request.Request(
        url,
        method="GET",
        headers={"User-Agent": "StickersFTW-TestHarness/2.0"},
    )

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return HttpResponse(
                status=response.status,
                headers={
                    key.lower(): value
                    for key, value in response.headers.items()
                },
                body=response.read(),
            )
    except urllib.error.HTTPError as error:
        return HttpResponse(
            status=error.code,
            headers={
                key.lower(): value
                for key, value in error.headers.items()
            },
            body=error.read(),
        )


_TEST_BOUNDARY = "StickersFTWTestHarnessBoundary1234567890"


def build_multipart_body(
    fields: dict[str, str],
    files: dict[str, tuple[str, bytes, str]],
) -> bytes:
    parts: list[bytes] = []
    for name, value in fields.items():
        parts.append(
            (
                f"--{_TEST_BOUNDARY}\r\n"
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                f"{value}\r\n"
            ).encode("utf-8")
        )
    for name, (filename, content, content_type) in files.items():
        parts.append(
            (
                f"--{_TEST_BOUNDARY}\r\n"
                f'Content-Disposition: form-data; name="{name}"; '
                f'filename="{filename}"\r\n'
                f"Content-Type: {content_type}\r\n\r\n"
            ).encode("utf-8")
            + content
            + b"\r\n"
        )
    parts.append(f"--{_TEST_BOUNDARY}--\r\n".encode("utf-8"))
    return b"".join(parts)


def http_post_multipart(
    url: str,
    fields: dict[str, str],
    files: dict[str, tuple[str, bytes, str]],
    timeout: float = 5.0,
) -> HttpResponse:
    body = build_multipart_body(fields, files)
    request = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "User-Agent": "StickersFTW-TestHarness/2.0",
            "Content-Type": f"multipart/form-data; boundary={_TEST_BOUNDARY}",
        },
    )

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return HttpResponse(
                status=response.status,
                headers={
                    key.lower(): value
                    for key, value in response.headers.items()
                },
                body=response.read(),
            )
    except urllib.error.HTTPError as error:
        return HttpResponse(
            status=error.code,
            headers={
                key.lower(): value
                for key, value in error.headers.items()
            },
            body=error.read(),
        )


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_tcp(
    host: str,
    port: int,
    process: subprocess.Popen[str],
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    last_error: Optional[BaseException] = None

    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"StickersFTW exited before opening {host}:{port} "
                f"(exit code {process.returncode})"
            )

        try:
            with socket.create_connection((host, port), timeout=0.25):
                return
        except OSError as error:
            last_error = error
            time.sleep(0.05)

    raise TimeoutError(
        f"Timed out waiting for StickersFTW on {host}:{port}: {last_error}"
    )


class Harness:
    def __init__(
        self,
        executable: Path,
        cwd: Path,
        timeout: float,
        show_server_log: bool,
        fake_verbose: bool,
        strict_schema: bool,
        extra_args: list[str],
        wait_time: int | None,
    ) -> None:
        self.executable = executable
        self.cwd = cwd
        self.timeout = timeout
        self.show_server_log = show_server_log
        self.fake_verbose = fake_verbose
        self.strict_schema = strict_schema
        self.extra_args = extra_args
        self.wait_time = wait_time

        self.fake_state = FakeTelegramState()
        self.fake_server = FakeTelegramServer(
            ("127.0.0.1", 0),
            self.fake_state,
            fake_verbose,
        )
        self.fake_port = int(self.fake_server.server_address[1])
        self.app_port = free_tcp_port()
        self.app_base = f"http://127.0.0.1:{self.app_port}"

        self.fake_thread = threading.Thread(
            target=self.fake_server.serve_forever,
            name="fake-telegram-api",
            daemon=True,
        )

        self.process: Optional[subprocess.Popen[str]] = None
        self.log_lines: deque[str] = deque(maxlen=10000)
        self.log_thread: Optional[threading.Thread] = None
        self.results: list[TestResult] = []

        self._manifest_payload: Optional[dict[str, Any]] = None

    def start(self) -> None:
        self.fake_thread.start()

        command = [
            str(self.executable),
            "--host",
            "127.0.0.1",
            "--port",
            str(self.app_port),
            "--token",
            TEST_TOKEN,
            "--server",
            f"http://127.0.0.1:{self.fake_port}",
            "--log-level",
            "debug",
            *self.extra_args,
        ]

        popen_kwargs: dict[str, Any] = {
            "cwd": str(self.cwd),
            "stdout": subprocess.PIPE,
            "stderr": subprocess.STDOUT,
            "text": True,
            "encoding": "utf-8",
            "errors": "replace",
            "bufsize": 1,
        }

        if os.name == "nt":
            popen_kwargs["creationflags"] = (
                subprocess.CREATE_NEW_PROCESS_GROUP
            )
        else:
            popen_kwargs["start_new_session"] = True

        print(
            f"Generated {len(STICKERS)} stickers and "
            f"{len(FILES)} downloadable Telegram files."
        )
        total_bytes = sum(len(item.data) for item in FILES.values())
        print(f"Fixture binary data: {total_bytes:,} bytes")
        print("Launching:")
        print("  " + subprocess.list2cmdline(command))

        self.process = subprocess.Popen(command, **popen_kwargs)
        assert self.process.stdout is not None

        if self.wait_time:
            print(f"Sleeping for {self.wait_time} seconds for debugger attach")
            time.sleep(self.wait_time)

        def collect_logs() -> None:
            assert self.process is not None
            assert self.process.stdout is not None

            for line in self.process.stdout:
                line = line.rstrip("\r\n")
                self.log_lines.append(line)
                if self.show_server_log:
                    print(f"[stickersftw] {line}")

        self.log_thread = threading.Thread(
            target=collect_logs,
            name="stickersftw-log-reader",
            daemon=True,
        )
        self.log_thread.start()

        try:
            wait_for_tcp(
                "127.0.0.1",
                self.app_port,
                self.process,
                timeout=self.timeout,
            )
        except Exception:
            self.print_server_logs()
            raise

    def run_test(self, name: str, test: Callable[[], None]) -> None:
        try:
            test()
        except Exception as error:
            self.results.append(TestResult(name, False, str(error)))
            print(f"[FAIL] {name}: {error}")
        else:
            self.results.append(TestResult(name, True))
            print(f"[PASS] {name}")

    def url(self, path: str) -> str:
        return self.app_base + path

    def get_manifest(self) -> dict[str, Any]:
        if self._manifest_payload is None:
            response = http_get(
                self.url(f"/v1/set/{TEST_PACK}/"),
                timeout=self.timeout,
            )
            assert response.status == 200, (
                f"expected manifest 200, got {response.status}"
            )
            assert response.content_type == "application/json", (
                "expected application/json, got "
                f"{response.content_type!r}"
            )
            self._manifest_payload = response.json()

        return self._manifest_payload

    def test_startup_get_me(self) -> None:
        actual = self.fake_state.count("getMe")
        assert actual == 1, f"expected one startup getMe call, got {actual}"

    def test_large_manifest_exact_payload(self) -> None:
        payload = self.get_manifest()

        assert payload["name"] == TEST_PACK
        assert payload["title"] == (
            "Stickers FTW — 120 Sticker Integration Pack"
        )
        assert isinstance(payload.get("stickers"), list)
        assert len(payload["stickers"]) == STICKER_COUNT

        # The current C++ serializer emits only these top-level members.
        if self.strict_schema:
            assert set(payload) == {"name", "title", "stickers"}, (
                f"unexpected manifest keys: {sorted(payload)}"
            )
        else:
            assert {"name", "title", "stickers"} <= set(payload)

        returned_ids = [item["id"] for item in payload["stickers"]]
        expected_ids = [sticker.public_id for sticker in STICKERS]
        assert returned_ids == expected_ids, (
            "sticker order or public IDs differ from Telegram result"
        )

        for index, (returned, fixture) in enumerate(
            zip(payload["stickers"], STICKERS, strict=True)
        ):
            # "thumb" and "emoji" are only present when the fixture actually
            # has one -- the server omits absent optional fields entirely
            # rather than emitting them as null.
            expected_keys = {"id", "width", "height", "size"}
            if fixture.thumbnail_public_id is not None:
                expected_keys.add("thumb")
            if fixture.emoji is not None:
                expected_keys.add("emoji")

            if self.strict_schema:
                assert set(returned) == expected_keys, (
                    f"sticker {index} keys differ: {sorted(returned)}"
                )
            else:
                assert {"id", "width", "height", "size"} <= set(returned)

            assert returned["id"] == fixture.public_id
            assert returned["width"] == fixture.width
            assert returned["height"] == fixture.height

            expected_size = (
                len(FILES[fixture.file_id].data)
                if fixture.expose_file_size
                else 0
            )
            assert returned["size"] == expected_size, (
                f"sticker {index} size mismatch: "
                f"expected {expected_size}, got {returned['size']}"
            )

            expected_thumb = fixture.thumbnail_public_id
            assert returned.get("thumb") == expected_thumb, (
                f"sticker {index} thumb mismatch: "
                f"expected {expected_thumb!r}, "
                f"got {returned.get('thumb')!r}"
            )

            assert returned.get("emoji") == fixture.emoji, (
                f"sticker {index} emoji mismatch: "
                f"expected {fixture.emoji!r}, "
                f"got {returned.get('emoji')!r}"
            )

            # Internal Telegram file_id must not leak through the public JSON.
            assert fixture.file_id not in json.dumps(
                returned,
                ensure_ascii=False,
            )

    def test_manifest_has_no_sticker_count(self) -> None:
        payload = self.get_manifest()
        assert "sticker_count" not in payload, (
            "current GetStickerSetResponse serializer does not emit "
            "sticker_count"
        )

    def test_manifest_cache(self) -> None:
        before = self.fake_state.count("getStickerSet", TEST_PACK)

        for _ in range(10):
            response = http_get(
                self.url(f"/v1/set/{TEST_PACK}/"),
                timeout=self.timeout,
            )
            assert response.status == 200

        after = self.fake_state.count("getStickerSet", TEST_PACK)

        # get_manifest() should already have populated the server cache.
        assert after == before, (
            "cached manifest requests reached fake Telegram again: "
            f"before={before}, after={after}"
        )

    def test_independent_pack_cache_entries(self) -> None:
        before = self.fake_state.count("getStickerSet", SECOND_PACK)

        first = http_get(
            self.url(f"/v1/set/{SECOND_PACK}/"),
            timeout=self.timeout,
        )
        second = http_get(
            self.url(f"/v1/set/{SECOND_PACK}/"),
            timeout=self.timeout,
        )

        after = self.fake_state.count("getStickerSet", SECOND_PACK)

        assert first.status == 200 and second.status == 200
        assert after - before == 1, (
            "second pack should require exactly one upstream lookup"
        )

    def assert_download(
        self,
        sticker: StickerFixture,
    ) -> None:
        expected_file = FILES[sticker.file_id]
        response = http_get(
            self.url(f"/v1/set/{TEST_PACK}/{sticker.public_id}/"),
            timeout=self.timeout,
        )

        assert response.status == 200, (
            f"{sticker.public_id}: expected 200, got {response.status}"
        )
        assert response.body == expected_file.data, (
            f"{sticker.public_id}: binary mismatch; "
            f"expected {len(expected_file.data):,} bytes, "
            f"got {len(response.body):,}"
        )
        assert response.content_type == expected_file.mime_type, (
            f"{sticker.public_id}: expected "
            f"{expected_file.mime_type}, "
            f"got {response.content_type!r}"
        )

    def test_all_120_sticker_downloads(self) -> None:
        for sticker in STICKERS:
            self.assert_download(sticker)

    def test_all_thumbnails(self) -> None:
        thumbnail_count = 0
        jpeg_count = 0
        webp_count = 0

        for sticker in STICKERS:
            if sticker.thumbnail_file_id is None:
                continue

            thumbnail_count += 1
            expected_file = FILES[sticker.thumbnail_file_id]
            if expected_file.kind == "jpeg":
                jpeg_count += 1
            elif expected_file.kind == "webp":
                webp_count += 1

            response = http_get(
                self.url(
                    f"/v1/set/{TEST_PACK}/{sticker.public_id}/thumbnail/"
                ),
                timeout=self.timeout,
            )

            assert response.status == 200, (
                f"{sticker.public_id} thumbnail: "
                f"expected 200, got {response.status}"
            )
            assert response.body == expected_file.data
            assert response.content_type == expected_file.mime_type, (
                f"{sticker.public_id} thumbnail: "
                f"expected {expected_file.mime_type}, "
                f"got {response.content_type!r}"
            )

        assert thumbnail_count == STICKER_COUNT // 2
        assert jpeg_count > 0
        assert webp_count > 0

    def test_missing_thumbnails(self) -> None:
        checked = 0

        for sticker in STICKERS:
            if sticker.thumbnail_file_id is not None:
                continue

            response = http_get(
                self.url(
                    f"/v1/set/{TEST_PACK}/{sticker.public_id}/thumbnail/"
                ),
                timeout=self.timeout,
            )
            assert response.status == 404, (
                f"{sticker.public_id}: expected missing thumbnail 404, "
                f"got {response.status}"
            )
            checked += 1

            if checked >= 12:
                break

        assert checked == 12

    def test_unknown_sticker_id(self) -> None:
        response = http_get(
            self.url(f"/v1/set/{TEST_PACK}/does_not_exist/"),
            timeout=self.timeout,
        )
        assert response.status == 404, (
            f"expected unknown sticker 404, got {response.status}"
        )

    def test_public_id_is_not_file_id(self) -> None:
        # The public endpoint searches Sticker::id (file_unique_id), not file_id.
        fixture = STICKERS[0]
        response = http_get(
            self.url(f"/v1/set/{TEST_PACK}/{fixture.file_id}/"),
            timeout=self.timeout,
        )
        assert response.status == 404, (
            "private Telegram file_id unexpectedly worked as public ID"
        )

    def test_all_file_cache_entries(self) -> None:
        before = self.fake_state.snapshot()

        # All sticker and thumbnail files were downloaded by prior tests.
        # Repeat a broad subset and ensure no getFile or byte download occurs.
        for sticker in STICKERS[::3]:
            self.assert_download(sticker)

        for sticker in STICKERS[::8]:
            if sticker.thumbnail_file_id is None:
                continue
            response = http_get(
                self.url(
                    f"/v1/set/{TEST_PACK}/{sticker.public_id}/thumbnail/"
                ),
                timeout=self.timeout,
            )
            assert response.status == 200

        after = self.fake_state.snapshot()

        get_file_before = sum(
            count
            for (operation, _), count in before.items()
            if operation == "getFile"
        )
        get_file_after = sum(
            count
            for (operation, _), count in after.items()
            if operation == "getFile"
        )
        download_before = sum(
            count
            for (operation, _), count in before.items()
            if operation == "downloadFile"
        )
        download_after = sum(
            count
            for (operation, _), count in after.items()
            if operation == "downloadFile"
        )

        assert get_file_after == get_file_before, (
            "cached files caused additional Telegram getFile calls"
        )
        assert download_after == download_before, (
            "cached files caused additional Telegram file downloads"
        )

    def test_upstream_call_totals_after_bulk_download(self) -> None:
        # Every unique referenced file should have been resolved and downloaded
        # exactly once: 120 stickers + 60 thumbnails.
        referenced_file_ids = {
            sticker.file_id for sticker in STICKERS
        }
        referenced_file_ids.update(
            sticker.thumbnail_file_id
            for sticker in STICKERS
            if sticker.thumbnail_file_id is not None
        )

        assert None not in referenced_file_ids

        for file_id in referenced_file_ids:
            assert self.fake_state.count("getFile", str(file_id)) == 1, (
                f"{file_id}: expected one getFile call"
            )
            path = FILES[str(file_id)].path
            assert self.fake_state.count("downloadFile", path) == 1, (
                f"{file_id}: expected one binary download"
            )

    def test_telegram_400_propagation(self) -> None:
        response = http_get(
            self.url(f"/v1/set/{MISSING_PACK}/"),
            timeout=self.timeout,
        )
        assert response.status == 400, (
            f"expected Telegram 400 to remain 400, got {response.status}"
        )

    def test_malformed_telegram_json_becomes_500(self) -> None:
        response = http_get(
            self.url(f"/v1/set/{MALFORMED_JSON_PACK}/"),
            timeout=self.timeout,
        )
        assert response.status == 500, (
            "expected malformed upstream JSON to become 500, "
            f"got {response.status}"
        )

    def test_invalid_sticker_payload_becomes_500(self) -> None:
        response = http_get(
            self.url(f"/v1/set/{INVALID_STICKER_PACK}/"),
            timeout=self.timeout,
        )
        assert response.status == 500, (
            "expected invalid sticker metadata to become 500, "
            f"got {response.status}"
        )

    def test_rate_limit(self) -> None:
        limited = http_get(
            self.url(f"/v1/set/{RATE_LIMITED_PACK}/"),
            timeout=self.timeout,
        )
        assert limited.status == 429, (
            f"expected Telegram 429, got {limited.status}"
        )

        before = self.fake_state.count(
            "getStickerSet",
            AFTER_RATE_LIMIT_PACK,
        )
        locally_blocked = http_get(
            self.url(f"/v1/set/{AFTER_RATE_LIMIT_PACK}/"),
            timeout=self.timeout,
        )
        after = self.fake_state.count(
            "getStickerSet",
            AFTER_RATE_LIMIT_PACK,
        )

        assert locally_blocked.status == 429, (
            "expected local cooldown to return 429, "
            f"got {locally_blocked.status}"
        )
        assert after == before, (
            "request reached fake Telegram during local cooldown"
        )

        time.sleep(1.15)

        recovered = http_get(
            self.url(f"/v1/set/{AFTER_RATE_LIMIT_PACK}/"),
            timeout=self.timeout,
        )
        assert recovered.status == 200, (
            "expected recovery after retry_after, "
            f"got {recovered.status}"
        )

    def test_bot_info(self) -> None:
        response = http_get(self.url("/v1/bot/"), timeout=self.timeout)
        assert response.status == 200, f"expected 200, got {response.status}"
        payload = response.json()
        assert payload.get("username") == "StickersFTWTestBot", (
            f"unexpected bot username: {payload}"
        )

    def wait_for_sent_message(
        self,
        start_index: int,
        user_id: int,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            messages = self.fake_state.sent_messages_snapshot()
            for message in messages[start_index:]:
                if message["chat_id"] == str(user_id):
                    return message
            time.sleep(0.05)
        raise AssertionError(
            f"timed out waiting for onboarding reply to user {user_id}"
        )

    def assert_onboarding_reply(self, user_id: int, command: str) -> None:
        start_index = len(self.fake_state.sent_messages_snapshot())
        self.fake_state.enqueue_private_message(user_id, command)
        message = self.wait_for_sent_message(start_index, user_id)

        assert "Bot can convert your stickers now!" in message["text"]
        assert str(user_id) in message["text"]
        keyboard = message["reply_markup"].get("inline_keyboard")
        assert keyboard and keyboard[0], (
            f"missing inline copy keyboard: {message['reply_markup']}"
        )
        button = keyboard[0][0]
        assert button.get("text") == "Copy user ID"
        assert button.get("copy_text", {}).get("text") == str(user_id), (
            f"copy button contains the wrong ID: {button}"
        )

    def test_deep_link_start_reply(self) -> None:
        self.assert_onboarding_reply(
            START_LINK_USER_ID,
            "/start ftw_connect_v1",
        )

    def test_plain_start_reply(self) -> None:
        self.assert_onboarding_reply(PLAIN_START_USER_ID, "/start")

    def test_push_creates_new_set(self) -> None:
        sticker_bytes = make_webp("push:create", 4096)
        response = http_post_multipart(
            self.url("/v1/set/PushedNewPack/"),
            fields={
                "user_id": PUSH_VALID_USER_ID,
                "title": "Pushed New Pack",
                "format": "static",
                "emojis": "🙂,✨",
            },
            files={"sticker": ("sticker.webp", sticker_bytes, "image/webp")},
            timeout=self.timeout,
        )
        assert response.status == 201, (
            f"expected 201 creating a new set, got {response.status}: "
            f"{response.body!r}"
        )
        payload = response.json()
        assert payload["name"] == "PushedNewPack_by_StickersFTWTestBot", (
            f"unexpected full set name: {payload.get('name')!r}"
        )
        assert len(payload["stickers"]) == 1

    def test_push_appends_to_existing_set(self) -> None:
        # Uses its own short name so this test is independent of
        # test_push_creates_new_set's ordering.
        first = http_post_multipart(
            self.url("/v1/set/PushedAppendPack/"),
            fields={
                "user_id": PUSH_VALID_USER_ID,
                "title": "Pushed Append Pack",
                "format": "static",
                "emojis": "🙂",
            },
            files={
                "sticker": (
                    "sticker.webp",
                    make_webp("push:append:first", 2048),
                    "image/webp",
                )
            },
            timeout=self.timeout,
        )
        assert first.status == 201, (
            f"expected 201 for the initial push, got {first.status}"
        )

        second = http_post_multipart(
            self.url("/v1/set/PushedAppendPack/"),
            fields={
                "user_id": PUSH_VALID_USER_ID,
                "format": "static",
                "emojis": "😺",
            },
            files={
                "sticker": (
                    "sticker.webp",
                    make_webp("push:append:second", 2048),
                    "image/webp",
                )
            },
            timeout=self.timeout,
        )
        assert second.status == 200, (
            f"expected 200 appending to an existing set, got "
            f"{second.status}: {second.body!r}"
        )
        payload = second.json()
        assert len(payload["stickers"]) == 2, (
            f"expected 2 stickers after append, got "
            f"{len(payload['stickers'])}"
        )

    def test_push_requires_title_for_new_set(self) -> None:
        response = http_post_multipart(
            self.url("/v1/set/PushedNoTitlePack/"),
            fields={
                "user_id": PUSH_VALID_USER_ID,
                "format": "static",
                "emojis": "🙂",
            },
            files={
                "sticker": (
                    "sticker.webp",
                    make_webp("push:no-title", 1024),
                    "image/webp",
                )
            },
            timeout=self.timeout,
        )
        assert response.status == 400, (
            "expected 400 without a title for a nonexistent set, got "
            f"{response.status}"
        )

    def test_push_rejects_unstarted_user(self) -> None:
        response = http_post_multipart(
            self.url("/v1/set/PushedUnstartedPack/"),
            fields={
                "user_id": PUSH_UNSTARTED_USER_ID,
                "title": "Should Fail",
                "format": "static",
                "emojis": "🙂",
            },
            files={
                "sticker": (
                    "sticker.webp",
                    make_webp("push:unstarted", 1024),
                    "image/webp",
                )
            },
            timeout=self.timeout,
        )
        assert response.status == 400, (
            "expected 400 for a user who hasn't started the bot, got "
            f"{response.status}"
        )
        payload = response.json()
        assert "PEER_ID_INVALID" in payload.get("description", ""), (
            f"expected PEER_ID_INVALID surfaced in the error body, got "
            f"{payload}"
        )

    def test_push_rejects_missing_fields(self) -> None:
        response = http_post_multipart(
            self.url("/v1/set/PushedMissingFieldsPack/"),
            fields={"user_id": PUSH_VALID_USER_ID},
            files={},
            timeout=self.timeout,
        )
        assert response.status == 400, (
            f"expected 400 for missing required fields, got "
            f"{response.status}"
        )

    def test_pushed_sticker_is_downloadable(self) -> None:
        sticker_bytes = make_webp("push:download", 3000)
        push_response = http_post_multipart(
            self.url("/v1/set/PushedDownloadPack/"),
            fields={
                "user_id": PUSH_VALID_USER_ID,
                "title": "Pushed Download Pack",
                "format": "static",
                "emojis": "🙂",
            },
            files={"sticker": ("sticker.webp", sticker_bytes, "image/webp")},
            timeout=self.timeout,
        )
        assert push_response.status == 201, (
            f"expected 201, got {push_response.status}"
        )
        payload = push_response.json()
        full_name = payload["name"]
        sticker_id = payload["stickers"][0]["id"]

        download = http_get(
            self.url(f"/v1/set/{full_name}/{sticker_id}/"),
            timeout=self.timeout,
        )
        assert download.status == 200, f"expected 200, got {download.status}"
        assert download.body == sticker_bytes, (
            "pushed sticker bytes did not round-trip through Telegram"
        )

    def test_graceful_shutdown(self) -> None:
        assert self.process is not None

        if self.process.poll() is not None:
            raise AssertionError(
                f"process already exited with code {self.process.returncode}"
            )

        if os.name == "nt":
            self.process.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            os.killpg(self.process.pid, signal.SIGINT)

        try:
            code = self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired as error:
            raise AssertionError(
                f"process did not exit within "
                f"{self.timeout:.1f}s after interrupt"
            ) from error

        assert code == 0, f"expected exit code 0, got {code}"

    def run_all(self) -> None:
        tests: list[tuple[str, Callable[[], None]]] = [
            ("startup getMe validation", self.test_startup_get_me),
            (
                "120-sticker manifest exact payload",
                self.test_large_manifest_exact_payload,
            ),
            (
                "manifest omits sticker_count",
                self.test_manifest_has_no_sticker_count,
            ),
            ("sticker-set metadata cache", self.test_manifest_cache),
            (
                "independent pack cache entries",
                self.test_independent_pack_cache_entries,
            ),
            (
                "download all 120 sticker files",
                self.test_all_120_sticker_downloads,
            ),
            ("download all 60 thumbnails", self.test_all_thumbnails),
            ("missing thumbnails return 404", self.test_missing_thumbnails),
            ("unknown public sticker ID returns 404", self.test_unknown_sticker_id),
            (
                "private file_id is not a public ID",
                self.test_public_id_is_not_file_id,
            ),
            ("all downloaded files are cached", self.test_all_file_cache_entries),
            (
                "upstream call totals after bulk download",
                self.test_upstream_call_totals_after_bulk_download,
            ),
            ("Telegram 400 propagation", self.test_telegram_400_propagation),
            (
                "malformed Telegram JSON becomes 500",
                self.test_malformed_telegram_json_becomes_500,
            ),
            (
                "invalid sticker payload becomes 500",
                self.test_invalid_sticker_payload_becomes_500,
            ),
            ("Telegram retry_after cooldown", self.test_rate_limit),
            ("bot info endpoint", self.test_bot_info),
            (
                "deep-link /start replies with copyable user ID",
                self.test_deep_link_start_reply,
            ),
            (
                "plain /start replies with copyable user ID",
                self.test_plain_start_reply,
            ),
            ("push creates a new set", self.test_push_creates_new_set),
            (
                "push appends to an existing set",
                self.test_push_appends_to_existing_set,
            ),
            (
                "push without title on new set fails",
                self.test_push_requires_title_for_new_set,
            ),
            (
                "push rejects a user who hasn't started the bot",
                self.test_push_rejects_unstarted_user,
            ),
            (
                "push rejects missing required fields",
                self.test_push_rejects_missing_fields,
            ),
            (
                "pushed sticker round-trips through download",
                self.test_pushed_sticker_is_downloadable,
            ),
            ("graceful interrupt shutdown", self.test_graceful_shutdown),
        ]

        for name, test in tests:
            self.run_test(name, test)

    def print_server_logs(self) -> None:
        if not self.log_lines:
            print("\nNo StickersFTW output was captured.")
            return

        print("\n--- StickersFTW output (last lines) ---")
        for line in self.log_lines:
            print(line)
        print("--- end StickersFTW output ---")

    def cleanup(self) -> None:
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.terminate()
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2.0)

        self.fake_server.shutdown()
        self.fake_server.server_close()
        self.fake_thread.join(timeout=2.0)

        if self.log_thread is not None:
            self.log_thread.join(timeout=1.0)

    def summary(self) -> int:
        passed = sum(result.passed for result in self.results)
        failed = len(self.results) - passed

        print()
        print("=" * 72)
        print(f"Result: {passed} passed, {failed} failed")
        print("=" * 72)

        if failed:
            print("Failures:")
            for result in self.results:
                if not result.passed:
                    print(f"  - {result.name}: {result.detail}")
            self.print_server_logs()
            return 1

        print("All StickersFTW integration tests passed. ☺")
        return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch a fake Telegram Bot API and integration-test "
            "StickersFTW BotServer with a generated 120-sticker set."
        )
    )

    parser.add_argument(
        "executable",
        type=Path,
        help="Path to StickersFTW BotServer executable (.exe on Windows)",
    )
    parser.add_argument(
        "--cwd",
        type=Path,
        help="Working directory for the executable; defaults to its parent",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="Startup, request, and shutdown timeout in seconds (default: 8)",
    )
    parser.add_argument(
        "--show-server-log",
        action="store_true",
        help="Stream StickersFTW output while tests run",
    )
    parser.add_argument(
        "--verbose-fake-api",
        action="store_true",
        help="Print requests received by the fake Telegram API",
    )
    parser.add_argument(
        "--relaxed-schema",
        action="store_true",
        help=(
            "Require actual values but allow additional JSON fields "
            "or omitted null-valued optional fields"
        ),
    )
    parser.add_argument(
        "--exe-arg",
        action="append",
        default=[],
        help="Extra argument passed to the executable; may be repeated",
    )
    parser.add_argument(
        "--wait-debugger",
        action="store_true",
        help="Wait for up to 10 seconds to attach debugger",
    )

    return parser.parse_args()


def main() -> int:
    # Fixture data and server debug logs can contain arbitrary Unicode
    # (emoji, em-dashes); Windows consoles default to a narrow codepage
    # (cp1252) that can't encode it, which would otherwise crash the
    # harness while it's trying to report a failure.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")

    args = parse_args()
    executable = args.executable.expanduser().resolve()

    if not executable.exists():
        print(
            f"error: executable does not exist: {executable}",
            file=sys.stderr,
        )
        return 2

    if not executable.is_file():
        print(
            f"error: executable path is not a file: {executable}",
            file=sys.stderr,
        )
        return 2

    cwd = (
        args.cwd.expanduser().resolve()
        if args.cwd
        else executable.parent
    )

    if not cwd.is_dir():
        print(
            f"error: working directory does not exist: {cwd}",
            file=sys.stderr,
        )
        return 2

    extra_args = list(args.exe_arg)
    wait_time = 10 if args.wait_debugger else None

    harness = Harness(
        executable=executable,
        cwd=cwd,
        timeout=args.timeout,
        show_server_log=args.show_server_log,
        fake_verbose=args.verbose_fake_api,
        strict_schema=not args.relaxed_schema,
        extra_args=extra_args,
        wait_time=wait_time,
    )

    try:
        harness.start()
        harness.run_all()
        return harness.summary()
    except KeyboardInterrupt:
        print("\nTest harness interrupted.", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"\nHarness failure: {error}", file=sys.stderr)
        harness.print_server_logs()
        return 2
    finally:
        harness.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
