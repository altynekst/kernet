#!/usr/bin/env python3
"""
fentbot_server.py
TCP server that receives observation vector and returns action id.
- compatible with checkpoints saved by fentbot_bc_train_fixed.py
- applies normalization if checkpoint contains norm_mean/norm_std
"""
from __future__ import annotations
import argparse
import socket
import struct
import threading
from typing import Tuple, Optional

import torch
import torch.nn as nn
import torch.nn.functional as F

MAGIC = b"FENTPY01"  # protocol magic for runtime server


def recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf.extend(chunk)
    return bytes(buf)


def default_policy(obs: Tuple[float, ...]) -> int:
    # same simple heuristic as before
    dx = obs[0] if len(obs) > 0 else 0.0
    if dx > 0.02:
        dir_index = 2
    elif dx < -0.02:
        dir_index = 0
    else:
        dir_index = 1
    jump = 0
    hook = 0
    aim_index = 0
    return (((dir_index * 2 + jump) * 2 + hook) * 5 + aim_index)


class PolicyMLP(nn.Module):
    def __init__(self, obs_dim: int, actions: int, hidden: int = 256, dropout: float = 0.1):
        super().__init__()
        # keep same names to ensure state_dict compatibility
        self.fc1 = nn.Linear(obs_dim, hidden)
        self.fc2 = nn.Linear(hidden, hidden)
        self.out = nn.Linear(hidden, actions)
        self._dropout_p = float(dropout)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = F.layer_norm(x, (x.size(1),))
        h1 = F.gelu(self.fc1(x))
        h2 = F.gelu(self.fc2(h1))
        h = 0.5 * (h1 + h2)
        h = F.dropout(h, p=self._dropout_p, training=self.training)
        return self.out(h)


@torch.no_grad()
def model_policy(
    model: PolicyMLP,
    obs: Tuple[float, ...],
    device: torch.device,
    norm_mean: Optional[torch.Tensor] = None,
    norm_std: Optional[torch.Tensor] = None,
) -> int:
    x = torch.tensor([list(obs)], dtype=torch.float32, device=device)
    if norm_mean is not None and norm_std is not None:
        # ensure shapes match
        if norm_mean.numel() == x.size(1):
            x = (x - norm_mean.unsqueeze(0)) / norm_std.unsqueeze(0)
    logits = model(x)
    a = int(torch.argmax(logits, dim=1).item())
    return a


def handle_client(
    conn: socket.socket, addr, model: Optional[PolicyMLP], device: torch.device, norm_mean: Optional[torch.Tensor], norm_std: Optional[torch.Tensor]
) -> None:
    try:
        while True:
            magic = recv_exact(conn, 8)
            if magic != MAGIC:
                raise ValueError("bad magic")

            (obs_dim,) = struct.unpack("<i", recv_exact(conn, 4))
            if obs_dim <= 0 or obs_dim > 100000:
                raise ValueError("bad obs_dim")

            obs_bytes = recv_exact(conn, obs_dim * 4)
            obs = struct.unpack("<" + "f" * obs_dim, obs_bytes)

            if model is not None:
                action_id = int(model_policy(model, obs, device, norm_mean=norm_mean, norm_std=norm_std))
            else:
                action_id = int(default_policy(obs))
            conn.sendall(struct.pack("<i", action_id))
    except Exception:
        try:
            conn.close()
        except Exception:
            pass


def serve(host: str, port: int, model: Optional[PolicyMLP], device: torch.device, norm_mean: Optional[torch.Tensor], norm_std: Optional[torch.Tensor]) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(16)
    print(f"listening on {host}:{port}")

    while True:
        conn, addr = srv.accept()
        t = threading.Thread(target=handle_client, args=(conn, addr, model, device, norm_mean, norm_std), daemon=True)
        t.start()


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5657)
    p.add_argument("--model", default="", help="Path to trained BC model (.pt). If empty, uses heuristic.")
    p.add_argument("--device", default="cpu")
    args = p.parse_args()

    device = torch.device(args.device)
    model: Optional[PolicyMLP] = None
    norm_mean: Optional[torch.Tensor] = None
    norm_std: Optional[torch.Tensor] = None
    if args.model:
        ckpt = torch.load(args.model, map_location=device)
        obs_dim = int(ckpt["obs_dim"])
        actions = int(ckpt["actions"])
        hidden = int(ckpt.get("hidden", 256))
        model = PolicyMLP(obs_dim, actions, hidden=hidden).to(device)
        try:
            model.load_state_dict(ckpt["state_dict"])
        except Exception as e:
            # попытка загрузить менее строго — покажем предупреждение
            print("warning: strict state_dict load failed:", e)
            try:
                model.load_state_dict(ckpt["state_dict"], strict=False)
                print("loaded with strict=False (some keys ignored/extra).")
            except Exception as e2:
                print("failed to load model state_dict:", e2)
                model = None

        model.eval()
        if "norm_mean" in ckpt and "norm_std" in ckpt:
            nm = torch.tensor(ckpt["norm_mean"], dtype=torch.float32, device=device)
            ns = torch.tensor(ckpt["norm_std"], dtype=torch.float32, device=device)
            norm_mean = nm
            norm_std = ns
        print(f"loaded model: {args.model} (obs_dim={obs_dim} actions={actions} hidden={hidden})")

    serve(args.host, args.port, model, device, norm_mean, norm_std)


if __name__ == "__main__":
    main()