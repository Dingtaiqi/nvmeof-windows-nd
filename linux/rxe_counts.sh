#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  rxe_counts.sh - dump rxe's hw counters, optionally diffing against a snapshot.
#
#  Usage:
#      rxe_counts.sh before     # writes /tmp/rxe_counts.before and prints them
#      rxe_counts.sh after      # writes .after, prints both and the delta
#
#  Read-only.  Exists because "the read works but the write does not" needs to be
#  told apart from "the write never reached the wire", and these counters are the
#  only place that distinction is visible without a packet capture tool.
# ===========================================================================
C=/sys/class/infiniband/rxe0/ports/1/hw_counters
WHAT="${1:-show}"

dump() {
    for f in "$C"/*; do
        printf '%s %s\n' "$(basename "$f")" "$(cat "$f")"
    done
}

case "$WHAT" in
    before)
        dump > /tmp/rxe_counts.before
        echo "--- rxe counters (before) ---"
        cat /tmp/rxe_counts.before
        ;;
    after)
        dump > /tmp/rxe_counts.after
        echo "--- rxe counters: delta (after - before) ---"
        join /tmp/rxe_counts.before /tmp/rxe_counts.after 2>/dev/null | \
        while read -r name before after; do
            [ "$before" = "$after" ] && continue
            printf '  %-26s %s -> %s\n' "$name" "$before" "$after"
        done
        echo "--- full (after) ---"
        cat /tmp/rxe_counts.after
        ;;
    *)
        dump
        ;;
esac
