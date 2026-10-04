#!/usr/bin/env python3
"""Generate a KernelNet Ed25519 key file without printing the private key."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("identity_id")
    parser.add_argument("private_key_file", type=Path)
    args = parser.parse_args()

    private_key = Ed25519PrivateKey.generate()
    seed = private_key.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    public_key = private_key.public_key().public_bytes(
        serialization.Encoding.Raw,
        serialization.PublicFormat.Raw,
    )
    args.private_key_file.parent.mkdir(parents=True, exist_ok=True)
    args.private_key_file.write_text(base64.b64encode(seed + public_key).decode("ascii") + "\n", encoding="ascii")
    try:
        os.chmod(args.private_key_file, 0o600)
    except OSError:
        pass

    encoded_public_key = base64.b64encode(public_key).decode("ascii")
    print(f"identity_id={args.identity_id}")
    print(f"public_key={encoded_public_key}")
    print(f"private_key_file={args.private_key_file}")


if __name__ == "__main__":
    main()
