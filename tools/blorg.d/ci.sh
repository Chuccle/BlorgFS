# shellcheck shell=bash
# blorg ci test | build | guest | status | results
# Runs the real Windows build and guest tests on GitHub Actions from any
# shell with a GitHub token. Sourced by tools/blorg.

gh_ok() {
    have gh || return 1
    # A token just for this, separate from whatever the git credential is.
    [[ -n "${BLORG_GH_TOKEN:-}" ]] && export GH_TOKEN="$BLORG_GH_TOKEN"
    # Dispatching needs write access; a public repo answers reads without any.
    [[ "$(gh api "repos/$GH_REPO" --jq '.permissions.push' 2>/dev/null)" == "true" ]]
}

need_gh() {
    gh_ok && return 0
    cat >&2 <<EOF
blorg: the GitHub API is not usable from this shell (gh missing or no valid token).
  Either set a token: a fine-grained PAT on $GH_REPO with Actions:
  read/write and Contents: read, as BLORG_GH_TOKEN (or GH_TOKEN), or push
  the branch: "Build BlorgFS" builds, packages and runs the guest tests on
  it, and reports back on the PR.
EOF
    exit 2
}

cur_ref() { git -C "$REPO" rev-parse --abbrev-ref HEAD; }

# Dispatch a workflow and print the id of the run it created. build.yml
# takes a correlation_id and puts it in its run name, so its run is matched
# exactly; other workflows are matched by ref and start time.
dispatch() {
    local wf="$1" ref="$2"; shift 2
    local since id cid="" filter
    since="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    if [[ "$wf" == build.yml ]]; then
        cid="blorg-$(date +%s)-$$"
        set -- "$@" -f "correlation_id=$cid"
        filter="[.[] | select(.displayTitle | endswith(\" $cid\"))][0].databaseId // empty"
    else
        filter="[.[] | select(.createdAt >= \"$since\")][0].databaseId // empty"
    fi
    gh workflow run "$wf" -R "$GH_REPO" --ref "$ref" "$@" >&2 || return 1
    for _ in $(seq 1 30); do
        sleep 4
        id="$(gh run list -R "$GH_REPO" -w "$wf" -b "$ref" -e workflow_dispatch -L 20 \
              --json databaseId,createdAt,displayTitle --jq "$filter")"
        [[ -n "$id" ]] && { echo "$id"; return 0; }
    done
    note "dispatched $wf but could not find its run"; return 1
}

# The build.yml run for what is pushed at ref: a push or PR has usually
# started one already, and following it beats building the same commit
# twice. Dispatch only when there is none.
build_run_for() {
    local ref="$1" sha id
    sha="$(gh api "repos/$GH_REPO/commits/$ref" --jq .sha 2>/dev/null)" ||
        { note "ref '$ref' is not on GitHub; push it first"; return 1; }
    [[ "$sha" == "$(git -C "$REPO" rev-parse HEAD 2>/dev/null)" ]] ||
        note "local HEAD differs from $ref on GitHub ($(cut -c1-7 <<<"$sha")); testing what is pushed"
    id="$(gh run list -R "$GH_REPO" -w build.yml -c "$sha" -L 10 --json databaseId,status,conclusion \
          --jq '[.[] | select(.status != "completed" or .conclusion == "success" or .conclusion == "failure")][0].databaseId // empty')"
    if [[ -n "$id" ]]; then
        note "following the existing build.yml run for ${sha:0:7}"
        echo "$id"
    else
        dispatch build.yml "$ref"
    fi
}

# Wait for a run (polling every BLORG_POLL seconds, default 10); on failure
# print the tail of each failed job's log.
watch_run() {
    local id="$1" concl
    note "https://github.com/$GH_REPO/actions/runs/$id"
    until [[ "$(gh run view "$id" -R "$GH_REPO" --json status --jq .status 2>/dev/null)" == "completed" ]]; do
        sleep "${BLORG_POLL:-10}"
    done
    concl="$(gh run view "$id" -R "$GH_REPO" --json conclusion --jq .conclusion)"
    echo "run $id: ${concl:-unknown}"
    if [[ "$concl" != "success" ]]; then
        gh run view "$id" -R "$GH_REPO" --log-failed 2>/dev/null | tail -n "${BLORG_LOG_LINES:-150}" ||
            note "logs unreachable from here; failing jobs' annotations:"
        gh run view "$id" -R "$GH_REPO" --json jobs \
            --jq '.jobs[] | select(.conclusion == "failure") | "  failed: \(.name) -- \(.url)"'
        return 1
    fi
}

