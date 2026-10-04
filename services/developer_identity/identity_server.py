#!/usr/bin/env python3
"""KernelNet short-lived Ed25519 developer identity service."""

from __future__ import annotations

import base64
import json
import os
import secrets
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from role_certificate import canonical_session, normalize_roles, verify_certificate

PROTOCOL_VERSION = 2
MAX_BODY_BYTES = 16 * 1024
MAX_CLIENT_ID = 127
MAX_SESSION_TTL = 45


class ApiError(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


def _validated_text(value: Any, field: str, maximum: int, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or len(value.encode("utf-8")) > maximum:
        raise ApiError(HTTPStatus.BAD_REQUEST, f"invalid {field}")
    if not allow_empty and not value:
        raise ApiError(HTTPStatus.BAD_REQUEST, f"invalid {field}")
    if any(ord(char) < 0x20 for char in value):
        raise ApiError(HTTPStatus.BAD_REQUEST, f"invalid {field}")
    return value


@dataclass(frozen=True)
class Identity:
    public_key: Ed25519PublicKey
    certificate: dict[str, Any]


def load_identities(path: Path) -> dict[str, Identity]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    root_bytes = base64.b64decode(raw.get("root_public_key", ""), validate=True)
    if len(root_bytes) != 32:
        raise ValueError("developers.json must contain a 32-byte root_public_key")
    root_public_key = Ed25519PublicKey.from_public_bytes(root_bytes)
    entries = raw.get("identities")
    if not isinstance(entries, list) or not entries:
        raise ValueError("developers.json must contain a non-empty identities array")
    result: dict[str, Identity] = {}
    for entry in entries:
        identity_id = _validated_text(entry.get("id"), "identity id", 31)
        public_key = base64.b64decode(entry.get("public_key", ""), validate=True)
        normalize_roles(entry.get("roles", []))
        verify_certificate(entry, root_public_key)
        if len(public_key) != 32 or identity_id in result:
            raise ValueError(f"invalid or duplicate identity: {identity_id}")
        result[identity_id] = Identity(Ed25519PublicKey.from_public_bytes(public_key), dict(entry))
    return result


@dataclass
class Challenge:
    record: dict[str, Any]
    remote: str
    challenge_expires_at: int


class IdentityState:
    def __init__(self, identities: dict[str, Identity], session_ttl: int = 15, challenge_ttl: int = 10) -> None:
        if not 5 <= session_ttl <= MAX_SESSION_TTL:
            raise ValueError(f"session_ttl must be between 5 and {MAX_SESSION_TTL}")
        if not 5 <= challenge_ttl <= 30:
            raise ValueError("challenge_ttl must be between 5 and 30")
        self.identities = identities
        self.session_ttl = session_ttl
        self.challenge_ttl = challenge_ttl
        self.challenges: dict[str, Challenge] = {}
        self.sessions: dict[str, dict[str, Any]] = {}
        self.lock = threading.Lock()

    def _prune_locked(self, now: int) -> None:
        self.challenges = {key: value for key, value in self.challenges.items() if value.challenge_expires_at >= now}
        self.sessions = {key: value for key, value in self.sessions.items() if value["expires_at"] >= now}

    def issue_challenge(self, payload: dict[str, Any], remote: str) -> dict[str, Any]:
        identity_id = _validated_text(payload.get("identity_id"), "identity_id", 31)
        identity = self.identities.get(identity_id)
        if identity is None:
            raise ApiError(HTTPStatus.NOT_FOUND, "unknown identity")
        server = _validated_text(payload.get("server"), "server", 127)
        name = _validated_text(payload.get("name"), "name", 63)
        clan = _validated_text(payload.get("clan"), "clan", 63, allow_empty=True)
        client_id = payload.get("client_id")
        if isinstance(client_id, bool) or not isinstance(client_id, int) or not 0 <= client_id <= MAX_CLIENT_ID:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid client_id")

        now = int(time.time())
        record: dict[str, Any] = {
            "identity_id": identity_id,
            "identity_public_key": identity.certificate["public_key"],
            "roles": identity.certificate["roles"],
            "certificate_issued_at": identity.certificate["issued_at"],
            "certificate_expires_at": identity.certificate["expires_at"],
            "certificate_signature": identity.certificate["certificate_signature"],
            "server": server,
            "client_id": client_id,
            "name": name,
            "clan": clan,
            "issued_at": now,
            "expires_at": now + self.session_ttl,
            "nonce": base64.b64encode(secrets.token_bytes(24)).decode("ascii"),
        }
        record["message"] = canonical_session(record)
        challenge_id = secrets.token_urlsafe(24)
        with self.lock:
            self._prune_locked(now)
            if len(self.challenges) >= 2048:
                oldest = min(self.challenges, key=lambda key: self.challenges[key].challenge_expires_at)
                self.challenges.pop(oldest, None)
            self.challenges[challenge_id] = Challenge(record, remote, now + self.challenge_ttl)
        return {"version": PROTOCOL_VERSION, "server_time": now, "challenge_id": challenge_id, **record}

    def activate(self, payload: dict[str, Any], remote: str) -> dict[str, Any]:
        challenge_id = _validated_text(payload.get("challenge_id"), "challenge_id", 64)
        encoded_signature = _validated_text(payload.get("signature"), "signature", 128)
        now = int(time.time())
        with self.lock:
            self._prune_locked(now)
            challenge = self.challenges.get(challenge_id)
            if challenge is None:
                raise ApiError(HTTPStatus.GONE, "challenge expired or already used")
            if challenge.remote != remote:
                raise ApiError(HTTPStatus.FORBIDDEN, "challenge belongs to another client")
            self.challenges.pop(challenge_id, None)

        try:
            signature = base64.b64decode(encoded_signature, validate=True)
        except (ValueError, TypeError) as error:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid signature encoding") from error
        if len(signature) != 64:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid signature length")
        record = challenge.record
        try:
            self.identities[record["identity_id"]].public_key.verify(signature, record["message"].encode("utf-8"))
        except Exception as error:
            raise ApiError(HTTPStatus.UNAUTHORIZED, "signature verification failed") from error

        active_record = {**record, "signature": encoded_signature}
        with self.lock:
            self._prune_locked(now)
            # One identity key represents one person and may mark one slot at a time.
            self.sessions[record["identity_id"]] = active_record
        return {"version": PROTOCOL_VERSION, "server_time": now, "session": active_record}

    def active_sessions(self, server: str) -> dict[str, Any]:
        server = _validated_text(server, "server", 127)
        now = int(time.time())
        with self.lock:
            self._prune_locked(now)
            sessions = [dict(record) for record in self.sessions.values() if record["server"] == server]
        sessions.sort(key=lambda record: (record["client_id"], record["identity_id"]))
        return {"version": PROTOCOL_VERSION, "server_time": now, "sessions": sessions}


class RateLimiter:
    def __init__(self, limit: int = 120, window_seconds: int = 60) -> None:
        self.limit = limit
        self.window_seconds = window_seconds
        self.entries: dict[str, deque[float]] = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, remote: str) -> bool:
        now = time.monotonic()
        with self.lock:
            bucket = self.entries[remote]
            while bucket and bucket[0] <= now - self.window_seconds:
                bucket.popleft()
            if len(bucket) >= self.limit:
                return False
            bucket.append(now)
            return True


class IdentityHandler(BaseHTTPRequestHandler):
    state: IdentityState
    rate_limiter = RateLimiter()
    server_version = "KernelNetIdentity/1"

    def _remote(self) -> str:
        # The reference nginx config binds this service to localhost, so X-Real-IP is trusted.
        return self.headers.get("X-Real-IP", self.client_address[0])

    def _write_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid content length") from error
        if not 0 < length <= MAX_BODY_BYTES:
            raise ApiError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "request body is too large")
        try:
            payload = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise ApiError(HTTPStatus.BAD_REQUEST, "invalid JSON") from error
        if not isinstance(payload, dict):
            raise ApiError(HTTPStatus.BAD_REQUEST, "JSON root must be an object")
        return payload

    def _dispatch(self, callback: Any) -> None:
        try:
            if not self.rate_limiter.allow(self._remote()):
                raise ApiError(HTTPStatus.TOO_MANY_REQUESTS, "rate limit exceeded")
            callback()
        except ApiError as error:
            self._write_json(error.status, {"error": error.message})
        except Exception as error:
            self.log_error("unhandled request error: %s", error)
            self._write_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": "internal server error"})

    def do_GET(self) -> None:  # noqa: N802
        def handle() -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/healthz":
                self._write_json(HTTPStatus.OK, {"status": "ok"})
                return
            if parsed.path != "/v1/sessions":
                raise ApiError(HTTPStatus.NOT_FOUND, "not found")
            server = parse_qs(parsed.query, keep_blank_values=True).get("server", [""])[0]
            self._write_json(HTTPStatus.OK, self.state.active_sessions(server))

        self._dispatch(handle)

    def do_POST(self) -> None:  # noqa: N802
        def handle() -> None:
            payload = self._read_json()
            if self.path == "/v1/challenge":
                response = self.state.issue_challenge(payload, self._remote())
            elif self.path == "/v1/activate":
                response = self.state.activate(payload, self._remote())
            else:
                raise ApiError(HTTPStatus.NOT_FOUND, "not found")
            self._write_json(HTTPStatus.OK, response)

        self._dispatch(handle)


def main() -> None:
    service_dir = Path(__file__).resolve().parent
    developers_file = Path(os.getenv("KERNELNET_DEVELOPERS_FILE", service_dir / "developers.json"))
    host = os.getenv("KERNELNET_HOST", "127.0.0.1")
    port = int(os.getenv("KERNELNET_PORT", "8088"))
    session_ttl = int(os.getenv("KERNELNET_SESSION_TTL", "15"))
    challenge_ttl = int(os.getenv("KERNELNET_CHALLENGE_TTL", "10"))
    IdentityHandler.state = IdentityState(load_identities(developers_file), session_ttl, challenge_ttl)
    server = ThreadingHTTPServer((host, port), IdentityHandler)
    print(f"KernelNet identity service listening on {host}:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
