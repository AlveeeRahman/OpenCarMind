#!/bin/sh
# Large model on an NVIDIA GPU: Kind 1 + Kind 2. The small on-device model is trained on the Mac
# (scripts/train_mac.sh) and is not part of this script.
# Run from the repository root after `sh scripts/download.sh` and `sh scripts/extract.sh`.
set -e
python -m pip install -r requirements.txt
export PYTHONPATH=src

# CarMind-Former: channel-agnostic patch transformer, self-supervised on Kind 1 + Kind 2 time series
python -m carmindformer.prepare_cache --data data --out data/cache
python -m carmindformer.train --model former --config configs/former.yaml --data data --device cuda
FORMER=$(ls -t runs/former-*/last.pt | head -1)
python -m carmindformer.eval --ckpt "$FORMER" --data data --device cuda

# CarMind-MLM: masked-token pretraining on Kind 2 CAN frames, then attack type per window and injected frame
python -m carmindformer.train_supervised can --data data --epochs 6 --steps 2000 --batch 128 --pretrain-steps 3000 --d-model 256 --layers 6 --device cuda
