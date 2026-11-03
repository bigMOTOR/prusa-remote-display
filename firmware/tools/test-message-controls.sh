#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Causal negative controls for the message family, in the style of test-repair-controls.py.
#
# ⛔ WHAT MAKES THESE CONTROLS RATHER THAN DECORATION. Each one restores the ACTUAL pre-repair
# production behaviour — through a PRD_OLD_* macro compiled into a disposable object, never into a
# shipped one — relinks the real renderer suite against it, and requires that suite to FAIL with
# the assertion the repair exists for. A control that does not fire proves the test is measuring
# nothing, which is the failure mode every instrument in this repository has had at least once.
#
# The production objects are never touched: each control compiles into its own scratch directory
# and the original .o files are restored by construction, because they are read and never written.
# Standalone runs must retain the same CC/CXX environment used by test-render.sh.
# The ordinary renderer invocation exports that environment to this child.
set -euo pipefail
export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

# ⛔ THE ARGUMENT IS VALIDATED BEFORE ANY PATH IS BUILT FROM IT. It is interpolated into a build
# directory, and this script creates and writes under that directory — so an unchecked argument is
# a filesystem target, not a label. Two allocator modes exist and nothing else is accepted.
MODE="${1:-1}"
case "${MODE}" in
 1|2) ;;
 *) echo "usage: $(basename "$0") [1|2]   (allocator mode)" >&2; exit 2 ;;
esac

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
H="$FW/test/render"
LV="$FW/build/dependencies/lvgl__lvgl"
OUT="$FW/build/controller-tests-$MODE"

[ -d "$OUT" ] || { echo "run tools/test-render.sh first: $OUT missing" >&2; exit 1; }

# Each run is isolated. Keep every failed/unverified control intact; for a control
# proven to fail on its exact expected assertion, retain its log and file hashes
# rather than accumulating copies of all objects and raw animation frames.
python3 "$FW/tools/control-artifacts.py" check "$OUT"
WORK="$(mktemp -d "$OUT/message-controls-XXXXXX")"
python3 "$FW/tools/control-artifacts.py" begin "$WORK"
echo "controls output: $WORK"

SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g -O1)
INC=(-I"$H" -I"$H/stubs" -I"$LV" -I"$LV/src")
for component in ui app_model app_store app_core prusalink validation fake_source diagnostics platform network button; do
 INC+=(-I"$FW/components/$component/include" -I"$FW/components/$component/src")
done
DEFS=(-DPRD_ASAN_MODE="$MODE" -DLV_CONF_INCLUDE_SIMPLE -DPRD_QR_TEMP_AUDIT)

# name | macro | the repair it removes | the assertion that must fail | translation units to rebuild
CONTROLS=(
 "symbol|PRD_OLD_MESSAGE_SYMBOL|R1 the shared symbol reproduces the accepted operational one|the settled shared symbol reproduces the accepted operational symbol exactly|components/ui/src/components/message_form.cpp"
 "reset-crossfade|PRD_OLD_RESET_CROSSFADE|R2 the answered question is retired at once|the answered question is gone from the first frame|components/ui/src/scenes/factory_reset_scene.cpp"
 "session-rows|PRD_OLD_SESSION_ROWS|R3 a session arriving under a statement draws nothing|a session arriving under a statement puts no credential on the panel|components/ui/src/scenes/provisioning_scene.cpp"
 "statement-swap|PRD_OLD_STATEMENT_SWAP|R4 a displacing statement enters rather than swapping|a displacing fault drives the tick|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-static|PRD_OLD_SETUP_STATIC|the protected setup composition animates on return|returning to the setup composition is an entry|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-phase-static|PRD_OLD_SETUP_PHASE_STATIC|an internal setup phase changes visibly without disturbing the session|an internal setup phase transition drives the tick|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-qr-redraw|PRD_OLD_SETUP_QR_REDRAW|an unchanged setup QR is not invalidated on every phase-motion tick|the QR block has no flushed area during the phase transition|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-early-session|PRD_OLD_SETUP_EARLY_SESSION|an early session stays private until the offer exists|an early session cannot reveal the credential under SetupRequired|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-session-reveal|PRD_OLD_SETUP_SESSION_REVEAL|the newly offered QR emerges on the shared clock|new QR starts at zero luminance rather than appearing in one frame|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-stale-offer|PRD_OLD_SETUP_STALE_OFFER|a statement returning to Required does not transiently encode the retained QR|statement return to Required never re-encodes the retained QR|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-session-resubmit|PRD_OLD_SETUP_SESSION_RESUBMIT|an unchanged session does not invalidate an idle QR|an identical setup session neither redraws nor restarts motion|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-session-replace|PRD_OLD_SETUP_SESSION_REPLACE|a coalesced new session scrubs the old credential and re-enters visibly|a coalesced replacement starts a new visible session entry|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-label-scrub|PRD_OLD_SETUP_LABEL_SCRUB|a cleared setup password leaves no bytes in the label buffer|explicit clear wipes every byte of the borrowed password label buffer|components/ui/src/scenes/provisioning_scene.cpp"
 "setup-success-entry|PRD_OLD_SETUP_SUCCESS_ENTRY|a validated setup returns to Ready from a blank appearance rather than the stale operational pose|successful setup starts Ready at its own current form, not stale Printing geometry|components/ui/src/controller.cpp"
 "qr-temp-scrub|PRD_OLD_QR_TEMP_SCRUB|LVGL wipes the temporary QR payload and modules before freeing them|QR temporary buffers are wiped before release|@lv_qrcode"
 "access-static|PRD_OLD_ACCESS_STATIC|a settled access reclassification moves|a reclassification drives the tick|components/ui/src/scenes/access_scene.cpp"
 "entry-clock|PRD_OLD_ENTRY_CLOCK|the bounded motions hold through Off instead of finishing unseen|a dimmed entry does not advance in the dark|components/ui/src/scenes/provisioning_scene.cpp components/ui/src/scenes/access_scene.cpp components/ui/src/scenes/factory_reset_scene.cpp"

 # ⬅️ THE ACCESS SCENE ON ITS OWN, so the evidence is not that SOME scene's hold is measured. This
 # one rebuilds only access_scene.cpp, so every statement and reset assertion still passes and the
 # suite gets as far as the access entry before failing — which is what proves that assertion is
 # the one carrying the access repair.
 "access-entry-clock|PRD_OLD_ENTRY_CLOCK|the access entry and morph hold through Off|a dimmed access entry does not advance in the dark|components/ui/src/scenes/access_scene.cpp"

 # The owner ruling of 2026-09-12: the erase screen animates its dots. Without it the running
 # title is the fixed three-dot string and the cycle never leaves it.
 "reset-static|PRD_OLD_RESET_STATIC|the erase screen's activity cadence|the running line is drawn, at its first dot|components/ui/src/scenes/factory_reset_scene.cpp"
)

