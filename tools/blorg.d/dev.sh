# shellcheck shell=bash
# blorg setup | check | driver-check | server-test | doctor | container
# The Linux tier: what can be checked without Windows. Sourced by tools/blorg.

# Version of a package in src/packages.config.
pkg_version() {
    sed -n "s/.*id=\"$1\" version=\"\([^\"]*\)\".*/\1/p" "$REPO/src/packages.config" | head -1
}

# NuGet's CDN cuts long HTTP/2 transfers through some egress proxies;
# HTTP/1.1 survives them, so force it.
nuget_fetch() {
    local id="$1" ver="$2" dest="$CACHE/nuget/$1.$2"
    [[ -f "$dest/.ok" ]] && { echo "$dest"; return 0; }
    local lid="${id,,}"
    mkdir -p "$CACHE/nuget"
    note "fetching $id $ver from nuget.org"
    curl -fsSL --http1.1 --retry 4 --retry-delay 3 -o "$dest.nupkg" \
        "https://api.nuget.org/v3-flatcontainer/$lid/$ver/$lid.$ver.nupkg" || return 1
    rm -rf "$dest"; mkdir -p "$dest"
    # Headers only: the libs and tools are Windows binaries we cannot use.
    unzip -q -o "$dest.nupkg" 'c/Include/*' -d "$dest" || return 1
    rm -f "$dest.nupkg"
    touch "$dest/.ok"
    echo "$dest"
}

flatcc_bin() { echo "$REPO/third_party/flatcc/bin/flatcc"; }
flatc_bin()  { echo "$CACHE/flatc/flatc"; }

# ---------------------------------------------------------------- setup

setup_flatcc() {
    [[ -x "$(flatcc_bin)" ]] && return 0
    have cmake || { note "cmake missing; cannot build flatcc"; return 1; }
    git -C "$REPO" submodule update --init third_party/flatcc >/dev/null || return 1
    note "building flatcc (driver-side schema compiler)"
    cmake -S "$REPO/third_party/flatcc" -B "$CACHE/flatcc-build" -DCMAKE_BUILD_TYPE=Release \
        -DFLATCC_TEST=OFF ${BLORG_CMAKE_GENERATOR:+-G "$BLORG_CMAKE_GENERATOR"} >/dev/null &&
    cmake --build "$CACHE/flatcc-build" -j"$(nproc)" >/dev/null 2>&1
}

# The same codegen build.yml runs, so the driver sources see the same headers.
gen_driver_headers() {
    git -C "$REPO" submodule update --init third_party/schemas third_party/picohttpparser >/dev/null || return 1
    setup_flatcc || return 1
    mkdir -p "$REPO/generated"
    "$(flatcc_bin)" -rv --common_reader -o "$REPO/generated" "$REPO/third_party/schemas/metadata_flatbuffer.fbs"
}

setup_wdk_headers() {
    local wdk sdk
    have unzip || { note "unzip missing"; return 1; }
    wdk="$(nuget_fetch Microsoft.Windows.WDK.x64 "$(pkg_version Microsoft.Windows.WDK.x64)")" || return 1
    sdk="$(nuget_fetch Microsoft.Windows.SDK.CPP "$(pkg_version Microsoft.Windows.SDK.CPP)")" || return 1
    printf '%s\n%s\n' "$wdk" "$sdk"
}

cmd_setup() {
    local rc=0 srv
    hdr "driver: flatcc + generated headers";   gen_driver_headers >/dev/null || { note "FAILED"; rc=2; }
    hdr "driver: WDK + SDK headers (NuGet)";    setup_wdk_headers  >/dev/null || { note "FAILED"; rc=2; }
    if srv="$(server_dir)"; then
        hdr "server-rs ($srv): flatc + the Windows target the package ships"
        server_flatc "$srv" >/dev/null || { note "flatc FAILED"; rc=2; }
        if have rustup; then
            rustup target add x86_64-pc-windows-gnu >/dev/null 2>&1 || note "rustup target add failed"
        fi
    else
        note "no server-rs checkout found (third_party/server-rs, ../server-rs or \$BLORG_SERVER_RS); skipping its setup"
    fi
    echo; cmd_doctor
    return $rc
}


