#!/usr/bin/env python3
"""
fentbot_bc_train_fixed.py
Behavior cloning trainer for FENTBC datasets.

- рекурсивно ищет .bin в указанной папке/списке
- нормализует признаки (mean/std) и сохраняет их в чекпойнт
- улучшенный PolicyMLP: layer-norm on input, GELU, residual averaging, dropout
- сохраняет "hidden" в чекпойнт для совместимости
"""
from __future__ import annotations
import argparse
import os
import struct
from dataclasses import dataclass
from typing import List, Tuple, Optional

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, TensorDataset

MAGIC = b"FENTBC01"


@dataclass
class BCDataset:
    obs: torch.Tensor  # normalized, cpu
    act: torch.Tensor  # cpu
    obs_dim: int
    actions: int


def iter_dataset_files(dataset_arg: str) -> List[str]:
    parts = [p.strip() for p in dataset_arg.replace(",", ";").split(";") if p.strip()]
    out: List[str] = []
    import glob

    for p in parts:
        # absolute / relative directory
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for fn in files:
                    if fn.lower().endswith(".bin"):
                        out.append(os.path.join(root, fn))
            continue

        # file path
        if os.path.isfile(p):
            out.append(p)
            continue

        # relative to cwd
        maybe = os.path.join(os.getcwd(), p)
        if os.path.isdir(maybe):
            for root, _, files in os.walk(maybe):
                for fn in files:
                    if fn.lower().endswith(".bin"):
                        out.append(os.path.join(root, fn))
            continue
        if os.path.isfile(maybe):
            out.append(maybe)
            continue

        # glob support
        matches = glob.glob(p)
        for m in matches:
            if os.path.isdir(m):
                for root, _, files in os.walk(m):
                    for fn in files:
                        if fn.lower().endswith(".bin"):
                            out.append(os.path.join(root, fn))
            elif os.path.isfile(m):
                out.append(m)

    return sorted(set(out))


def load_one_fentbc(path: str) -> Tuple[int, int, List[Tuple[Tuple[float, ...], int]]]:
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC:
            raise ValueError("Invalid magic")
        (version,) = struct.unpack("<i", f.read(4))
        if version != 1:
            raise ValueError(f"Unsupported version {version}")
        obs_dim, actions = struct.unpack("<ii", f.read(8))
        if obs_dim <= 0 or actions <= 0:
            raise ValueError("Bad header")

        recs: List[Tuple[Tuple[float, ...], int]] = []
        rec_size = 4 + obs_dim * 4
        while True:
            buf = f.read(rec_size)
            if not buf:
                break
            if len(buf) != rec_size:
                break
            (a,) = struct.unpack_from("<i", buf, 0)
            obs = struct.unpack_from("<" + "f" * obs_dim, buf, 4)
            recs.append((obs, int(a)))
    return obs_dim, actions, recs


def load_fentbc_many(dataset_arg: str) -> Tuple[BCDataset, torch.Tensor, torch.Tensor]:
    files = iter_dataset_files(dataset_arg)
    if not files:
        raise ValueError("No .bin files found")

    obs_dim_ref = -1
    actions_ref = -1
    acts: List[int] = []
    obss: List[Tuple[float, ...]] = []

    ok_files = 0
    for p in files:
        try:
            obs_dim, actions, recs = load_one_fentbc(p)
        except Exception as e:
            print(f"skip {p!r}: {e}")
            continue
        if obs_dim_ref == -1:
            obs_dim_ref = obs_dim
            actions_ref = actions
        if obs_dim != obs_dim_ref or actions != actions_ref:
            print(f"skip {p!r}: obs_dim/actions mismatch ({obs_dim}/{actions}) != ({obs_dim_ref}/{actions_ref})")
            continue
        for obs, a in recs:
            obss.append(obs)
            acts.append(a)
        ok_files += 1

    if not obss:
        raise ValueError("No samples in dataset")

    # create tensors on CPU
    obs_t = torch.tensor(obss, dtype=torch.float32, device="cpu")
    act_t = torch.tensor(acts, dtype=torch.int64, device="cpu")

    # compute normalization stats on CPU
    mean = obs_t.mean(dim=0)
    std = obs_t.std(dim=0, unbiased=False)
    std = torch.clamp(std, min=1e-6)

    # normalize
    obs_t = (obs_t - mean) / std

    print(f"loaded files={ok_files}/{len(files)} samples={obs_t.shape[0]} obs_dim={obs_dim_ref} actions={actions_ref}")
    return BCDataset(obs=obs_t, act=act_t, obs_dim=obs_dim_ref, actions=actions_ref), mean, std


