#!/bin/sh
# Small model (CarMind-Edge-S) on the Mac: Kind 1 data only (HCRL Driving Dataset), Apple MLX on Metal,
# quantization-aware from the first step, then ESP-PPQ int8 for the ESP32-S31. The large model trains on CUDA
# (scripts/train_cuda.sh); nothing here touches Kind 2.
#
#   sh scripts/train_mac.sh                  # full run, about 2 minutes on an M-series Mac
#   EPOCHS=3 STEPS=50 sh scripts/train_mac.sh  # smoke run
set -e
cd "$(dirname "$0")/.."
PY="${PY:-$HOME/Desktop/dev/metal_env/bin/python}"   # torch + mlx + esp-ppq live in this env
EPOCHS="${EPOCHS:-40}"; STEPS="${STEPS:-200}"; OUT="${OUT:-runs/supervised_mlx}"
export PYTHONPATH=src
"$PY" -c "import mlx.core as mx; assert mx.metal.is_available(), 'Metal not available'" 2>/dev/null

echo "== baseline (logistic regression on the same channels and split)"
"$PY" -m carmindformer.baseline_driver --data data --protocol holdout --channels edge | tail -1
"$PY" -m carmindformer.baseline_driver --data data --protocol session --channels edge | tail -1

echo "== Edge-S, QAT, holdout protocol (64/16/20 per drive)"
"$PY" -m carmindformer.train_edge_mlx --data data --protocol holdout --qat --epochs "$EPOCHS" --steps "$STEPS" --out "$OUT" | tail -1
echo "== Edge-S, QAT, cross-session protocol (path 1 -> path 2)"
"$PY" -m carmindformer.train_edge_mlx --data data --protocol session --qat --epochs "$EPOCHS" --steps "$STEPS" --out "$OUT" | tail -1

echo "== export (ONNX, calibration windows, gallery, normalisation) and ESP-PPQ int8 for the ESP32-S31"
"$PY" -m carmindformer.export_edge_sup --mlx-weights "$OUT/edge_sup_holdout_qat.safetensors" --data data --out export/edge_sup | tail -1
"$PY" -m carmindformer.quantize_edge_sup --export export/edge_sup --data data --target esp32s31 --quant-type w8a8 2>/dev/null | tail -1