# Compiles every driver translation unit with clang against the WDK's own
# kernel headers: catches what a compile catches (typos, undeclared or
# misused APIs, type errors, header breakage) in seconds, with no Windows.
# It is NOT the MSVC build: no PREfast, no link, no signing, and clang's
# diagnostics differ from cl's -- Invoke-BlorgChecks on Windows stays the
# gate. Defines and include paths are read from BlorgFS.vcxproj so the two
# cannot drift apart.
# shellcheck disable=SC2120  # files come from `blorg driver-check FILE...`
cmd_driver_check() {
    have clang || die "clang missing (apt-get install clang)"
    local verbose=0 files=()
    while (( $# )); do case "$1" in -v|--verbose) verbose=1; shift ;; *) files+=("$1"); shift ;; esac; done

    gen_driver_headers >/dev/null || die "could not generate the flatcc headers"
    local paths wdk sdk
    paths="$(setup_wdk_headers)" || die "could not fetch WDK/SDK headers"
    wdk="$(sed -n 1p <<<"$paths")"; sdk="$(sed -n 2p <<<"$paths")"
    local winc sinc
    winc="$(ls -d "$wdk"/c/Include/10.* | sort -V | tail -1)"
    sinc="$(ls -d "$sdk"/c/Include/10.* | sort -V | tail -1)"

    local proj="$REPO/src/BlorgFS.vcxproj" defs incs
    defs="$(grep -o '<PreprocessorDefinitions>[^<]*' "$proj" | head -1 | sed 's/<PreprocessorDefinitions>//')"
    incs="$(grep -o '<AdditionalIncludeDirectories>[^<]*' "$proj" | head -1 | sed 's/<AdditionalIncludeDirectories>//')"
    local -a flags=(
        -fsyntax-only --target=x86_64-pc-windows-msvc
        -fms-extensions -fms-compatibility -fms-compatibility-version=19.40
        -nostdinc -isystem "$(clang -print-resource-dir)/include"
        -isystem "$winc/km/crt" -isystem "$winc/km" -isystem "$winc/shared" -isystem "$sinc/shared"
        -D_AMD64_ -DAMD64 -D_WIN64 -D_KERNEL_MODE
        # What MSVC accepts as a warning (C4028-style pointer mismatches)
        # clang makes an error; keep it visible but not fatal.
        -Wno-error=incompatible-function-pointer-types
        # Noise from WDK idioms that MSVC never warns about.
        -Wno-microsoft-anon-tag -Wno-multichar -Wno-microsoft-static-assert
    )
    local d i
    IFS=';' read -ra parts <<<"$defs"
    for d in "${parts[@]}"; do [[ -n "$d" && "$d" != %* ]] && flags+=("-D$d"); done
    IFS=';' read -ra parts <<<"$incs"
    for i in "${parts[@]}"; do
        [[ -z "$i" || "$i" == %* ]] && continue
        i="${i//\$(ProjectDir)/$REPO/src/}"; i="${i//\\//}"
        flags+=("-I$i")
    done

    if (( ${#files[@]} == 0 )); then
        mapfile -t files < <(grep -o 'ClCompile Include="[^"]*"' "$proj" | sed 's/.*="//;s/"$//;s#\\#/#g;s#^#src/#')
    fi
    # One clang per core; results are printed in project order.
    local tmp f out fail=0 nerr nwarn n=0 jobs
    tmp="$(mktemp -d)"; jobs="$(nproc)"
    for f in "${files[@]}"; do
        (( n >= jobs )) && wait -n
        (cd "$REPO" && clang "${flags[@]}" "$f" >"$tmp/$n" 2>&1) &
        n=$((n + 1))
    done
    wait
    n=0
    for f in "${files[@]}"; do
        out="$(cat "$tmp/$n")"; n=$((n + 1))
        nerr="$(grep -c 'error:' <<<"$out")"; nwarn="$(grep -c 'warning:' <<<"$out")"
        if (( nerr )); then
            fail=1; echo "FAIL $f ($nerr errors)"; grep -A3 'error:' <<<"$out" | head -40
        else
            if (( nwarn )); then echo "ok   $f ($nwarn warnings)"; else echo "ok   $f"; fi
            (( verbose )) && [[ -n "$out" ]] && echo "$out"
        fi
    done
    rm -rf "$tmp"
    return $fail
}

server_env() {
    local srv="$1"
    local flatc_dir
    flatc_dir="$(server_flatc "$srv")" || return
    export PATH="$flatc_dir:$PATH"
    # A panicking test's full backtrace buries the assertion in a terminal
    # transcript; ask for it with BLORG_RUST_BACKTRACE=1.
    export RUST_BACKTRACE="${BLORG_RUST_BACKTRACE:-0}"
}

cmd_server_test() {
    local srv rc=0
    srv="$(server_dir)" || die "no server-rs checkout"
    server_env "$srv" || return 2
    hdr "server-rs: cargo test";  (cd "$srv" && cargo test --locked -q) || rc=1
    hdr "server-rs: clippy";      (cd "$srv" && cargo clippy --locked -q --all-targets -- -D warnings) || rc=1
    # The cfg(windows) code paths, for the target the package ships.
    # Library and binaries only, so no mingw is needed for the C parts of
    # the bench dependencies.
    hdr "server-rs: clippy for x86_64-pc-windows-gnu"
    (cd "$srv" && cargo clippy --locked -q --target x86_64-pc-windows-gnu --lib --bins -- -D warnings) || rc=1
    return $rc
}

cmd_check() {
    local rc=0
    hdr "driver compile check";    cmd_driver_check || rc=1
    if server_dir >/dev/null; then cmd_server_test || rc=1; fi
    echo
    (( rc == 0 )) && echo "Linux tier: PASS" || echo "Linux tier: FAIL"
    echo "Not covered here: MSVC/PREfast build, usermode sandbox suites, driver load. Use 'blorg ci test'."
    return $rc
}

cmd_doctor() {
    yes_no() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi; }
    local srv; srv="$(server_dir 2>/dev/null || echo none)"
    echo "repo                $REPO"
    echo "server-rs           $srv"
    echo "clang               $(yes_no have clang)"
    echo "cmake               $(yes_no have cmake)"
    echo "cargo               $(yes_no have cargo)"
    echo "nuget.org           $(yes_no curl -fsS --http1.1 -m 10 -o /dev/null https://api.nuget.org/v3/index.json)"
    echo "GitHub API          $(yes_no gh_ok)"
    echo
    echo "So, from here:"
    echo "  blorg check                Linux tier (driver compile check, server-rs)"
    if gh_ok; then
        echo "  blorg ci test              real MSVC build (build.yml) + real-kernel guest tests, via GitHub Actions"
    else
        echo "  (no GitHub token)          push the branch and read CI on the PR; see 'blorg ci' for how to add a token"
    fi
}

