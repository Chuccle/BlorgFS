# shellcheck shell=bash
# blorg pins | server | package -- the combined BlorgFS + server-rs package.
# Sourced by tools/blorg. The one recipe: build.yml runs these commands,
# and a package built by hand is built the same way.

SERVER_TARGETS=(linux-x64:x86_64-unknown-linux-musl:server-rs windows-x64:x86_64-pc-windows-gnu:server-rs.exe)

# Gitlink of PATH in REPO's index (mode 160000), or fail.
gitlink() {
    local line
    line="$(git -C "$1" ls-files -s -- "$2")" || return 1
    [[ "$line" == 160000\ * ]] || { note "'$2' is not a submodule in $1"; return 1; }
    awk '{print $2}' <<<"$line"
}

# blorg pins: the driver's schemas pin must equal the schemas pin inside
# the pinned server-rs commit, or the two sides compile different schemas
# and only a mounted volume shows it. Reads the index, not the working tree,
# so it answers for what is committed. Prints "server_rs schemas".
cmd_pins() {
    local srv="$REPO/third_party/server-rs" server_pin driver_schemas server_schemas
    [[ -e "$srv/.git" ]] || die "third_party/server-rs is not checked out (git submodule update --init third_party/server-rs)"
    server_pin="$(gitlink "$REPO" third_party/server-rs)" || exit 2
    driver_schemas="$(gitlink "$REPO" third_party/schemas)" || exit 2
    server_schemas="$(git -C "$srv" ls-tree "$server_pin" -- schemas | awk '{print $3}')"
    [[ -n "$server_schemas" ]] || die "cannot read schemas at server-rs $server_pin (is that commit fetched?)"
    note "server-rs $server_pin; schemas: driver $driver_schemas, server $server_schemas"
    if [[ "$driver_schemas" != "$server_schemas" ]]; then
        local msg="schema mismatch: the driver builds schemas $driver_schemas, the pinned server-rs ($server_pin) builds $server_schemas. Bump third_party/schemas and third_party/server-rs together."
        [[ -n "${GITHUB_ACTIONS:-}" ]] && echo "::error title=Package pins::$msg"
        echo "blorg: $msg" >&2
        return 1
    fi
    echo "$server_pin $driver_schemas"
}

# flatc built from server-rs's own buildtools/flatbuffers submodule: its
# build.rs runs flatc, and the generated code must match its flatbuffers
# crate. Cached by submodule commit. Prints the directory holding flatc.
server_flatc() {
    local srv="$1" rev dir
    rev="$(git -C "$srv" rev-parse HEAD:buildtools/flatbuffers 2>/dev/null)" || die "no buildtools/flatbuffers in $srv"
    dir="$CACHE/flatc-${rev:0:12}"
    if [[ ! -x "$dir/flatc" ]]; then
        have cmake || die "cmake missing; cannot build flatc"
        git -C "$srv" submodule update --init --recursive >/dev/null || die "submodule update failed in $srv"
        note "building flatc from $srv/buildtools/flatbuffers"
        cmake -S "$srv/buildtools/flatbuffers" -B "$dir-build" -DCMAKE_BUILD_TYPE=Release \
            -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF >/dev/null &&
            cmake --build "$dir-build" --target flatc -j"$(nproc)" >/dev/null 2>&1 || die "flatc build failed"
        mkdir -p "$dir" && cp "$dir-build/flatc" "$dir/" && rm -rf "$dir-build"
    fi
    echo "$dir"
}