print_annotations() {
    local id="$1" job
    for job in $(gh run view "$id" -R "$GH_REPO" --json jobs --jq '.jobs[].databaseId' 2>/dev/null); do
        gh api "repos/$GH_REPO/check-runs/$job/annotations" \
            --jq '.[] | select(.title // "" | startswith("blorg ")) | "--- \(.title)\n\(.message)"' 2>/dev/null
    done
}

print_verdicts() {
    local dir="$1" v
    while IFS= read -r v; do echo "--- $v"; cat "$v"; done < <(find "$dir" -name verdict.txt 2>/dev/null)
}

cmd_ci() {
    local sub="${1:-}"; shift || true
    local ref; ref="$(cur_ref)"
    case "$sub" in
        build)
            need_gh
            [[ "${1:-}" == "--ref" ]] && ref="$2"
            local id; id="$(build_run_for "$ref")" || exit 2
            echo "build_run=$id"
            watch_run "$id"
            ;;
        guest)
            need_gh
            local args=() a
            while (( $# )); do
                case "$1" in
                    --package-run) args+=(-f "package_run_id=$2"); shift 2 ;;
                    --no-verifier) args+=(-f verifier=false); shift ;;
                    --ref)         ref="$2"; shift 2 ;;
                    *) die "unknown ci guest argument '$1'" ;;
                esac
            done
            local id; id="$(dispatch guest.yml "$ref" "${args[@]}")" || exit 2
            watch_run "$id"; a=$?
            cmd_ci results "$id"
            return $a
            ;;
        test)
            # The whole pipeline: build.yml builds, packages and runs the
            # guest tests as its last job. --no-verifier needs a separate
            # guest run, so it re-tests the built package via guest.yml.
            need_gh
            local nover=() bid rc
            while (( $# )); do
                case "$1" in
                    --ref)         ref="$2"; shift 2 ;;
                    --no-verifier) nover=(--no-verifier); shift ;;
                    *) die "unknown ci test argument '$1'" ;;
                esac
            done
            bid="$(build_run_for "$ref")" || exit 2
            echo "build_run=$bid"
            if (( ${#nover[@]} )); then
                watch_run "$bid" || true
                hdr "guest tests without Driver Verifier on build $bid's package"
                cmd_ci guest --ref "$ref" --package-run "$bid" "${nover[@]}"
                return
            fi
            watch_run "$bid"; rc=$?
            cmd_ci results "$bid"
            return $rc
            ;;
        status)
            need_gh
            [[ "${1:-}" == "--ref" ]] && ref="$2"
            gh run list -R "$GH_REPO" -b "$ref" -L 15 \
                --json databaseId,workflowName,status,conclusion,headSha,createdAt \
                --jq '.[] | "\(.databaseId)  \(.workflowName)  \(.status)/\(.conclusion // "-")  \(.headSha[0:7])  \(.createdAt)"'
            ;;
        results)
            need_gh
            local id="${1:?usage: blorg ci results RUN_ID}" dest="$OUT/runs/${1}"
            rm -rf "$dest"; mkdir -p "$dest"
            # Annotations come from api.github.com; artifacts and logs are
            # served from blob storage that an egress proxy may block,
            # so the workflows put their verdicts in annotations too.
            print_annotations "$id"
            if gh run download "$id" -R "$GH_REPO" -D "$dest" >/dev/null 2>&1; then
                echo "artifacts: $dest"
                ls "$dest" 2>/dev/null | sed 's/^/  /'
                print_verdicts "$dest"
            else
                note "could not download run $id's artifacts (none yet, or blob storage unreachable from here)"
            fi
            ;;
        *) die "usage: blorg ci test|build|guest|status|results" ;;
    esac
}

