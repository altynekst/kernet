#!/usr/bin/env python3
"""Issue or replace a root-signed KernelNet role certificate."""

from __future__ import annotations

import argparse
import base64
import json
import time
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from role_certificate import canonical_certificate, normalize_roles


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("identity_id")
    parser.add_argument("public_key", help="Base64 Ed25519 public key supplied by the role owner")
    parser.add_argument("roles", nargs="+", help="developer, tester and/or supporter")
    parser.add_argument("--root-key", type=Path, required=True)
    parser.add_argument("--registry", type=Path, default=Path(__file__).with_name("developers.json"))
    parser.add_argument("--days", type=int, default=365)
    args = parser.parse_args()
    if not 1 <= args.days <= 3650:
        raise SystemExit("--days must be between 1 and 3650")

    public_key = base64.b64decode(args.public_key, validate=True)
    secret_key = base64.b64decode(args.root_key.read_text(encoding="ascii").strip(), validate=True)
    if len(public_key) != 32 or len(secret_key) != 64:
        raise SystemExit("expected a 32-byte public key and a 64-byte KernelNet private key file")
    now = int(time.time())
    entry = {
        "id": args.identity_id,
        "display_name": args.identity_id,
        "public_key": args.public_key,
        "roles": normalize_roles(args.roles),
        "issued_at": now,
        "expires_at": now + args.days * 24 * 60 * 60,
    }
    signature = Ed25519PrivateKey.from_private_bytes(secret_key[:32]).sign(canonical_certificate(entry).encode("utf-8"))
    entry["certificate_signature"] = base64.b64encode(signature).decode("ascii")

    registry = json.loads(args.registry.read_text(encoding="utf-8"))
    registry["identities"] = [item for item in registry.get("identities", []) if item.get("id") != args.identity_id]
    registry["identities"].append(entry)
    registry["identities"].sort(key=lambda item: item["id"])
    args.registry.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8")
    print(f"issued roles={','.join(entry['roles'])} identity_id={args.identity_id} expires_at={entry['expires_at']}")


if __name__ == "__main__":
    main()
