#!/usr/bin/env python3
"""A learned scorer over menu entries, from preference pairs (HRL_notes.md §5).

    python eval/scorer.py evaluate wrap_counter table_fsm mealy_detector    # held-out pairwise accuracy
    python eval/scorer.py train wrap_counter table_fsm --out eval/runs/scorer/model.pt

The data is menu_data.py's: at a state, an entry whose lists passed more
often is preferred over one whose lists passed less often. The model is the
MR reward model of gp_reward-priors (optbnn/bnn/nets/mlp.py and
optbnn/training/training.py, MRTrainer): an MLP gives each entry a reward
from its features, and two entries are compared by the Bradley-Terry
cross-entropy over their rewards. An entry is a segment of one step, so
MR's pooling over a segment is the identity. A linear model is the same with
no hidden layer; with little data it is the default.

Everything runs on the CPU and is seeded, so training is reproducible.

evaluate reports pairwise accuracy on pairs never trained on:
  - leaving out one problem at a time, training on the rest of all the
    families given;
  - leaving out one family at a time: the case of a family being learned,
    whose pairs the scorer has never seen.
Beside it, two floors that need no training: a coin, and the heuristic
"prefer the entry that uses more of the design's unused facts".
"""

import argparse
import json
import math
import os
import sys

import numpy as np
import torch
from torch import nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import menu_data  # noqa: E402

DATA = menu_data.OUT
SEED = 0


class Linear(nn.Module):
    """optbnn's Linear: weights drawn N(0, 1), scaled by 1/sqrt(n_in) in the forward pass."""

    def __init__(self, n_in, n_out):
        super().__init__()
        self.n_in = n_in
        self.W = nn.Parameter(torch.randn(n_in, n_out))
        self.b = nn.Parameter(torch.zeros(n_out))

    def forward(self, x):
        return x @ (self.W / math.sqrt(self.n_in)) + self.b


class Reward(nn.Module):
    """optbnn's MLP with one output, the entry's reward; no hidden layer is
    the linear model."""

    def __init__(self, n_in, hidden=()):
        super().__init__()
        dims = [n_in] + list(hidden)
        self.hidden = nn.ModuleList(Linear(a, b) for a, b in zip(dims, dims[1:]))
        self.out = Linear(dims[-1], 1)

    def forward(self, x):
        for layer in self.hidden:
            x = torch.relu(layer(x))
        return self.out(x).squeeze(-1)


WITH_LLM = False      # set by --with-llm: the model's log-probability of the entry as one more feature


def load(families, data_dir=None):
    return [json.load(open(os.path.join(data_dir or DATA, f"{f}.json"))) for f in families]


def row(entry):
    if WITH_LLM:
        if "logprob" not in entry:
            sys.exit("error: --with-llm needs data with the model's log-probabilities (rollouts.py)")
        return entry["features"] + [entry["logprob"]]
    return entry["features"]


def pair_arrays(datasets, keep=lambda family, tag: True):
    """Better and worse feature rows of every pair, and the (family, tag) of each."""
    better, worse, where = [], [], []
    for data in datasets:
        for i, a, b in menu_data.pairs(data):
            state = data["states"][i]
            if keep(data["family"], state["tag"]):
                better.append(row(state["entries"][a]))
                worse.append(row(state["entries"][b]))
                where.append((data["family"], state["tag"]))
    return np.array(better, dtype=np.float32), np.array(worse, dtype=np.float32), where


def fit(better, worse, hidden=(), epochs=300, lr=0.01, weight_decay=1e-3):
    """A reward model trained by Bradley-Terry cross-entropy on the pairs,
    with the features standardized by the training rows. Returns the model
    and the standardization."""
    torch.manual_seed(SEED)
    rows = np.concatenate([better, worse])
    mean, std = rows.mean(0), rows.std(0) + 1e-6
    xb = torch.tensor((better - mean) / std)
    xw = torch.tensor((worse - mean) / std)
    net = Reward(xb.shape[1], hidden)
    opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=weight_decay)
    target = torch.zeros(len(xb), dtype=torch.long)       # the first of each pair is preferred
    loss_fn = nn.CrossEntropyLoss()
    for _ in range(epochs):
        opt.zero_grad()
        loss = loss_fn(torch.stack([net(xb), net(xw)], 1), target)
        loss.backward()
        opt.step()
    net.eval()
    return net, mean, std


def accuracy(net, mean, std, better, worse):
    with torch.no_grad():
        rb = net(torch.tensor((better - mean) / std))
        rw = net(torch.tensor((worse - mean) / std))
    return float((rb > rw).float().mean())


def heuristic(better, worse):
    """Prefer the entry that uses more unused facts; a tie counts half. (With
    --with-llm the model's log-probability is the last column, after it.)"""
    k = menu_data.FEATURES.index("uses_unused")
    return float(np.mean(np.where(better[:, k] > worse[:, k], 1.0, np.where(better[:, k] == worse[:, k], 0.5, 0.0))))


