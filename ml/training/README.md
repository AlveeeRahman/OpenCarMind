# Training the on-board model

This folder holds the code that trained, refined and exported the model on the board. You will find the shipped files in `../onboard`, and [../../docs/MODEL.md](../../docs/MODEL.md) explains what the model does. `docs/on-board-model-report.md` is the full experiment report, including the ideas that did not help.

## What is here

| Path | What it holds |
|---|---|
| `src/carmindformer/` | model, data loaders, labels, training, search, quantisation and export code |
| `scripts/` | shell wrappers for download, training and refinement |
| `configs/` | YAML configs for the larger research models |
| `tools/` | `target_scan.py` (which extra signals could the model estimate) and `espdl_bench/` (run the exported model on a board) |
| `shipped/` | weights and configuration of the search winner, the starting point for refinement |
| `docs/` | the model report, a systems note and the training logs of the shipped model |

The model on the board is built from `model_onboard.py`, found by `nas_edge.py`, trained by `train_onboard.py` and exported by `export_onboard.py`. Start with `train_onboard.py`, a single readable script that trains the deployed network. The labels are in `tasks.py`, the channel names in `channels.py` and the loaders in `data.py`.

`model_former.py`, `model_mlm.py`, `train.py` and `can_frames.py` belong to an earlier research phase, with larger transformer models on OBD and CAN data. The firmware does not use them.

## Set up

On a Mac with Apple silicon (what the shipped model used, MLX on the GPU):

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements-mac.txt
```

On other machines, `requirements.txt` installs the PyTorch-only set. Refinement needs MLX.

## Data

The model trains on the HCRL Driving Dataset (one KIA Soul, ten drivers, about 94,000 rows at 1 Hz). The authors released it for academic purposes and ask users to cite the paper below. `sh scripts/download.sh` fetches it into `data/` and `sh scripts/extract.sh` unpacks it. Nothing from the dataset is stored in this repository.

## Train and export

The network came from a search and a quantisation-aware training run. The weights and configuration of that search winner are in `shipped/`, so you can train without repeating the search. To rerun the search yourself:

```sh
PYTHONPATH=src python3 -m carmindformer.nas_edge --heads man,est --name edge_kind1_me_v1 --final --qat --single-task --top 5
```

`train_onboard.py` builds dense one-second windows with MAP removed, thins half of them like a slow adapter, trains with QAT for 3 seeds of 60 epochs (about 15 minutes on an M-series Mac) and keeps the best seed on validation. It reads the network and hyperparameters from `configs/onboard.json` and starts from the weights in `shipped/` when you pass `--init`. The test split is read once, at the end.

```sh
CARMIND_DROP_CHANNELS=map_kpa PYTHONPATH=src python3 train_onboard.py --data data \
    --init shipped/edge_kind1_me_v1_winner_qat.safetensors --lr-scale 0.5 --name onboard_v1
CARMIND_DROP_CHANNELS=map_kpa PYTHONPATH=src python3 -m carmindformer.export_onboard \
    --weights runs/onboard/onboard_v1/onboard_v1_winner_qat.safetensors \
    --config runs/onboard/onboard_v1/onboard_v1_final_qat.json \
    --target esp32s3 --quant-type w8a16 --out export/onboard_s3
```

Add `--eval-only` to score existing weights on the test split. Rerunning the command above from the shipped weights gave a clean macro-F1 of 0.912 and R² of 0.822, and 0.877 and 0.789 on thinned windows, against 0.911, 0.830, 0.874 and 0.792 for the shipped model. GPU training is not bit-for-bit repeatable, so expect differences of about 0.01.

The exporter writes the 8-bit model, `norm.json`, `targets.json` and `export.json`. Copy them into `../onboard`, then regenerate `main/ml_params.h` with `tools/gen_ml_params.py` from the repository root. The refinement keeps the estimate means and standard deviations in `../onboard/targets.json`, so the firmware constants stay valid.

## Citation

B. I. Kwak, J. Woo and H. K. Kim, "Know Your Master: Driver Profiling-based Anti-theft Method," PST 2016.
