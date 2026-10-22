# SPDX-License-Identifier: MIT
"""Story 10-13a: C3D reading and Plug-In Gait joint centres -> RAV detection csv.

Shared by the C3D corpora of the physics bench. From the spike (kyo_impact_lib.py load / track /
speed, kyo2rav.py bones / PARENT / csv writer): mixamorig bone names, metres, Y up, 240 Hz,
'# parent=' line; the format tests/dump_clip.h reads.
"""
import os

import numpy as np

RATE = 240.0  # the csv's sample rate (Hz)


def load(path):
    """(rate_hz, labels, F x N x 4 array: x, y, z in mm, residual)."""
    import warnings

    import c3d

    with open(path, "rb") as h, warnings.catch_warnings():
        warnings.simplefilter("ignore")  # "No analog data found in file."
        r = c3d.Reader(h)
        labels = [l.strip() for l in r.point_labels]
        frames = [p[:, :4].copy() for _, p, _ in r.read_frames()]
        return float(r.point_rate), labels, np.array(frames)


def track(P, labels, name):
    """One point's F x 3 track (mm); NaN where the residual is negative (no data)."""
    i = labels.index(name)
    x = P[:, i, :3].astype(float)
    x[P[:, i, 3] < 0] = np.nan
    return x


def speed(x, rate):
    """F x 3 track in mm -> speed in m/s."""
    v = np.gradient(x, axis=0) * rate / 1000.0
    return np.linalg.norm(v, axis=1)


def subject_prefix(labels):
    """'B0371:' or '' (the prefix of the PiG pelvis origin; the first one when two people)."""
    return [l for l in labels if l.endswith("PELO")][0][:-4]


def _lerp(a, b, t):
    return a + (b - a) * t


def bones(L):
    """PiG joint centres -> mixamorig bones (mm, Vicon axes). `L(name)` gives a point's track."""
    pel, trx = L("PELO"), L("TRXO")
    B = {}
    B["Hips"] = pel
    B["Spine"] = _lerp(pel, trx, 0.25)
    B["Spine1"] = _lerp(pel, trx, 0.5)
    B["Spine2"] = _lerp(pel, trx, 0.75)
    B["Neck"] = L("C7") * 0.5 + trx * 0.5
    B["Head"] = L("HEDO")
    for s, S in (("L", "Left"), ("R", "Right")):
        B[S + "Shoulder"] = _lerp(trx, L(s + "CLO"), 0.5)
        B[S + "Arm"] = L(s + "CLO")
        B[S + "ForeArm"] = L(s + "HUO")
        B[S + "Hand"] = L(s + "RAO")
        hn = L(s + "HNO")
        B[S + "HandMiddle1"] = hn
        B[S + "HandMiddle4"] = hn + (hn - B[S + "Hand"]) * 0.5
        asi, psi = L(s + "ASI"), L(s + "PSI")
        B[S + "UpLeg"] = _lerp(_lerp(asi, psi, 0.5), L(s + "FEO"), 0.15)
        B[S + "Leg"] = L(s + "FEO")
        B[S + "Foot"] = L(s + "TIO")
        B[S + "ToeBase"] = L(s + "TOE")
        B[S + "Toe_End"] = L(s + "TOE") + (L(s + "TOE") - L(s + "HEE")) * 0.15
    return B


PARENT = {"Hips": None, "Spine": "Hips", "Spine1": "Spine", "Spine2": "Spine1", "Neck": "Spine2", "Head": "Neck"}
for _S in ("Left", "Right"):
    PARENT.update({_S + "Shoulder": "Spine2", _S + "Arm": _S + "Shoulder", _S + "ForeArm": _S + "Arm",
                   _S + "Hand": _S + "ForeArm", _S + "HandMiddle1": _S + "Hand", _S + "HandMiddle4": _S + "HandMiddle1",
                   _S + "UpLeg": "Hips", _S + "Leg": _S + "UpLeg", _S + "Foot": _S + "Leg",
                   _S + "ToeBase": _S + "Foot", _S + "Toe_End": _S + "ToeBase"})


def write_csv(path, item, B, rate):
    """Writes the detection csv of bones `B` (F x 3 tracks in mm, Vicon Z up, sampled at `rate`):
    resampled to RATE, gaps interpolated, metres, Y up, ground at the left toe's 2nd percentile
    (+1 cm). Written through a temporary file. Returns the csv's duration (s)."""
    names = list(PARENT)
    n = len(B["Hips"])
    t_src = np.arange(n) / rate
    t = np.arange(0, t_src[-1], 1 / RATE)
    cols = []
    for nm in names:
        x = B[nm]
        good = ~np.isnan(x).any(axis=1)
        if not good.any():
            raise ValueError(f"bone {nm} has no data")
        cols.append(np.stack([np.interp(t, t_src[good], x[good, k]) for k in range(3)], axis=1))
    X = np.stack(cols, axis=1) / 1000.0                  # T x bones x 3, metres
    X = X[:, :, [0, 2, 1]] * np.array([1, 1, -1])        # Vicon Z-up -> Y-up
    ground = np.nanpercentile(X[:, names.index("LeftToeBase"), 1], 2)
    X[:, :, 1] -= ground - 0.01
    tmp = path + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(f"# item={item}\n# rate_hz={RATE:g}\n# visible=0.000000,{t[-1]:.6f}\n# ref=\n")
        f.write("# parent=" + ",".join(str(-1 if PARENT[nm] is None else names.index(PARENT[nm])) for nm in names) + "\n")
        f.write("t," + ",".join(f"mixamorig:{nm}.{a}" for nm in names for a in "xyz") + "\n")
        for i in range(len(t)):
            f.write(f"{t[i]:.6f}," + ",".join(f"{v:.6f}" for v in X[i].ravel()) + "\n")
    os.replace(tmp, path)
    return float(t[-1])
