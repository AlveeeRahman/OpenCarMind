# AI services

Since 2.1.0, OpenCarMind EdgeOS runs three small AI models as services: the battery estimator, the battery anomaly detector and the network guard. They are soft real-time jobs. A late result is worth less, never dangerous. The [battery page](battery.md) and the [network guard page](network-guard.md) explain each model. This page covers how they share the chip.

```{include} ../_gen/readme-ai-services.md
```

## Why the services meet their deadlines and cannot deadlock

```{include} ../_gen/readme-deadlines.md
```

## Models tested and not shipped

```{include} ../_gen/readme-not-shipped.md
```
