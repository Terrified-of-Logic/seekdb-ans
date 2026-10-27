"""Deterministic complete vectors and original 64-bit VID input for native smoke."""
import struct
from pathlib import Path
import numpy as np

rng = np.random.default_rng(20261027)
n, dim = 10000, 128
vectors = rng.standard_normal((n, dim)).astype(np.float32)
ids = np.arange(n, dtype=np.int64) * 17 + 10_000_000_000
queries = rng.standard_normal((100, dim)).astype(np.float32)
with open("smoke.input", "wb") as out:
    out.write(struct.pack("<5Q", 0x315350564849, n, dim, 16, 200))
    out.write(ids.tobytes())
    out.write(vectors.tobytes())
vectors.tofile("smoke.f32")
ids.tofile("smoke.ids")
queries.tofile("smoke.queries")
gt = np.argsort(((queries[:, None, :] - vectors[None, :, :]) ** 2).sum(axis=2), axis=1)[:, :10]
ids[gt].tofile("smoke.truth")
