#!/usr/bin/env python3
"""Generate a model header from an exported on-board model folder (norm.json, targets.json, export.json, onboard.espdl).

  python3 tools/gen_ml_params.py ml/onboard main/ml_params.h                    CarMind E.V, names ML_*
  python3 tools/gen_ml_params.py ml/onboard/rev main/ml_params_rev.h MLR        CarMind R.E.V, names MLR_*

The header carries everything the firmware needs to build the model input and read its outputs exactly as the
model was trained: channel order, normalisation, which channels the model ever saw valid, window length, class
names and how to de-standardise the estimates. Re-run it whenever a model folder changes; CI checks both headers.

A model that also outputs "z" (the 64-value feature both heads read) gets an R.E.V section: the factory fuel head w0,
read from the .espdl itself (see est_fuel_row), and a prior id that ties learned state to this exact file.
"""
import hashlib
import json
import os
import re
import struct
import sys


def c_float(x):
    t = f"{float(x):.9g}"
    if not any(ch in t for ch in ".en"):          # "1" -> "1.0": "1f" is not a floating literal in C++
        t += ".0"
    return t + "f"


def c_floats(v):
    return ", ".join(c_float(x) for x in v)


class Espdl:
    """Minimal reader for ESP-DL's EDL2 container: a 16-byte header ("EDL2", flags, uint64 length) and a FlatBuffer
    following managed_components/espressif__esp-dl/fbs_loader/espdl.fbs. Only the fields used here are decoded."""

    MODEL_GRAPH = 7                                          # table Model: field index of `graph`
    GRAPH_INITIALIZER, GRAPH_OUTPUT, GRAPH_TEST_OUT = 2, 5, 9
    TENSOR_DIMS, TENSOR_TYPE, TENSOR_NAME, TENSOR_RAW, TENSOR_EXP = 0, 1, 6, 8, 13
    VALUEINFO_NAME = 0
    INT8, INT16, INT64 = 3, 5, 7

    def __init__(self, data):
        if data[:4] != b"EDL2":
            raise ValueError("not an EDL2 .espdl file")
        n = struct.unpack_from("<Q", data, 8)[0]
        self.b = data[16:16 + n]
        g = self._table(self._deref(self._table(self._deref(0))(self.MODEL_GRAPH)))
        self.outputs = [self._string(self._table(t)(self.VALUEINFO_NAME)) for t in self._tables(g(self.GRAPH_OUTPUT))]
        self.init = {t["name"]: t for t in map(self._tensor, self._tables(g(self.GRAPH_INITIALIZER)))}
        self.test_out = {t["name"]: t for t in map(self._tensor, self._tables(g(self.GRAPH_TEST_OUT)))} if g(self.GRAPH_TEST_OUT) else {}

    def _deref(self, p):
        return p + struct.unpack_from("<I", self.b, p)[0]

    def _table(self, o):
        vt = o - struct.unpack_from("<i", self.b, o)[0]
        vlen = struct.unpack_from("<H", self.b, vt)[0]

        def field(i):
            if 4 + 2 * i >= vlen:
                return None
            off = struct.unpack_from("<H", self.b, vt + 4 + 2 * i)[0]
            return o + off if off else None
        return field

    def _vector(self, p):
        v = self._deref(p)
        return v + 4, struct.unpack_from("<I", self.b, v)[0]

    def _tables(self, p):
        if p is None:
            return []
        s, n = self._vector(p)
        return [self._deref(s + 4 * k) for k in range(n)]

    def _string(self, p):
        s, n = self._vector(p)
        return self.b[s:s + n].decode()

    def _ints64(self, p):
        if p is None:
            return []
        s, n = self._vector(p)
        return list(struct.unpack_from(f"<{n}q", self.b, s))

    def _tensor(self, o):
        t = self._table(o)
        raw = b""
        if t(self.TENSOR_RAW):
            s, n = self._vector(t(self.TENSOR_RAW))       # [AlignedBytes]: 16 bytes per element
            raw = self.b[s:s + 16 * n]
        return {"name": self._string(t(self.TENSOR_NAME)), "dims": self._ints64(t(self.TENSOR_DIMS)),
                "type": struct.unpack_from("<i", self.b, t(self.TENSOR_TYPE))[0] if t(self.TENSOR_TYPE) else 0,
                "exp": self._ints64(t(self.TENSOR_EXP)), "raw": raw}

    def values(self, name, test=False):
        """Dequantised values of an initializer (or of a test output): integer x 2^exponent.

        The esp32s3 target stores one exponent per tensor. The esp32s31 (esp32p4 path) stores one per output channel;
        the values are output-major, so exponent k covers the k-th contiguous block of n / len(exp) values."""
        t = (self.test_out if test else self.init)[name]
        n = 1
        for d in t["dims"]:
            n *= d
        fmt = {self.INT8: "b", self.INT16: "h", self.INT64: "q"}[t["type"]]
        e = t["exp"]
        if n % len(e):
            raise ValueError(f"{name}: {n} values, {len(e)} exponents")
        block = n // len(e)
        return [v * 2.0 ** e[i // block] for i, v in enumerate(struct.unpack_from(f"<{n}{fmt}", t["raw"]))]


def est_fuel_row(m, fuel_index, z_dim):
    """The factory fuel head w0 = [W_fuel (z_dim values), bias] in standardised fuel units.

    The est layer is a Gemm z[1, z_dim] -> est[1, 3]. ESP-PPQ stores its weight as int8 [1, 1, z_dim, 3] with one power-of-2
    exponent and its bias as int64 with another; the flat weight order is output-major (all z_dim weights of output 0,
    then of output 1, ...). That order is not assumed: the file carries a test input with ESP-PPQ's expected z and est,
    and every output row recomputed from the test z must match the expected est to within 2 LSB, or this fails."""
    W, bias = m.values("est.weight"), m.values("est.bias")
    n_out = len(W) // z_dim
    rows = [W[k * z_dim:(k + 1) * z_dim] for k in range(n_out)]
    if "z" in m.test_out and "est" in m.test_out:
        z, est = m.values("z", test=True), m.values("est", test=True)
        lsb = 2.0 ** m.test_out["est"]["exp"][0]
        for k in range(n_out):
            got = sum(a * b for a, b in zip(z, rows[k])) + bias[k]
            if abs(got - est[k]) > 2 * lsb:
                raise ValueError(f"est row {k}: {got:.5f} from the test z, file says {est[k]:.5f}; weight layout not understood")
    return rows[fuel_index] + [bias[fuel_index]]


def main(src, dst, prefix="ML"):
    norm = json.load(open(os.path.join(src, "norm.json")))
    tg = json.load(open(os.path.join(src, "targets.json")))
    meta = json.load(open(os.path.join(src, "export.json")))
    espdl = open(os.path.join(src, "onboard.espdl"), "rb").read()
    digest = hashlib.sha256(espdl).hexdigest()
    version = digest[:12]
    # Learned state depends on the trunk (z) and the estimate head (W0). A classifier-only change keeps both, so
    # export.json may name the file whose learned state still applies; otherwise the id is this file's own hash.
    prior = meta.get("prior_sha256", digest)
    prior_why = ("first 4 bytes of sha256 of the model whose trunk and estimate head this file keeps (export.json prior_sha256)"
                 if "prior_sha256" in meta else "first 4 bytes of sha256(onboard.espdl): learned state belongs to this file")
    ch = norm["channels"]
    trained = norm.get("trained", [True] * len(ch))
    assert len(ch) == 10 and len(trained) == 10
    out = f"""/* Generated by tools/gen_ml_params.py from {src}. Do not edit by hand. */
#pragma once

#define ML_MODEL_VERSION   "{version}"          /* first 12 hex of sha256(onboard.espdl) */
#define ML_WINDOW          {int(norm["window"])}                        /* bins per input */
#define ML_BIN_S           {int(norm.get("bin_s", 1))}                          /* seconds per bin */
#define ML_CHANNELS        10
#define ML_IN_CHANNELS     {30 if norm.get("deltas", True) else 20}                        /* values, masks{", differences" if norm.get("deltas", True) else ""} */
#define ML_DELTAS          {1 if norm.get("deltas", True) else 0}
#define ML_N_CLASSES       {len(tg["classes"])}
#define ML_N_ESTIMATES     {len(tg["targets"])}
#define ML_QUANT           "{meta.get("quant_type", "?")}"
#define ML_TARGET_CHIP     "{meta.get("target", "?")}"

/* channel order: {", ".join(ch)} */
static const float   ML_NORM_MEAN[ML_CHANNELS] = {{{c_floats(norm["mean"])}}};
static const float   ML_NORM_STD[ML_CHANNELS]  = {{{c_floats(norm["std"])}}};
static const unsigned char ML_TRAINED[ML_CHANNELS] = {{{", ".join("1" if t else "0" for t in trained)}}};

static const char *const ML_CLASSES[ML_N_CLASSES] = {{{", ".join('"' + c + '"' for c in tg["classes"])}}};
static const char *const ML_ESTIMATES[ML_N_ESTIMATES] = {{{", ".join('"' + c + '"' for c in tg["targets"])}}};
static const float   ML_EST_MEAN[ML_N_ESTIMATES] = {{{c_floats(tg["mean"])}}};
static const float   ML_EST_STD[ML_N_ESTIMATES]  = {{{c_floats(tg["std"])}}};
"""
    m = Espdl(espdl)
    rev = ""
    if "z" in m.outputs:
        z_dim = int(meta.get("z_dim", 64))
        assert "fuel_rate_lph" in tg["targets"], "an R.E.V model needs a fuel estimate"
        w0 = est_fuel_row(m, tg["targets"].index("fuel_rate_lph"), z_dim)
        rev = f"""
/* R.E.V: the model has a third output "z", the {z_dim}-value feature both heads read. */
#define ML_Z_DIM           {z_dim}
#define ML_FUEL_INDEX      {tg["targets"].index("fuel_rate_lph")}                          /* fuel in the est output */
#define ML_PRIOR_ID        0x{prior[:8]}u                /* {prior_why} */
/* Factory fuel head, standardised units: fuel = z . W0[0..{z_dim - 1}] + W0[{z_dim}]. Dequantised from the .espdl's est layer (int8 weight x 2^e,
   int64 bias x 2^e) and checked against the file's own test vector by tools/gen_ml_params.py. */
static const float   ML_W0[ML_Z_DIM + 1] = {{{c_floats(w0)}}};
"""
    out += rev
    if prefix != "ML":
        out = re.sub(r"\bML_", prefix + "_", out)
    open(dst, "w").write(out)
    print(f"{dst}: model {version}, window {norm['window']}, trained {trained}{', z output, prior w0' if rev else ''}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
