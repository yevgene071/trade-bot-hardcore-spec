#!/usr/bin/env bash
# T2-LABELING: показывает прогресс разметки по детекторам.
# Usage: ./tool/labeler/report.sh [replay/labels]

set -euo pipefail

LABELS_DIR="${1:-replay/labels}"

if [ ! -d "$LABELS_DIR" ]; then
    echo "Labels dir not found: $LABELS_DIR"
    exit 1
fi

DETECTORS=(density iceberg tape level approach leader)
TP_MIN=200
FP_MIN=400

printf "\n%-12s %8s %8s %8s  %-8s %-8s\n" "Detector" "Total" "TP" "FP" "TP_goal" "FP_goal"
printf -- "%-12s %8s %8s %8s  %-8s %-8s\n" "----------" "-------" "-------" "-------" "-------" "-------"

for DET in "${DETECTORS[@]}"; do
    TOTAL=0; TP=0; FP=0
    shopt -s nullglob
    for FILE in "$LABELS_DIR"/*_"${DET}"_labels.jsonl; do
        while IFS= read -r line; do
            [[ -z "$line" ]] && continue
            is_tp=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print('1' if d.get('is_true_positive') else '0')" 2>/dev/null || echo "0")
            TOTAL=$((TOTAL + 1))
            if [[ "$is_tp" == "1" ]]; then TP=$((TP + 1)); else FP=$((FP + 1)); fi
        done < "$FILE"
    done

    TP_STATUS="$([ "$TP" -ge "$TP_MIN" ] && echo "OK" || echo "${TP}/${TP_MIN}")"
    FP_STATUS="$([ "$FP" -ge "$FP_MIN" ] && echo "OK" || echo "${FP}/${FP_MIN}")"
    printf "%-12s %8d %8d %8d  %-8s %-8s\n" "$DET" "$TOTAL" "$TP" "$FP" "$TP_STATUS" "$FP_STATUS"
done

echo ""
