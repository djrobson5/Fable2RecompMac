# mac_common.sh -- shared by build_toolchains.sh, setup_mac.sh and package_app.sh (sourced; bash 3.2 and up).
#
# Homebrew's folders go on PATH when they are missing (a launcher starts the scripts with a bare PATH);
# a PATH that already has them keeps its order.
for _d in /usr/local/bin /opt/homebrew/bin; do
    case ":$PATH:" in *":$_d:"*) ;; *) PATH=$_d:$PATH ;; esac
done
unset _d
export PATH
export GIT_TERMINAL_PROMPT=0

# ---- events for a launcher ----------------------------------------------------------------------------
# With --progress json or FABLE2_PROGRESS=json the scripts also print machine-readable lines, `@fable2 {json}`,
# among their usual ones. One JSON object per line:
#   {"stage": id, "title": text}                       a step begins
#   {"progress": done, "total": n, "eta": s, "note": text}
#   {"error": code, "message": text, "log": path}      sent once, before a non-zero exit
#   {"result": "ok", ...}                              the last line of a run that worked
# Without the flag or the variable nothing is printed that was not printed before.
PROGRESS_JSON=0
[ "${FABLE2_PROGRESS:-}" = json ] && PROGRESS_JSON=1
ERROR_EMITTED=0
DEFAULT_ERROR=error      # the code for a stop that fail() did not name (a script sets its own per stage)
LAST_DIE=

set_progress_mode() {   # text | json
    case "$1" in
    json) PROGRESS_JSON=1; export FABLE2_PROGRESS=json ;;
    text) PROGRESS_JSON=0; unset FABLE2_PROGRESS ;;
    *) die "--progress takes text or json" ;;
    esac
}

