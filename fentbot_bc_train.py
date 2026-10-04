import argparse
import os
import struct
from dataclasses import dataclass
from typing import Iterable, List, Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, TensorDataset


MAGIC = b"FENTBC01"


@dataclass
class BCDataset:
    obs: torch.Tensor
    act: torch.Tensor
    obs_dim: int
    actions: int


def iter_dataset_files(dataset_arg: str) -> List[str]:
    if os.path.isdir(dataset_arg):
        out: List[str] = []
        for root, _, files in os.walk(dataset_arg):
            for fn in files:
                if fn.lower().endswith(".bin"):
                    out.append(os.path.join(root, fn))
        out.sort()
        return out
    if any(sep in dataset_arg for sep in [";", ","]):
        parts = [p.strip() for p in dataset_arg.replace(",", ";").split(";") if p.strip()]
        return parts
    return [dataset_arg]


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


def load_fentbc_many(dataset_arg: str, device: torch.device) -> BCDataset:
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
        except Exception:
            continue
        if obs_dim_ref == -1:
            obs_dim_ref = obs_dim
            actions_ref = actions
        if obs_dim != obs_dim_ref or actions != actions_ref:
            continue
        for obs, a in recs:
            obss.append(obs)
            acts.append(a)
        ok_files += 1

    if not obss:
        raise ValueError("No samples in dataset")

    obs_t = torch.tensor(obss, dtype=torch.float32, device=device)
    act_t = torch.tensor(acts, dtype=torch.int64, device=device)
    print(f"loaded files={ok_files}/{len(files)} samples={obs_t.shape[0]} obs_dim={obs_dim_ref} actions={actions_ref}")
    return BCDataset(obs=obs_t, act=act_t, obs_dim=obs_dim_ref, actions=actions_ref)


class PolicyMLP(nn.Module):
    def __init__(self, obs_dim: int, actions: int, hidden: int = 256):
        super().__init__()
        self.fc1 = nn.Linear(obs_dim, hidden)
        self.fc2 = nn.Linear(hidden, hidden)
        self.out = nn.Linear(hidden, actions)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = F.relu(self.fc1(x))
        x = F.relu(self.fc2(x))
        return self.out(x)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--dataset", required=True)
    p.add_argument("--out", default="fentbot_bc_model.pt")
    p.add_argument("--epochs", type=int, default=10)
    p.add_argument("--batch", type=int, default=4096)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--hidden", type=int, default=256)
    p.add_argument("--device", default="cpu")
    p.add_argument("--val-split", type=float, default=0.02)
    args = p.parse_args()

    device = torch.device(args.device)
    data = load_fentbc_many(args.dataset, device)

    n = data.obs.shape[0]
    perm = torch.randperm(n, device=device)
    n_val = int(max(1, round(n * float(args.val_split))))
    val_idx = perm[:n_val]
    tr_idx = perm[n_val:]

    tr = TensorDataset(data.obs[tr_idx].cpu(), data.act[tr_idx].cpu())
    va = TensorDataset(data.obs[val_idx].cpu(), data.act[val_idx].cpu())

    tr_loader = DataLoader(tr, batch_size=args.batch, shuffle=True, num_workers=0)
    va_loader = DataLoader(va, batch_size=args.batch, shuffle=False, num_workers=0)

    model = PolicyMLP(data.obs_dim, data.actions, hidden=args.hidden).to(device)
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
            torch.save(
                {
                    "obs_dim": data.obs_dim,
                    "actions": data.actions,
                    "state_dict": model.state_dict(),
                    "hidden": int(args.hidden),
                },
                args.out,
            )

        print(
            f"epoch {epoch+1}/{args.epochs} loss={train_loss:.6f} train_acc={train_acc:.4f} val_acc={val_acc:.4f} best_val={best_val:.4f}"
        )

    print(f"saved: {args.out}")


if __name__ == "__main__":
    main()
