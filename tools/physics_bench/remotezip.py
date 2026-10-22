# SPDX-License-Identifier: MIT
"""Story 10-13a: members of a remote zip, read through HTTP range requests (zip64 aware).

From the spike (investigations/spike-free-strike-data-2026-10-22/remotezip.py), as a module:
the zip's directory is read once and cached as json; a member is extracted to the cache only when
missing (or of the wrong size). A signed URL that expired (HTTP error) is resolved again from the
public link, three tries per member.
"""
import http.client
import json
import os
import struct
import time
import urllib.error
import urllib.request
import zlib

TIMEOUT_S = 120


class FetchError(Exception):
    pass


def _content_range(r):
    """The response's Content-Range; FetchError (before any body is read) unless it is a 206."""
    cr = r.headers.get("Content-Range")
    if r.status != 206 or not cr:
        raise FetchError(f"no range support (HTTP {r.status}, Content-Range {cr!r})")
    return cr


def resolve(url):
    """The signed URL the public link redirects to, and the zip's size."""
    req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        cr = _content_range(r)
        try:
            return r.geturl(), int(cr.split("/")[1])
        except (IndexError, ValueError):
            raise FetchError(f"bad Content-Range {cr!r}")


def _rng(url, a, b):
    req = urllib.request.Request(url, headers={"Range": f"bytes={a}-{b}"})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        _content_range(r)
        data = r.read()
    if len(data) != b - a + 1:
        raise FetchError(f"short read {len(data)} of {b - a + 1} bytes")
    return data


def _central(url, size):
    tail = _rng(url, max(0, size - 70000), size - 1)
    i = tail.rfind(b"PK\x05\x06")
    if i < 0:
        raise FetchError("no zip end record")
    cd_size, cd_off = struct.unpack("<II", tail[i + 12:i + 20])
    j = tail.rfind(b"PK\x06\x06")
    if j >= 0:
        cd_size, cd_off = struct.unpack("<QQ", tail[j + 40:j + 56])
    cd = _rng(url, cd_off, cd_off + cd_size - 1)
    out, p = [], 0
    while p < len(cd) and cd[p:p + 4] == b"PK\x01\x02":
        meth, = struct.unpack("<H", cd[p + 10:p + 12])
        csize, usize = struct.unpack("<II", cd[p + 20:p + 28])
        nl, el, cl = struct.unpack("<HHH", cd[p + 28:p + 34])
        off, = struct.unpack("<I", cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode("utf-8", "replace")
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            hid, hl = struct.unpack("<HH", extra[q:q + 4])
            body = extra[q + 4:q + 4 + hl]
            if hid == 1:
                vals = list(struct.unpack("<" + "Q" * (hl // 8), body[:hl // 8 * 8]))
                k = 0
                if usize == 0xFFFFFFFF:
                    usize = vals[k]; k += 1
                if csize == 0xFFFFFFFF:
                    csize = vals[k]; k += 1
                if off == 0xFFFFFFFF:
                    off = vals[k]; k += 1
            q += 4 + hl
        out.append(dict(name=name, meth=meth, csize=csize, usize=usize, off=off))
        p += 46 + nl + el + cl
    return out


class RemoteZip:
    """`public_url`: the stable link (figshare ndownloader); `cache_dir`: where the listing and the
    extracted members go (outside the repo)."""

    def __init__(self, public_url, cache_dir, offline=False):
        self.public_url = public_url
        self.cache_dir = cache_dir
        self.offline = offline
        self.url = None
        os.makedirs(cache_dir, exist_ok=True)
        self._entries = None

    def _signed(self, fresh=False):
        if self.url is None or fresh:
            self.url, _ = resolve(self.public_url)
        return self.url

    def entries(self):
        """The zip's members (name, meth, csize, usize, off), cached in <cache>/listing.json."""
        if self._entries is not None:
            return self._entries
        path = os.path.join(self.cache_dir, "listing.json")
        if os.path.exists(path):
            with open(path) as f:
                self._entries = json.load(f)
            return self._entries
        if self.offline:
            raise FetchError("no cached listing and --no-fetch")
        last = None
        for attempt in range(3):
            try:
                url, size = resolve(self.public_url)
                self.url = url
                ents = _central(url, size)
                break
            except (urllib.error.URLError, OSError, FetchError, struct.error, http.client.HTTPException) as e:
                last = e
                time.sleep(1 + attempt)
        else:
            raise FetchError(f"listing failed: {last}")
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(ents, f)
        os.replace(tmp, path)
        self._entries = ents
        return ents

    def local_path(self, entry):
        return os.path.join(self.cache_dir, entry["name"].replace("/", "_").replace("\\", "_"))

    def cached(self, entry):
        p = self.local_path(entry)
        return os.path.exists(p) and os.path.getsize(p) == entry["usize"]

    def extract(self, entry):
        """The member's local path; downloaded only when missing. Returns (path, downloaded);
        raises FetchError after three failed tries (the URL re-resolved before each retry)."""
        dest = self.local_path(entry)
        if self.cached(entry):
            return dest, False
        if self.offline:
            raise FetchError("not cached and --no-fetch")
        last = None
        for attempt in range(3):
            try:
                url = self._signed(fresh=attempt > 0)
                h = _rng(url, entry["off"], entry["off"] + 29)
                if h[:4] != b"PK\x03\x04":
                    raise FetchError("bad local header")
                nl, el = struct.unpack("<HH", h[26:30])
                start = entry["off"] + 30 + nl + el
                data = _rng(url, start, start + entry["csize"] - 1)
                if entry["meth"] == 8:
                    data = zlib.decompress(data, -15)
                elif entry["meth"] != 0:
                    raise FetchError(f"compression method {entry['meth']}")
                if len(data) != entry["usize"]:
                    raise FetchError(f"size {len(data)}, expected {entry['usize']}")
                tmp = dest + ".tmp"
                with open(tmp, "wb") as f:
                    f.write(data)
                os.replace(tmp, dest)
                return dest, True
            except (urllib.error.URLError, OSError, FetchError, zlib.error, http.client.HTTPException) as e:
                last = e
                time.sleep(1 + attempt)
        raise FetchError(f"{entry['name']}: {last}")
