"""The statistics of the evaluation (LLM_RTL_code_generation.md 6.1.3).

pass@k with Chen et al.'s unbiased estimator, the syntax and functional
ladders, the exact split of a paired difference into a syntax term and a
conditional term, and the paired tests. Standard library only.
"""

import math
import random


def pass_at_k(n, c, k):
    """Unbiased pass@k for one problem: n samples, c of them passing (Chen et
    al. 2021, arXiv 2107.03374). The product form avoids the overflow of the
    binomial coefficients: 1 - prod_{i=n-c+1}^{n} (1 - k/i)."""
    if not 0 <= c <= n:
        raise ValueError(f"c={c} is not within 0..n={n}")
    if k > n:
        raise ValueError(f"pass@{k} needs at least {k} samples, not {n}")
    if n - c < k:
        return 1.0
    p = 1.0
    for i in range(n - c + 1, n + 1):
        p *= 1.0 - k / i
    return 1.0 - p


def ladder(counts, n, ks):
    """{k: mean pass@k over problems} for per-problem pass counts."""
    if not counts:
        return {k: float("nan") for k in ks}
    return {k: sum(pass_at_k(n, c, k) for c in counts) / len(counts) for k in ks}


def split(s_p, q_p, s_d, q_d):
    """The paired functional difference f_P - f_D, for one problem, split
    exactly into (syntax term, conditional term), where f = s * q: s is the
    syntax rate and q the conditional functional rate, the fraction of
    syntax-valid samples that also pass. The symmetric (Shapley) split is
    additive, so averaging each term over problems gives terms that sum to the
    benchmark-level difference.

    q is undefined for an arm with no syntax-valid sample; pass None, and it
    is taken equal to the other arm's q, which puts that problem's whole
    difference in the syntax term. With neither defined, both terms are 0."""
    if q_p is None and q_d is None:
        return 0.0, 0.0
    if q_p is None:
        q_p = q_d
    if q_d is None:
        q_d = q_p
    syntax = (s_p - s_d) * (q_p + q_d) / 2.0
    conditional = (q_p - q_d) * (s_p + s_d) / 2.0
    return syntax, conditional


def bootstrap_ci(values_by_problem, stat, resamples=2000, seed=1, level=0.95):
    """Percentile bootstrap over problems. `values_by_problem` is a list with
    one entry per problem; `stat` maps a list of such entries to a number."""
    rng = random.Random(seed)
    m = len(values_by_problem)
    if m == 0:
        return float("nan"), float("nan")
    draws = []
    for _ in range(resamples):
        sample = [values_by_problem[rng.randrange(m)] for _ in range(m)]
        draws.append(stat(sample))
    draws.sort()
    lo = draws[int(math.floor((1 - level) / 2 * resamples))]
    hi = draws[min(resamples - 1, int(math.ceil((1 + level) / 2 * resamples)) - 1)]
    return lo, hi


def mcnemar_exact(b, c):
    """Two-sided exact McNemar p-value for the discordant counts of a paired
    binary outcome: b problems pass only in arm A, c only in arm B."""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)
