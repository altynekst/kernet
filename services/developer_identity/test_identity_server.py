from __future__ import annotations

import base64
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

from identity_server import ApiError, Identity, IdentityState
from role_certificate import canonical_certificate, verify_certificate


class IdentityStateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root_key = Ed25519PrivateKey.generate()
        self.private_key = Ed25519PrivateKey.generate()
        public_bytes = self.private_key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        certificate = {
            "id": "tester-one",
            "display_name": "Tester One",
            "public_key": base64.b64encode(public_bytes).decode("ascii"),
            "roles": ["tester", "supporter"],
            "issued_at": 1,
            "expires_at": 100000000,
        }
        certificate["certificate_signature"] = base64.b64encode(
            self.root_key.sign(canonical_certificate(certificate).encode("utf-8"))
        ).decode("ascii")
        self.certificate = certificate
        identity = Identity(Ed25519PublicKey.from_public_bytes(public_bytes), certificate)
        self.state = IdentityState({"tester-one": identity}, session_ttl=30, challenge_ttl=15)
        self.payload = {"identity_id": "tester-one", "server": "127.0.0.1:8303", "client_id": 7, "name": "Dev", "clan": "KernelNet"}

    def activate(self, remote: str = "127.0.0.1") -> dict:
        challenge = self.state.issue_challenge(self.payload, remote)
        signature = self.private_key.sign(challenge["message"].encode("utf-8"))
        return self.state.activate({"challenge_id": challenge["challenge_id"], "signature": base64.b64encode(signature).decode("ascii")}, remote)

    def test_valid_signature_creates_session(self) -> None:
        self.activate()
        sessions = self.state.active_sessions(self.payload["server"])["sessions"]
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0]["client_id"], 7)

    def test_invalid_signature_is_rejected_and_challenge_is_consumed(self) -> None:
        challenge = self.state.issue_challenge(self.payload, "127.0.0.1")
        request = {"challenge_id": challenge["challenge_id"], "signature": base64.b64encode(bytes(64)).decode("ascii")}
        with self.assertRaises(ApiError):
            self.state.activate(request, "127.0.0.1")
        with self.assertRaises(ApiError):
            self.state.activate(request, "127.0.0.1")

    def test_challenge_is_bound_to_request_ip(self) -> None:
        challenge = self.state.issue_challenge(self.payload, "10.0.0.1")
        signature = self.private_key.sign(challenge["message"].encode("utf-8"))
        with self.assertRaises(ApiError):
            self.state.activate({"challenge_id": challenge["challenge_id"], "signature": base64.b64encode(signature).decode("ascii")}, "10.0.0.2")

    def test_one_developer_key_marks_only_one_slot(self) -> None:
        self.activate()
        self.payload["server"] = "127.0.0.2:8303"
        self.payload["client_id"] = 8
        self.activate()
        self.assertEqual(self.state.active_sessions("127.0.0.1:8303")["sessions"], [])
        self.assertEqual(len(self.state.active_sessions("127.0.0.2:8303")["sessions"]), 1)

    def test_expired_session_is_pruned(self) -> None:
        self.activate()
        self.state.sessions["tester-one"]["expires_at"] = 0
        self.assertEqual(self.state.active_sessions(self.payload["server"])["sessions"], [])

    def test_roles_and_certificate_are_bound_into_session_signature(self) -> None:
        self.activate()
        session = self.state.active_sessions(self.payload["server"])["sessions"][0]
        self.assertEqual(session["roles"], ["tester", "supporter"])
        self.assertIn("roles=tester,supporter", session["message"])

    def test_tampered_role_certificate_is_rejected(self) -> None:
        tampered = dict(self.certificate)
        tampered["roles"] = ["developer"]
        with self.assertRaises(ValueError):
            verify_certificate(tampered, self.root_key.public_key())


if __name__ == "__main__":
    unittest.main()