class PolicyMLP(nn.Module):
    def __init__(self, obs_dim: int, actions: int, hidden: int = 256, dropout: float = 0.1):
        super().__init__()
        # same parameter names as before to preserve state_dict keys
        self.fc1 = nn.Linear(obs_dim, hidden)
        self.fc2 = nn.Linear(hidden, hidden)
        self.out = nn.Linear(hidden, actions)
        self._dropout_p = float(dropout)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # per-sample layer norm (no extra parameters)
        x = F.layer_norm(x, (x.size(1),))
        h1 = F.gelu(self.fc1(x))
        h2 = F.gelu(self.fc2(h1))
        h = 0.5 * (h1 + h2)  # residual-like average
        h = F.dropout(h, p=self._dropout_p, training=self.training)
        return self.out(h)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--dataset", required=True, help="folder or semicolon-separated list of files/folders")
    p.add_argument("--out", default="fentbot_bc_model.pt")
    p.add_argument("--epochs", type=int, default=50)
    p.add_argument("--batch", type=int, default=128)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--hidden", type=int, default=256)
    p.add_argument("--dropout", type=float, default=0.1)
    p.add_argument("--device", default="cpu")
    p.add_argument("--val-split", type=float, default=0.1)
    args = p.parse_args()

    device = torch.device(args.device)

    dataset, norm_mean, norm_std = load_fentbc_many(args.dataset)

    n = dataset.obs.shape[0]
    perm = torch.randperm(n)
    n_val = int(max(1, round(n * float(args.val_split))))
    val_idx = perm[:n_val]
    tr_idx = perm[n_val:]

    tr = TensorDataset(dataset.obs[tr_idx].cpu(), dataset.act[tr_idx].cpu())
    va = TensorDataset(dataset.obs[val_idx].cpu(), dataset.act[val_idx].cpu())

    tr_loader = DataLoader(tr, batch_size=args.batch, shuffle=True, num_workers=0)
    va_loader = DataLoader(va, batch_size=args.batch, shuffle=False, num_workers=0)

    model = PolicyMLP(dataset.obs_dim, dataset.actions, hidden=args.hidden, dropout=args.dropout).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)

    best_val = 0.0

    for epoch in range(args.epochs):
        model.train()
        total = 0
        correct = 0
        total_loss = 0.0

        for xb, yb in tr_loader:
            xb = xb.to(device)
            yb = yb.to(device)
            logits = model(xb)
            loss = F.cross_entropy(logits, yb)

            opt.zero_grad(set_to_none=True)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()

            total_loss += float(loss.item()) * int(xb.shape[0])
            pred = logits.argmax(dim=1)
            total += int(xb.shape[0])
            correct += int((pred == yb).sum().item())

        train_acc = correct / max(1, total)
        train_loss = total_loss / max(1, total)

        model.eval()
        v_total = 0
        v_correct = 0
        with torch.no_grad():
            for xb, yb in va_loader:
                xb = xb.to(device)
                yb = yb.to(device)
                logits = model(xb)
                pred = logits.argmax(dim=1)
                v_total += int(xb.shape[0])
                v_correct += int((pred == yb).sum().item())
        val_acc = v_correct / max(1, v_total)

        if val_acc >= best_val:
            best_val = val_acc
            os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
            # save mean/std as lists for cross-language compatibility
            torch.save(
                {
                    "obs_dim": dataset.obs_dim,
                    "actions": dataset.actions,
                    "state_dict": model.state_dict(),
                    "hidden": int(args.hidden),
                    "norm_mean": norm_mean.tolist(),
                    "norm_std": norm_std.tolist(),
                },
                args.out,
            )

        print(
            f"epoch {epoch+1}/{args.epochs} loss={train_loss:.6f} train_acc={train_acc:.4f} val_acc={val_acc:.4f} best_val={best_val:.4f}"
        )

    print(f"saved: {args.out}")


if __name__ == "__main__":
    main()