#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
# Actual LVGL renderer with the unchanged64KiB pool and a CLIB/ASan redzone variant.
set -euo pipefail
export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"
FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H="$FW/test/render"
LV="$FW/build/dependencies/lvgl__lvgl"
python3 "$FW/tools/prepare-lvgl.py"
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g -O1)
INC=(-I"$H" -I"$H/stubs" -I"$LV" -I"$LV/src")
for component in ui app_model app_store app_core prusalink validation fake_source diagnostics platform network button json_reader; do
 INC+=(-I"$FW/components/$component/include" -I"$FW/components/$component/src")
done
PRD_CPP=(
 "$FW"/components/app_model/src/{board_identity,build_info,hardware_check,observation,values,state_layers,printer_data,device_data,app_state,events,reducer,presentation,display_policy}.cpp
 "$FW"/components/app_store/src/store.cpp
 "$FW"/components/network/src/lan_policy.cpp
 "$FW"/components/json_reader/src/reader.cpp
 "$FW"/components/prusalink/src/{decode,normalize,outcome,request,worker_lifecycle}.cpp
 "$FW"/components/fake_source/src/{observations,fixtures,scenarios,source}.cpp
 "$FW"/components/validation/src/{status_timeline,walk_time_base}.cpp
 "$FW"/components/ui/src/{formatters,presentation_diff,scene_selection,controller,wifi_qr}.cpp
 "$FW"/components/ui/src/theme/{theme,tokens}.cpp
 "$FW"/components/ui/src/text/text.cpp
 "$FW"/components/ui/src/fonts/{fonts,motion_fonts}.cpp
 "$FW"/components/ui/src/components/{download_mark,message_form,progress_ring,status_message,telemetry_column}.cpp
 "$FW"/components/ui/src/scenes/{operational_energy,operational_accent,operational_luminance,operational_content,operational_scene,unavailable_motion,operational_form,unavailable_acknowledgement}.cpp
 "$FW"/components/ui/src/scenes/{provisioning_scene,factory_reset_scene,firmware_update_scene,access_scene}.cpp
 "$H/stubs/log_stub.cpp"
)
for MODE in 1 2; do
 OUT="$FW/build/controller-tests-$MODE"
 mkdir -p "$OUT"
 # Remove only the obsolete generated object from the reader's previous location.
 # Otherwise the wildcard link can retain code that is no longer in PRD_CPP.
 rm -f "$OUT/p_json_scan.o"
 DEFS=(-DPRD_ASAN_MODE="$MODE" -DLV_CONF_INCLUDE_SIMPLE -DPRD_QR_TEMP_AUDIT)
 i=0
 while IFS= read -r f; do
  "$CC" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu11 -Wno-unused-parameter -c "$f" -o "$OUT/lv_$i.o"
  i=$((i+1))
 done < <(find "$LV/src" -name '*.c' -type f | LC_ALL=C sort)
 for f in "$FW"/components/ui/src/fonts/prd_font_*.c; do
  "$CC" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu11 -c "$f" -o "$OUT/$(basename "${f%.c}").o"
 done
 for f in "${PRD_CPP[@]}"; do
  "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 -Wno-unused-parameter -c "$f" -o "$OUT/p_$(basename "${f%.cpp}").o"
 done
 mkdir -p "$OUT/mask-callbacks"
 "$CC" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu11 -c "$H/mask_callbacks.c" -o "$OUT/mask-callbacks/test.o"
 "$CXX" "${SAN[@]}" "$OUT/mask-callbacks/test.o" "$OUT"/*.o -o "$OUT/mask-callbacks/test"
 MASK_NAMES=(line angle radius fade map)
 for mask in 0 1 2 3 4; do
  ASAN_OPTIONS=detect_leaks=0 "$OUT/mask-callbacks/test" "$mask" > "$OUT/mask-callbacks/$mask.log" 2>&1
  grep -Fxq "callback=${MASK_NAMES[$mask]}" "$OUT/mask-callbacks/$mask.log"
  grep -Fxq "passed=${MASK_NAMES[$mask]}" "$OUT/mask-callbacks/$mask.log"
 done
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/bounded_form.cpp" "$OUT"/*.o -o "$OUT/render-tests"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/render-tests" -1 - > "$OUT/frames.log" 2>&1
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/controller_motion.cpp" "$OUT"/*.o -o "$OUT/controller-tests"
 mkdir -p "$OUT/controller-frames"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/controller-tests" "$OUT/controller-frames" > "$OUT/controller.log" 2>&1
 mkdir -p "$OUT/access-frames"
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/access_scene.cpp" "$OUT"/*.o -o "$OUT/access-tests"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/access-tests" "$OUT/access-frames" > "$OUT/access.log" 2>&1
 mkdir -p "$OUT/message-frames"
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/message_family.cpp" "$OUT"/*.o -o "$OUT/message-tests"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/message-tests" "$OUT/message-frames" > "$OUT/message.log" 2>&1
 ASAN_OPTIONS=detect_leaks=0 "$OUT/message-tests" --controls > "$OUT/message-controls.log" 2>&1
 mkdir -p "$OUT/firmware-update-frames"
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/firmware_update.cpp" "$OUT"/*.o -o "$OUT/firmware-update-tests"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/firmware-update-tests" "$OUT/firmware-update-frames" > "$OUT/firmware-update.log" 2>&1
 ASAN_OPTIONS=detect_leaks=0 "$OUT/firmware-update-tests" --controls > "$OUT/firmware-update-controls.log" 2>&1
 # Causal controls: the actual pre-repair production behaviour, required to fail the suite.
 bash "$FW/tools/test-message-controls.sh" "$MODE"
 # Access review uses a separate wrapper; never link both synthetic-source owners together.
 mkdir -p "$OUT/access-review-frames"
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -I"$FW/test/support" -std=gnu++17 \
  -DCONFIG_PRD_VALIDATION_PRINTING_TIMELINE=1 -DCONFIG_PRD_VALIDATION_ACCESS_REVIEW=1 \
  "$H/access_review.cpp" "$FW/components/validation/src/access_review.cpp" \
  "$FW/components/validation/src/access_review_flow.cpp" \
  "$FW/components/validation/src/gesture_policy.cpp" "$OUT"/*.o -o "$OUT/access-review"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/access-review" "$OUT/access-review-frames" > "$OUT/access-review.log" 2>&1
 # Optional on-board review instrument, linked separately so its flags/objects cannot alter
 # either existing renderer corpus. The production decoder/store/controller are shared above.
 mkdir -p "$OUT/review-objects" "$OUT/review-frames"
 REVIEW_DEFS=(-DCONFIG_PRD_VALIDATION_PRINTING_TIMELINE=1 -DCONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW=1)
 for file in printing_timeline_flow loss_review gesture_policy; do
  "$CXX" "${SAN[@]}" "${DEFS[@]}" "${REVIEW_DEFS[@]}" "${INC[@]}" -std=gnu++17 -c "$FW/components/validation/src/$file.cpp" -o "$OUT/review-objects/$file.o"
 done
 "$CXX" "${SAN[@]}" "${DEFS[@]}" "${INC[@]}" -std=gnu++17 "$H/printing_review.cpp" "$OUT"/*.o "$OUT/review-objects"/*.o -o "$OUT/printing-review"
 ASAN_OPTIONS=detect_leaks=0 "$OUT/printing-review" "$OUT/review-frames" > "$OUT/review.log" 2>&1
 ASAN_OPTIONS=detect_leaks=0 "$OUT/printing-review" --rapid-controls > "$OUT/review-controls.log" 2>&1
done
python3 "$H/verify.py" "$FW/build/controller-tests-1/frames.log" "$FW/build/controller-tests-2/frames.log"