# blorg server [--out DIR] [linux-x64|windows-x64 ...]: cross-builds the
# pinned server-rs (cargo build --release --locked, default features) into
# DIR/<platform>/. Default: both platforms into out/server.
#
# A clean server-rs checkout's binaries are kept in the cache, keyed by its
# commit, the target and the compiler, so an unchanged pin is a copy rather
# than a build. A checkout with local changes is always built.
cmd_server() {
    local out="$OUT/server" want=() srv flatc="" t plat triple bin rev="" key
    while (( $# )); do
        case "$1" in
            --out) out="$2"; shift 2 ;;
            *) want+=("$1"); shift ;;
        esac
    done
    srv="$REPO/third_party/server-rs"
    [[ -f "$srv/Cargo.toml" ]] || die "third_party/server-rs is not checked out (git submodule update --init --recursive third_party/server-rs)"
    [[ -z "$(git -C "$srv" status --porcelain --untracked-files=no 2>/dev/null)" ]] &&
        rev="$(git -C "$srv" rev-parse HEAD 2>/dev/null)"
    for t in "${SERVER_TARGETS[@]}"; do
        IFS=: read -r plat triple bin <<<"$t"
        (( ${#want[@]} == 0 )) || [[ " ${want[*]} " == *" $plat "* ]] || continue
        mkdir -p "$out/$plat"
        key=""
        [[ -n "$rev" ]] && key="$CACHE/server-rs/${rev:0:12}-$triple-$(cd "$srv" && rustc -V | sha256sum | cut -c1-8)"
        if [[ -n "$key" && -f "$key/$bin" ]]; then
            note "server-rs for $plat: cached build of ${rev:0:12}"
            cp "$key/$bin" "$out/$plat/"
            continue
        fi
        hdr "server-rs for $plat ($triple)"
        [[ -n "$flatc" ]] || flatc="$(server_flatc "$srv")"
        (cd "$srv" && rustup target add "$triple" >/dev/null 2>&1) || true
        (cd "$srv" && PATH="$flatc:$PATH" cargo build --release --locked --target "$triple") || return 1
        cp "$srv/target/$triple/release/$bin" "$out/$plat/"
        [[ -n "$key" ]] && mkdir -p "$key" && cp "$srv/target/$triple/release/$bin" "$key/"
    done
    note "servers in $out"
}

# The driver build file NAME from the first of DIRS that has it, any case:
# Inf2Cat writes blorgfs.cat, and the package may be assembled on Linux.
find_build_file() {
    local name="$1" d hit; shift
    for d in "$@"; do
        [[ -d "$d" ]] || continue
        hit="$(find "$d" -maxdepth 1 -type f -iname "$name" | head -1)"
        [[ -n "$hit" ]] && { echo "$hit"; return 0; }
    done
    return 1
}

# blorg package --driver DIR [--servers DIR] [--out DIR] [--release]
#   --driver   the driver build output, x64\Release as msbuild left it
#              (BlorgFS.sys/.cer there, the staged INF/catalog in BlorgFS\)
#   --servers  blorg server's output (default out/server)
#   --release  use VERSION as is (a tagged release); otherwise the version
#              carries both commits, e.g. 0.1.0+g417fc40.s2fa2955
# Prints the version. Layout: manifest.json, driver/, server/<platform>/.
cmd_package() {
    local driver="" servers="$OUT/server" out="$OUT/package" release=0
    while (( $# )); do
        case "$1" in
            --driver)  driver="$2"; shift 2 ;;
            --servers) servers="$2"; shift 2 ;;
            --out)     out="$2"; shift 2 ;;
            --release) release=1; shift ;;
            *) die "unknown package argument '$1'" ;;
        esac
    done
    [[ -d "$driver" ]] || die "usage: blorg package --driver <x64/Release> [--servers DIR] [--out DIR] [--release]"
    have openssl || die "openssl missing"
    local pins server_pin schemas base commit version
    pins="$(cmd_pins)" || return 1
    read -r server_pin schemas <<<"$pins"
    base="$(tr -d '[:space:]' < "$REPO/VERSION")"
    [[ "$base" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be MAJOR.MINOR.PATCH, got '$base'"
    commit="$(git -C "$REPO" rev-parse HEAD)"
    if (( release )); then version="$base"; else version="$base+g${commit:0:7}.s${server_pin:0:7}"; fi

    [[ -e "$out" && -n "$(ls -A "$out" 2>/dev/null)" ]] && die "$out is not empty"
    mkdir -p "$out/driver" "$out/server"
    local name src d1 d2
    for name in BlorgFS.sys:BlorgFS:. BlorgFS.inf:BlorgFS BlorgFS.cat:BlorgFS BlorgFS.cer:.; do
        IFS=: read -r name d1 d2 <<<"$name"
        src="$(find_build_file "$name" "$driver/$d1" ${d2:+"$driver/$d2"})" ||
            die "$name not found in $driver (was the driver built?)"
        cp "$src" "$out/driver/$name"
    done
    cp "$REPO/deploy/Install-BlorgFS.ps1" "$REPO/deploy/Uninstall-BlorgFS.ps1" "$out/driver/"
    local t plat triple bin targets=()
    for t in "${SERVER_TARGETS[@]}"; do
        IFS=: read -r plat triple bin <<<"$t"
        [[ -f "$servers/$plat/$bin" ]] || die "no $plat server at $servers/$plat/$bin (blorg server)"
        mkdir -p "$out/server/$plat"
        cp "$servers/$plat/$bin" "$out/server/$plat/"
        targets+=("$plat=$triple")
    done
    chmod +x "$out/server/linux-x64/server-rs"

    local cer="$out/driver/BlorgFS.cer" fmt=DER
    openssl x509 -inform DER -in "$cer" -noout 2>/dev/null || fmt=PEM
    BLORG_SUBJECT="$(openssl x509 -inform "$fmt" -in "$cer" -noout -subject -nameopt RFC2253 | sed 's/^subject=//')" \
    BLORG_THUMB="$(openssl x509 -inform "$fmt" -in "$cer" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g')" \
    python3 - "$out" "$version" "$commit" "$server_pin" "$schemas" \
        "$(sed -n '/^\[package\]/,/^\[/s/^version *= *"\(.*\)"/\1/p' "$REPO/third_party/server-rs/Cargo.toml" | head -1)" \
        "${targets[@]}" <<'PY'
import hashlib, json, os, sys
out, version, commit, server, schemas, crate, *targets = sys.argv[1:]
files = {}
for root, _, names in os.walk(out):
    for n in names:
        p = os.path.join(root, n)
        rel = os.path.relpath(p, out).replace(os.sep, "/")
        with open(p, "rb") as f:
            files[rel] = hashlib.sha256(f.read()).hexdigest()
manifest = {
    "schema": 1, "name": "blorg", "version": version,
    "components": {
        "blorgfs": {"commit": commit, "configuration": "Release", "platform": "x64", "signing": "test",
                    "signer": {"subject": os.environ["BLORG_SUBJECT"], "thumbprint": os.environ["BLORG_THUMB"].upper()}},
        "server_rs": {"commit": server, "version": crate, "targets": dict(t.split("=", 1) for t in targets)},
        "schemas": {"commit": schemas},
    },
    "files": dict(sorted(files.items())),
}
if os.environ.get("GITHUB_ACTIONS"):
    manifest["ci"] = {"run": f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}",
                      "ref": os.environ.get("GITHUB_REF")}
with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
    note "packaged blorg $version into $out"
    echo "$version"
}
