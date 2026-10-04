#!/usr/bin/env bash
# Read-path profile of the packaged driver in the CI guest, under three
# emulated links. Run as guest.yml's dispatched script:
#   bash tools/profiling/profile.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${SESSION_OUT:-$PWD/prof-out}"
mkdir -p "$out"
root="$(pgrep -a server-rs | awk '{print $NF}' | head -1)"
[[ -d "$root" ]] || { echo "no server-rs corpus found"; exit 1; }
size="$(python3 "$here/parse.py" src/Statistics.h)"
ver="$(sed -n 's/^#define BLORGFS_STATISTICS_VERSION \([0-9]*\).*/\1/p' src/Statistics.h)"
echo "response size $size version $ver; host $(nproc) cpus; $(uname -r)"

echo "== Benchmark hygiene: Driver Verifier off, reboot"
blorg guest ssh 'verifier /reset | Out-Null; exit 0'
blorg guest reboot
# The service is demand-start: Install-BlorgFS.ps1 starts it, a reboot does not.
blorg guest ssh 'sc.exe start BlorgFS | Out-Null; for ($i = 0; $i -lt 60 -and -not (Test-Path B:/); $i++) { Start-Sleep 1 }; exit 0'
blorg guest ps "$here/Hygiene.ps1"

mkcorpus() {
    local d="$root/prof/$1"
    mkdir -p "$d/small" "$d/small2"
    truncate -s 512M "$d/big.bin" "$d/m1.bin" "$d/m2.bin" "$d/m3.bin" "$d/paced2.bin"
    truncate -s 256M "$d/unb.bin" "$d/raw.bin" "$d/paced.bin" "$d/c0.bin" "$d/c1.bin" "$d/c2.bin" "$d/c3.bin"
    truncate -s 1G "$d/rand.bin"
    for i in $(seq 0 299); do head -c 16384 /dev/urandom > "$d/small/f$i.bin"; done
    for i in $(seq 0 99); do head -c 16384 /dev/urandom > "$d/small2/f$i.bin"; done
}

shape() {
    sudo tc qdisc del dev lo root 2>/dev/null || true
    [[ "$1" == none ]] && return 0
    local delay="$2" rate="$3"
    sudo modprobe sch_netem 2>/dev/null || true
    sudo tc qdisc add dev lo root handle 1: prio bands 4 priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 &&
    sudo tc qdisc add dev lo parent 1:4 handle 40: netem delay "$delay" rate "$rate" limit 100000 &&
    sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport 18080 0xffff flowid 1:4 &&
    sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport 18080 0xffff flowid 1:4
}

hostbase() {
    python3 - "$1" <<'PY'
import http.client, time, random, sys
p = "%5Cprof%5C" + sys.argv[1] + "%5Craw.bin"
c = http.client.HTTPConnection("127.0.0.1", 18080)
def get(off, n):
    c.request("GET", f"/get_file?path={p}", headers={"Range": f"bytes={off}-{off+n-1}", "Connection": "keep-alive"})
    r = c.getresponse(); b = r.read(); assert r.status == 206 and len(b) == n, (r.status, len(b))
for name, n, count, rnd in (("host-random-64k", 65536, 300, True), ("host-serial-128k", 131072, 2048, False), ("host-serial-1m", 1 << 20, 256, False)):
    lat = []; t0 = time.perf_counter(); rng = random.Random(1)
    for i in range(count):
        off = rng.randrange(0, (256 << 20) // n) * n if rnd else i * n
        t = time.perf_counter(); get(off, n); lat.append((time.perf_counter() - t) * 1000)
    el = time.perf_counter() - t0; lat.sort()
    q = lambda f: lat[min(len(lat) - 1, int(f * len(lat)))]
    print(f"{name:26} {count*n/2**20:8.1f} MiB {el:7.2f} s {count*n/2**20/el:8.1f} MiB/s | n={len(lat)} mean={sum(lat)/len(lat):.3f}ms p50={q(.5):.3f} p90={q(.9):.3f} p99={q(.99):.3f} max={lat[-1]:.3f}")
PY
}

run_profile() {
    local name="$1"; shift
    echo; echo "===== PROFILE $name ($*) ====="
    mkcorpus "$name"
    if ! shape "$@"; then echo "tc shaping failed; skipping $name"; shape none; return; fi
    tc -s qdisc show dev lo | head -4
    hostbase "$name" | tee "$out/$name-host.txt"
    blorg guest ps "$here/Profile-Guest.ps1" "$name" "$size" "$ver" || echo "guest script failed for $name"
    shape none
    blorg guest pull "C:/blorgfs-ci/prof/$name" "$out/raw-$name" || true
}

run_profile loopback none
run_profile lan  shaped 1ms 1gbit
[[ -n "${WITH_WAN:-}" ]] && run_profile wan shaped 15ms 200mbit

echo
echo "===== PROFILE RESULTS ====="
for name in loopback lan ${WITH_WAN:+wan}; do
    echo "### $name"
    cat "$out/$name-host.txt" 2>/dev/null
    t="$(find "$out/raw-$name" -name timings.txt 2>/dev/null | head -1)"
    [[ -n "$t" ]] && cat "$t"
    python3 "$here/parse.py" src/Statistics.h $(find "$out/raw-$name" -name '*.bin' 2>/dev/null | sort)
done
echo "===== END PROFILE RESULTS ====="
