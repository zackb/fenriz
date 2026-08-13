#!/bin/bash
# Every Wayland protocol the toolchain offers must be accounted for: implemented in src/, or
# named in docs/PROTOCOLS.md under "Should implement" or "Won't implement". Anything else is
# a blind spot, and this exits 1 naming it.
#
#   ./scripts/protocol-audit.sh            # gate: silent on success, exit 1 on a blind spot
#   ./scripts/protocol-audit.sh --report   # full classification, always exit 0
#
# This is deliberately OPEN-world. The old cross-check (`grep wlr_.*_create src/*.cpp` against
# the Supported table) only compared the doc to the code, so a protocol absent from both was
# invisible to it — which is exactly how ext-workspace-v1 went missing while wlroots had
# shipped a complete implementation for the whole time. The universe here comes from the
# installed headers and XML, not from anything in this repo.
#
# The contract, matching how the doc is written:
#   * Supported is proven by the CODE — a create symbol appearing in src/ is enough.
#   * Everything else must be named in docs/PROTOCOLS.md by its exact upstream basename
#     (`wlr_xdg_dialog_v1`, `ext-workspace-v1`, `text-input-unstable-v3`), so the classification
#     survives a rename and there is no fuzzy matching to drift.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src
DOC=$ROOT/docs/PROTOCOLS.md
REPORT=""
[ "${1:-}" = "--report" ] && REPORT=1

WLR_INC=$(pkg-config --variable=includedir wlroots-0.20 2>/dev/null)/wlroots-0.20/wlr/types
WP_DIR=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)

for d in "$WLR_INC" "$WP_DIR"; do
    [ -d "$d" ] || { echo "protocol-audit: no such directory: $d (missing dev package?)"; exit 1; }
done
[ -f "$DOC" ] || { echo "protocol-audit: missing $DOC"; exit 1; }

# One grep of the whole tree beats one per protocol: ~130 protocols x 2 lookups is slow enough
# to notice in ctest.
code=$(grep -rhoE '\bwlr_[a-z0-9_]+_create[a-z0-9_]*' "$SRC" | sort -u)

implemented=() documented=() unclassified=()

# key: the upstream basename, which is what the doc must name. `syms`: the create symbols that
# prove implementation, empty for XML-only protocols (no wlroots helper to call).
classify() {
    local key=$1 path=$2 syms=$3
    local s
    for s in $syms; do
        if grep -qxF "$s" <<<"$code"; then
            implemented+=("$key ($s)")
            return
        fi
    done
    # Upstream files carry `-unstable-` where everyone (including the doc, and the interface
    # names themselves) writes the bare protocol name: pointer-constraints-unstable-v1 is
    # pointer-constraints-v1. One deterministic strip, not a fuzzy match.
    if grep -qF "$key" "$DOC" || grep -qF "${key/-unstable-/-}" "$DOC"; then
        documented+=("$key")
        return
    fi
    unclassified+=("$key -- $path")
}

# wlroots server-side implementations. A protocol global constructor is the thing taking a
# `struct wl_display *` first — that filters out the ~17 headers here that aren't protocols
# (wlr_buffer.h, wlr_output.h, ...) without a hand-maintained exclusion list to rot.
for h in "$WLR_INC"/*.h; do
    syms=$(tr '\n' ' ' <"$h" |
        grep -oE '[a-z0-9_]+_create[a-z0-9_]*[[:space:]]*\([[:space:]]*struct wl_display \*' |
        grep -oE '^[a-z0-9_]+' | sort -u)
    [ -n "$syms" ] || continue
    classify "$(basename "$h" .h)" "$h" "$syms"
done

# Official protocols. Catches the ones wlroots has no helper for, which the header scan above
# cannot see — a hand-rolled protocol is still a decision worth recording.
while IFS= read -r xml; do
    classify "$(basename "$xml" .xml)" "$xml" ""
done < <(find "$WP_DIR" -name '*.xml' | sort)

if [ -n "$REPORT" ]; then
    printf '== implemented in src/ (%d)\n' "${#implemented[@]}"
    printf '   %s\n' "${implemented[@]}"
    printf '\n== classified in docs/PROTOCOLS.md (%d)\n' "${#documented[@]}"
    printf '   %s\n' "${documented[@]}"
    printf '\n== UNCLASSIFIED (%d)\n' "${#unclassified[@]}"
    [ ${#unclassified[@]} -gt 0 ] && printf '   %s\n' "${unclassified[@]}"
    exit 0
fi

if [ ${#unclassified[@]} -gt 0 ]; then
    echo "protocol-audit: ${#unclassified[@]} protocol(s) in neither the code nor docs/PROTOCOLS.md:"
    printf '  %s\n' "${unclassified[@]}"
    echo
    echo "Implement it, or add it to 'Should implement' / 'Won't implement' in $DOC."
    echo "The row must contain the key verbatim (the name left of ' -- ' above)."
    exit 1
fi

echo "protocol-audit: ${#implemented[@]} implemented, ${#documented[@]} classified, 0 unaccounted for"