def cmd_evaluate(args):
    datasets = load(args.families, args.data)
    hidden = tuple(args.hidden)
    groups = sorted({w for d in datasets for w in pair_arrays([d])[2]})
    print(f"model: {'linear' if not hidden else 'MLP ' + str(list(hidden))}; "
          f"{sum(len(menu_data.pairs(d)) for d in datasets)} pairs")
    print("\nleaving out one problem")
    print(f"{'family':16s} {'problem':36s} {'pairs':>5s} {'scorer':>7s} {'heuristic':>9s}")
    totals = []
    for fam, tag in groups:
        b, w, _ = pair_arrays(datasets, lambda f, t: (f, t) != (fam, tag))
        tb, tw, _ = pair_arrays(datasets, lambda f, t: (f, t) == (fam, tag))
        if not len(tb) or not len(b):
            continue
        net, mean, std = fit(b, w, hidden, args.epochs)
        acc, h = accuracy(net, mean, std, tb, tw), heuristic(tb, tw)
        totals.append((len(tb), acc, h))
        print(f"{fam:16s} {tag:36s} {len(tb):5d} {acc:7.2f} {h:9.2f}")
    n = sum(t[0] for t in totals)
    print(f"{'all, pair-weighted':53s} {n:5d} {sum(t[0] * t[1] for t in totals) / n:7.2f} "
          f"{sum(t[0] * t[2] for t in totals) / n:9.2f}")
    if len(datasets) > 1:
        print("\nleaving out one family")
        for data in datasets:
            fam = data["family"]
            b, w, _ = pair_arrays([d for d in datasets if d["family"] != fam])
            tb, tw, _ = pair_arrays([data])
            if not len(tb) or not len(b):
                continue
            net, mean, std = fit(b, w, hidden, args.epochs)
            print(f"{fam:16s} {len(tb):5d} pairs  scorer {accuracy(net, mean, std, tb, tw):.2f}  "
                  f"heuristic {heuristic(tb, tw):.2f}")
    print("\n(a coin scores 0.50)")


def cmd_train(args):
    datasets = load(args.families, args.data)
    b, w, _ = pair_arrays(datasets)
    net, mean, std = fit(b, w, tuple(args.hidden), args.epochs)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    torch.save({"net": net.state_dict(), "hidden": list(args.hidden), "mean": mean.tolist(), "std": std.tolist(),
                "features": menu_data.FEATURES + (["llm_logprob"] if WITH_LLM else []), "with_llm": WITH_LLM,
                "data": os.path.abspath(args.data or DATA), "families": args.families, "pairs": len(b),
                "epochs": args.epochs, "seed": SEED}, args.out)
    print(f"trained on {len(b)} pairs of {', '.join(args.families)}; training accuracy "
          f"{accuracy(net, mean, std, b, w):.2f}; written to {args.out}")


class Scorer:
    """A trained scorer: reward(features) for a menu's entries."""

    def __init__(self, path):
        saved = torch.load(path, weights_only=False)
        self.with_llm = saved.get("with_llm", False)
        if saved["features"] != menu_data.FEATURES + (["llm_logprob"] if self.with_llm else []):
            sys.exit(f"error: {path} was trained on other features")
        self.path = path
        self._usable = {}
        self.net = Reward(len(saved["features"]), tuple(saved["hidden"]))
        self.net.load_state_dict(saved["net"])
        self.net.eval()
        self.mean, self.std = np.array(saved["mean"]), np.array(saved["std"])

    def __call__(self, rows):
        x = (np.array(rows, dtype=np.float32) - self.mean) / self.std
        with torch.no_grad():
            return self.net(torch.tensor(x, dtype=torch.float32)).tolist()

    def rate(self, problem, done, menu, logprobs=None):
        """The score of each entry of a menu after `done`. A scorer trained
        with the model's log-probability as a feature needs `logprobs`."""
        tag = problem.case["tag"]
        if tag not in self._usable:
            self._usable[tag] = len(problem.unused_facts([]))
        rows = [menu_data.features(problem, list(done), e["step"], self._usable[tag]) for e in menu]
        if self.with_llm:
            if logprobs is None:
                sys.exit(f"error: {self.path} needs the model's log-probabilities")
            rows = [r + [lp] for r, lp in zip(rows, logprobs)]
        return self(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("evaluate", "train"):
        s = sub.add_parser(name)
        s.add_argument("families", nargs="+", help="families whose menu data (menu_data.py) to use")
        s.add_argument("--hidden", type=int, nargs="*", default=[], help="hidden layer sizes (default: linear)")
        s.add_argument("--epochs", type=int, default=300)
        s.add_argument("--data", help=f"where the data is (default {DATA}; rollouts.py writes eval/runs/rollouts)")
        s.add_argument("--with-llm", action="store_true",
                       help="add the model's log-probability of each entry as a feature (data from rollouts.py)")
        if name == "train":
            s.add_argument("--out", required=True)
    args = ap.parse_args()
    global WITH_LLM
    WITH_LLM = args.with_llm
    {"evaluate": cmd_evaluate, "train": cmd_train}[args.cmd](args)


if __name__ == "__main__":
    main()
