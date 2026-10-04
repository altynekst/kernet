"""Canonical KernelNet role certificates and session messages."""

from __future__ import annotations

import base64
from typing import Any, Iterable

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

ALLOWED_ROLES = ("developer", "tester", "supporter")
MAX_CERTIFICATE_SECONDS = 10 * 365 * 24 * 60 * 60


def normalize_roles(roles: Iterable[str]) -> list[str]:
    result = sorted(set(roles), key=ALLOWED_ROLES.index)
    if not result or any(role not in ALLOWED_ROLES for role in result):
        raise ValueError("roles must contain only developer, tester and supporter")
    return result


def canonical_certificate(entry: dict[str, Any]) -> str:
    roles = normalize_roles(entry["roles"])
    return "\n".join(
        (
            "kernelnet-role-v1",
            f"identity_id={entry['id']}",
            f"public_key={entry['public_key']}",
            f"roles={','.join(roles)}",
            f"issued_at={entry['issued_at']}",
            f"expires_at={entry['expires_at']}",
        )
    )


def verify_certificate(entry: dict[str, Any], root_public_key: Ed25519PublicKey) -> None:
    public_key = base64.b64decode(entry["public_key"], validate=True)
    signature = base64.b64decode(entry["certificate_signature"], validate=True)
    if len(public_key) != 32 or len(signature) != 64:
        raise ValueError("invalid role certificate key or signature length")
    if entry["expires_at"] <= entry["issued_at"] or entry["expires_at"] - entry["issued_at"] > MAX_CERTIFICATE_SECONDS:
        raise ValueError("invalid role certificate lifetime")
    try:
        root_public_key.verify(signature, canonical_certificate(entry).encode("utf-8"))
    except InvalidSignature as error:
        raise ValueError(f"invalid role certificate for {entry.get('id', '?')}") from error


def _b64_text(value: str) -> str:
    return base64.b64encode(value.encode("utf-8")).decode("ascii")


def canonical_session(record: dict[str, Any]) -> str:
    roles = normalize_roles(record["roles"])
    return "\n".join(
        (
            "kernelnet-session-v2",
            f"identity_id={record['identity_id']}",
            f"identity_public_key={record['identity_public_key']}",
            f"roles={','.join(roles)}",
            f"certificate_issued_at={record['certificate_issued_at']}",
            f"certificate_expires_at={record['certificate_expires_at']}",
            f"certificate_signature={record['certificate_signature']}",
            f"server={_b64_text(record['server'])}",
            f"client_id={record['client_id']}",
            f"name={_b64_text(record['name'])}",
            f"clan={_b64_text(record['clan'])}",
            f"issued_at={record['issued_at']}",
            f"expires_at={record['expires_at']}",
            f"nonce={record['nonce']}",
        )
    )
