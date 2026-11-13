# Pairing safety

The ESP-Mosaico asks you before it pairs an adapter, keeps a fingerprint of the adapter it paired, and refuses a clock that jumps too far. The S3 build does not change: its pairing prompt is behind `CONFIG_OBD_BLE_PAIR_CONFIRM`, off by default.

## The three rules

```{include} ../_gen/readme-pairing.md
```

## Pairing asks you first

```{include} ../_gen/readme-after-check.md
```
