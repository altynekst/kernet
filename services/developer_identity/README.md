# KernelNet developer identity service

This service publishes short-lived, Ed25519-signed KernelNet identity sessions for developers, testers and supporters. The VPS cannot forge a role or a session: roles are certified by the offline root key, while each user signs sessions with their own private key.

## Protocol

1. A user generates a personal Ed25519 key. Only its public half is sent to the project owner.
2. The owner signs a role certificate (`developer`, `tester`, `supporter`) with the offline root key.
3. The game client requests a challenge for its current numeric server address, client slot, name, and clan.
4. The service returns the certified roles and a canonical message with a random nonce and a 15-second expiry.
5. The client validates the root certificate and every session field before signing the exact message.
6. Other KernelNet clients verify both signatures locally and compare the live slot/name/clan.

One identity key can mark only one active slot. A disconnected badge disappears when the signed statement expires, normally within 15 seconds. Clients use their own wall clock for expiry, so a compromised VPS cannot keep replaying an old valid signature.

## Issuing tester and supporter roles

The role owner first creates a personal key and sends only the printed public key to the project owner:

```bash
python generate_key.py user-id kernelnet_identity.key
```

On the trusted project-owner laptop, issue a tester certificate:

```bash
python issue_role.py user-id PUBLIC_KEY tester --root-key /private/kernelnet_root.key
```

For a donor of at least USD 3, issue `supporter`. Multiple roles can be listed together, for example `tester supporter`. Commit and deploy the updated `developers.json`; never commit either private key. Revocation is done by removing the identity from the registry or issuing a replacement certificate with a shorter lifetime.

## Local run

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python -m unittest -v test_identity_server.py
python identity_server.py
```

The API listens on `127.0.0.1:8088` by default. Test it with `curl http://127.0.0.1:8088/healthz`.

## VPS deployment

1. Create a `kernelnet` system user and copy this directory to `/opt/kernelnet-identity`.
2. Create the virtual environment, install `requirements.txt`, and keep `developers.json` readable by the service user.
3. Copy `kernelnet-identity.service` to `/etc/systemd/system/`, then enable and start it.
4. Put nginx in front of the localhost service and obtain a TLS certificate. Do not expose port 8088 publicly because the reference handler trusts nginx's `X-Real-IP` header.
5. Set `cl_zz_dev_identity_url` in every distributed client to the HTTPS origin.
6. On each certified user's computer, set `cl_zz_dev_identity_id`, `cl_zz_dev_identity_key_file` and `cl_zz_dev_identity_publish 1`. Publishing remains disabled by default.

Never copy a private key to the VPS or commit it. Adding a tester or supporter requires only a new root-signed entry in `developers.json`; clients do not need to be rebuilt because they embed only the root public key.
