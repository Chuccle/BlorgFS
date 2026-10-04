"""Decode IOCTL_BLORGFS_QUERY_STATISTICS dumps against src/Statistics.h.

parse.py <Statistics.h>            -> response size
parse.py <Statistics.h> <dump>...  -> one block per dump
"""
import os
import re
import struct
import sys

hdr = open(sys.argv[1], encoding="utf-8-sig").read()
names = ("BLORGFS_STATISTICS_LATENCY_BUCKETS", "BLORGFS_SLOW_FETCH_PER_CPU", "BLORGFS_SLOW_FETCH_SAMPLES")
consts = {k: int(re.search(r"#define\s+" + k + r"\s+(\d+)", hdr).group(1)) for k in names}
body = re.search(r"typedef struct _BLORGFS_STATISTICS\s*\{(.*?)\}\s*BLORGFS_STATISTICS,", hdr, re.S).group(1)
body = re.sub(r"//.*", "", body)
fields, off = [], 0
for m in re.finditer(r"(ULONG64|LONG64|BLORGFS_SLOW_FETCH)\s+(\w+)((?:\[\w+\])*)\s*;", body):
    t, n, dims = m.groups()
    cnt = 1
    for d in re.findall(r"\[(\w+)\]", dims):
        cnt *= int(d) if d.isdigit() else consts[d]
    size = 88 if t == "BLORGFS_SLOW_FETCH" else 8
    fields.append((n, off, cnt if dims else 0))
    off += size * cnt
resp_size = 40 + off + 8 + 8 + 88 * consts["BLORGFS_SLOW_FETCH_SAMPLES"]
if len(sys.argv) == 2:
    print(resp_size)
    sys.exit()


def read(path):
    d = open(path, "rb").read()
    ver, size, cpus, flags, freq, epoch, now = struct.unpack_from("<IIIIqqq", d, 0)
    o = {"_window_s": (now - epoch) / freq if freq else 0, "_flags": flags}
    for n, fo, cnt in fields:
        if n == "SlowFetches":
            continue
        if cnt:
            o[n] = list(struct.unpack_from("<%dQ" % cnt, d, 40 + fo))
        else:
            o[n] = struct.unpack_from("<Q", d, 40 + fo)[0]
    return o


def pct(b, q):
    total = sum(b)
    if not total:
        return 0
    acc = 0
    for i, v in enumerate(b):
        acc += v
        if acc >= q * total:
            return (1 << i) / 1000.0
    return (1 << (len(b) - 1)) / 1000.0


def mean(s, n):
    return s / n / 1000.0 if n else 0.0


for path in sys.argv[2:]:
    t = read(path)
    name = os.path.basename(path)[:-4]
    print(f"--- {name}: window {t['_window_s']:.1f}s{' CHECKED BUILD' if t['_flags'] & 1 else ''}")
    f = t["FetchesCompleted"]
    if t["FetchesIssued"]:
        print(f"  fetches {t['FetchesIssued']} ok {f} failed {t['FetchesFailed']}, {t['FetchBytes']/2**20:.0f} MiB, avg {t['FetchBytes']/max(f,1)/1024:.0f} KiB;"
              f" paging demand {t['ReadsDemand']} spec {t['ReadsSpeculative']} seq {t['ReadsSequential']}; cached reads {t['ReadsCached']}, posted {t['ReadsPosted']}")
        print(f"  fetch ms mean {mean(t['FetchLatencySumUs'], f):.2f} p50<={pct(t['FetchLatencyBuckets'], .5):.2f} p99<={pct(t['FetchLatencyBuckets'], .99):.2f} max {t['FetchLatencyMaxUs']/1000:.1f};"
              f" demand mean {mean(t['DemandLatencySumUs'], t['ReadsDemand']):.2f} max {t['DemandLatencyMaxUs']/1000:.1f}; spec mean {mean(t['SpeculativeLatencySumUs'], t['ReadsSpeculative']):.2f}")
        s = t["FetchSplitSamples"]
        if s:
            print(f"  phases ms (n={s}): presend {mean(t['FetchPreSendSumUs'], s):.3f} acquire {mean(t['FetchAcquireSumUs'], s):.3f}"
                  f" send {mean(t['FetchSendSumUs'], s):.3f} (submit {mean(t['FetchSendSubmitSumUs'], s):.3f} settle {mean(t['FetchSendSettleSumUs'], s):.3f})"
                  f" wait {mean(t['FetchWaitSumUs'], s):.3f} ttfb {mean(t['FetchTtfbSumUs'], s):.3f} body {mean(t['FetchBodySumUs'], s):.3f}")
        print(f"  conns pooled {t['ConnectionsPooled']} fresh {t['ConnectionsFresh']} (fresh acquire mean {mean(t['FetchFreshAcquireSumUs'], t['FetchFreshConnects']):.2f} ms)"
              f" closed-pool-full {t['ConnectionsClosedPoolFull']} keepalive-retries {t['KeepAliveRetries']} timeouts {t['SocketTimeouts']};"
              f" readahead grows {t['ReadAheadGrows']} shrinks {t['ReadAheadShrinks']} windows {t['ReadAdaptWindows']}")
    if t["UserReadSamples"]:
        print(f"  app reads (IRP path) {t['UserReadSamples']} mean {mean(t['UserReadLatencySumUs'], t['UserReadSamples']):.3f} ms"
              f" p50<={pct(t['UserReadLatencyBuckets'], .5):.3f} p99<={pct(t['UserReadLatencyBuckets'], .99):.3f} max {t['UserReadLatencyMaxUs']/1000:.1f}")
    meta = t["FileInfoRequests"] + t["DirInfoRequests"]
    if meta or t["SuccessfulCreates"]:
        print(f"  creates ok {t['SuccessfulCreates']} failed {t['FailedCreates']}; pathcache hit {t['PathCacheHits']} miss {t['PathCacheMisses']};"
              f" fileinfo GETs {t['FileInfoRequests']} mean {mean(t['FileInfoLatencySumUs'], t['FileInfoRequests']):.2f} ms;"
              f" dirinfo GETs {t['DirInfoRequests']} mean {mean(t['DirInfoLatencySumUs'], t['DirInfoRequests']):.2f} ms; fsp posts {t['FspPosts']}")