for control in "${CONTROLS[@]}"; do
 IFS='|' read -r name macro repair expected sources <<< "$control"
 scratch="$WORK/$name"
 mkdir -p "$scratch/objects"

 # Every unchanged object is reused; only the named units are rebuilt with the old behaviour.
 for object in "$OUT"/*.o; do
  cp "$object" "$scratch/objects/$(basename "$object")"
 done

 for source in $sources; do
  if [ "$source" = "@lv_qrcode" ]; then
   qr_object=""
   for object in "$OUT"/lv_*.o; do
    if nm -g "$object" | grep -E ' [Tt] _?lv_qrcode_update$' >/dev/null; then
     qr_object="$(basename "$object")"
     break
    fi
   done
   [ -n "$qr_object" ] || { echo "LVGL QR object not found" >&2; exit 1; }
   "$CC" "${SAN[@]}" "${DEFS[@]}" "-D$macro=1" "${INC[@]}" -std=gnu11 -Wno-unused-parameter \
    -c "$LV/src/libs/qrcode/lv_qrcode.c" -o "$scratch/objects/$qr_object"
  else
   base="$(basename "${source%.cpp}")"
   "$CXX" "${SAN[@]}" "${DEFS[@]}" "-D$macro=1" "${INC[@]}" -std=gnu++17 -Wno-unused-parameter \
    -c "$FW/$source" -o "$scratch/objects/p_$base.o"
  fi
 done

 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/message_family.cpp" \
  "$scratch/objects"/*.o -o "$scratch/control"

 mkdir -p "$scratch/frames"
 set +e
 ASAN_OPTIONS=detect_leaks=0 "$scratch/control" "$scratch/frames" > "$scratch/control.log" 2>&1
 status=$?
 set -e

 if [ "$status" -eq 0 ]; then
  echo "CONTROL DID NOT FIRE: $name — the suite passed with $repair removed" >&2
  exit 1
 fi

 if ! grep -qxF "FAIL $expected" "$scratch/control.log"; then
  echo "CONTROL FIRED ON THE WRONG ASSERTION: $name" >&2
  echo "  expected: FAIL $expected" >&2
  grep -E '^FAIL' "$scratch/control.log" >&2 || true
  exit 1
 fi

 # A control that crashes rather than asserting proves nothing about the assertion.
 if grep -qE 'ERROR: AddressSanitizer|runtime error:' "$scratch/control.log"; then
  echo "CONTROL FAULTED RATHER THAN FAILING ITS ASSERTION: $name" >&2
  exit 1
 fi

 echo "control $name: $repair removed; the suite failed as required"
 python3 "$FW/tools/control-artifacts.py" compact "$scratch" "$expected"
done

echo "PASS ${#CONTROLS[@]} old-production-code negative controls, mode $MODE"