json_str() {   # a JSON string from the arguments (bytes that are not valid UTF-8 are dropped)
    local s="$*" clean
    if clean=$(printf '%s' "$s" | iconv -c -f UTF-8 -t UTF-8 2>/dev/null); then s=$clean; fi
    # (iconv -c keeps a sequence cut off at the very end: if what is left still fails, keep only ASCII)
    printf '%s' "$s" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 || s=$(printf '%s' "$s" | LC_ALL=C tr -cd '\001-\177')
    s=${s//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\r'/\\r}
    s=${s//$'\t'/\\t}
    s=${s//[[:cntrl:]]/ }
    printf '"%s"' "$s"
}
json_bool() { if [ "$1" = 1 ]; then printf true; else printf false; fi; }
json_list() {   # json_list a b c -> ["a", "b", "c"]
    local out= item
    for item in "$@"; do out="$out${out:+, }$(json_str "$item")"; done
    printf '[%s]' "$out"
}

event() {   # event '{"result": "ok"}'
    if [ "$PROGRESS_JSON" = 1 ]; then printf '@fable2 %s\n' "$1"; fi
    return 0
}
stage_event() { event "{\"stage\": $(json_str "$1"), \"title\": $(json_str "$2")}"; }
progress_event() {   # done total [eta seconds] [note]
    local line="{\"progress\": $1, \"total\": $2"
    [ -n "${3:-}" ] && line="$line, \"eta\": $3"
    [ -n "${4:-}" ] && line="$line, \"note\": $(json_str "$4")"
    event "$line}"
}
error_event() {   # code message [log]
    local line="{\"error\": $(json_str "$1"), \"message\": $(json_str "$2")"
    [ -n "${3:-}" ] && line="$line, \"log\": $(json_str "$3")"
    ERROR_EMITTED=1
    # a script that runs this one (setup_mac.sh) names a file here, to learn that an error event was sent
    [ -z "${FABLE2_ERROR_FLAG:-}" ] || : > "$FABLE2_ERROR_FLAG" 2>/dev/null || true
    event "$line}"
}

step() { printf '==> %s\n' "$*"; }
die() { LAST_DIE="$*"; printf 'error: %s\n' "$*" >&2; exit 1; }

# fail <code> <message> [log]: the error event (the code becomes disk_full when the log says so), then die.
fail() {
    local code=$1 message=$2 log=${3:-}
    if [ -n "$log" ] && [ -f "$log" ] && grep -qE "No space left on device|is the disk full" "$log" 2>/dev/null; then
        code=disk_full
        message="the disk is full ($message)"
    fi
    error_event "$code" "$message" "$log"
    die "$message"
}

# ---- children, cancel -----------------------------------------------------------------------------------
# bash runs a trap only when its foreground command ends, so anything that can run long goes through run_child:
# the child gets its own process group and the script waits for it with `wait`, which a signal interrupts. A
# signal (TERM, INT, HUP) is a cancel: the child's group is stopped, the script's cleanup runs, exit status 143,
# and no error event (a cancel is not a failure).
CHILD=
CLEANUP_HOOK=      # a function name: runs on cancel and on every exit (removes temporaries)
# How long (in tenths of a second) a stopped child group gets before it is killed. A script that runs another
# script sets a longer wait than that one, so the inner script's own cleanup finishes before the outer kills it.
KILL_WAIT_TICKS=30

kill_child() {
    [ -n "$CHILD" ] || return 0
    kill -TERM -- "-$CHILD" 2>/dev/null || kill -TERM "$CHILD" 2>/dev/null || true
    local i=0
    while [ $i -lt "$KILL_WAIT_TICKS" ] && pgrep -g "$CHILD" >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
    kill -KILL -- "-$CHILD" 2>/dev/null || true
    wait "$CHILD" 2>/dev/null || true
    CHILD=
}

on_cancel() {
    trap '' TERM INT HUP PIPE
    ERROR_EMITTED=1
    kill_child
    exit 143
}

# the EXIT trap: a stop that fail() did not report (a plain die, a failed command under set -e) still gives
# the launcher its error event.
on_exit() {
    local rc=$?
    trap '' TERM INT HUP PIPE
    kill_child
    if [ -n "$CLEANUP_HOOK" ]; then "$CLEANUP_HOOK" || true; fi
    if [ $rc -ne 0 ] && [ "$PROGRESS_JSON" = 1 ] && [ "$ERROR_EMITTED" = 0 ]; then
        error_event "$DEFAULT_ERROR" "${LAST_DIE:-stopped unexpectedly (exit status $rc)}"
    fi
}

install_traps() {
    trap on_exit EXIT
    trap on_cancel TERM INT HUP PIPE     # (a launcher that stops reading our output is a cancel too)
}

# run_child <command> [args...]: runs it in its own process group, returns its status.
run_child() {
    set -m
    "$@" </dev/null &
    CHILD=$!
    set +m
    local rc=0
    wait "$CHILD" || rc=$?
    CHILD=
    return $rc
}

# ---- classifying a failure ------------------------------------------------------------------------------------------------
log_size() { wc -c < "$1" 2>/dev/null | tr -d ' ' || echo 0; }
# net_error_since <log> <size before>: 0 when what was added to the log after that size shows a network failure
net_error_since() {
    tail -c +$(( ${2:-0} + 1 )) "$1" 2>/dev/null \
        | grep -qiE 'Could not resolve host|Failed to connect|unable to access|Connection (timed out|reset|refused)|Network is unreachable|early EOF|RPC failed|SSL_|Could not read from remote|Temporary failure in name resolution|Operation timed out'
}

# ---- ninja progress -------------------------------------------------------------------------------------
# A filter for ninja's output (the caller keeps the log with tee): the `[n/N]` lines become progress events, at
# most about one a second, the first and the last; without --progress json a plain `[n/N] ...` line every few
# seconds. PROGRESS_BASE, PROGRESS_SPAN and PROGRESS_SCALE (all empty by default) place the counts inside a
# larger bar: done = BASE + SPAN * n / N of SCALE units, with PROGRESS_LABEL in front of the note.
PROGRESS_BASE=0
PROGRESS_SPAN=
PROGRESS_SCALE=
PROGRESS_LABEL=
ninja_progress() {
    local line n total note started= lastsec=-100 eta done_u total_u
    while IFS= read -r line; do
        if [[ $line =~ ^\[([0-9]+)/([0-9]+)\]\ (.*)$ ]]; then
            n=${BASH_REMATCH[1]}; total=${BASH_REMATCH[2]}; note=${BASH_REMATCH[3]}
        else
            continue
        fi
        [ "$n" -gt 0 ] || continue
        [ -n "$started" ] || started=$SECONDS
        if [ "$PROGRESS_JSON" != 1 ]; then
            if [ "$n" -eq "$total" ] || [ $((SECONDS - lastsec)) -ge 5 ]; then
                lastsec=$SECONDS
                printf '[%s/%s] %s\n' "$n" "$total" "${note:0:100}"
            fi
            continue
        fi
        if [ "$n" -eq "$total" ] || [ "$n" -eq 1 ] || [ "$SECONDS" -ne "$lastsec" ]; then
            lastsec=$SECONDS
            eta=
            if [ "$n" -gt 1 ] && [ "$n" -lt "$total" ] && [ "$SECONDS" -gt "$started" ]; then
                eta=$(( (SECONDS - started) * (total - n) / (n - 1) ))
            fi
            if [ -n "$PROGRESS_SCALE" ]; then
                done_u=$(( PROGRESS_BASE + PROGRESS_SPAN * n / total )); total_u=$PROGRESS_SCALE
            else
                done_u=$n; total_u=$total
            fi
            progress_event "$done_u" "$total_u" "$eta" "${PROGRESS_LABEL}${note:0:100}"
        fi
    done
    return 0
}

# ---- the source's identity (the stamps in the app's Info.plist) -------------------------------------------
# series_dirt_ok <submodule dir> <patch dir>: 0 when the submodule's tracked files equal its commit with the
# patch series applied (cmake/patch_series.cmake does that at configure time), or are not changed at all.
series_dirt_ok() {
    local dir=$1 patches=$2 index p
    [ -e "$dir/.git" ] || return 0     # not a checkout of its own (a source archive): nothing to compare
    git --no-optional-locks -C "$dir" diff --quiet HEAD 2>/dev/null && return 0
    set -- "$patches"/*.patch
    [ -e "$1" ] || return 1
    index=$(mktemp "${TMPDIR:-/tmp}/fable2-series-index.XXXXXX") || return 1
    rm -f "$index"
    p=0
    GIT_INDEX_FILE=$index git -C "$dir" read-tree HEAD >/dev/null 2>&1 \
        && GIT_INDEX_FILE=$index git -C "$dir" apply --cached "$@" >/dev/null 2>&1 \
        && GIT_INDEX_FILE=$index git -C "$dir" diff --quiet 2>/dev/null || p=1
    rm -f "$index"
    return $p
}

# git_stamp [repo]: sets STAMP_COMMIT (short HEAD), STAMP_DIRTY (1 when tracked files differ from HEAD, not
# counting a submodule that holds only the configure-time patch series; untracked and ignored files never
# count) and STAMP_BASE (short merge base of HEAD with <remote>/main when that remote exists, else empty).
# The remote is `upstream`, or the one named by FABLE2_UPSTREAM_REMOTE (a dev tree can stamp with `origin`);
# a launcher's clone gets GitHub's itar3 repository as `upstream`.
git_stamp() {
    local repo=${1:-$ROOT} base remote=${FABLE2_UPSTREAM_REMOTE:-upstream}
    STAMP_COMMIT=$(git --no-optional-locks -C "$repo" rev-parse --short HEAD 2>/dev/null) || STAMP_COMMIT=unknown
    STAMP_DIRTY=0
    if [ -n "$(git --no-optional-locks -C "$repo" status --porcelain --untracked-files=no --ignore-submodules=dirty 2>/dev/null)" ]; then
        STAMP_DIRTY=1
    elif ! series_dirt_ok "$repo/thirdparty/XenosRecomp" "$repo/tools/xenosrecomp-patches" \
         || ! series_dirt_ok "$repo/thirdparty/plume" "$repo/tools/plume-patches"; then
        STAMP_DIRTY=1
    fi
    STAMP_BASE=
    if git -C "$repo" remote get-url "$remote" >/dev/null 2>&1; then
        base=$(git --no-optional-locks -C "$repo" merge-base HEAD "refs/remotes/$remote/main" 2>/dev/null) \
            && STAMP_BASE=$(git --no-optional-locks -C "$repo" rev-parse --short "$base" 2>/dev/null)
    fi
    return 0
}
