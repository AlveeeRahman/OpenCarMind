# OpenCarMind EdgeOS

OpenCarMind EdgeOS is the firmware for the ESP-Mosaico. It runs on FreeRTOS's preemptive scheduler and adds a small set of rules that decide what may run, when and how fast. This page covers the five parts a driver meets: the boot health check, the runlevels, model admission by response-time analysis, the degradation ladder and battery protection.

```{include} ../_gen/readme-edgeos-intro.md
```

## What it is

```{include} ../_gen/readme-what-it-is.md
```

## The parts and why each exists

```{include} ../_gen/readme-parts.md
```

## Services and runlevels

```{include} ../_gen/readme-runlevels.md
```

## Model admission by response-time analysis

```{include} ../_gen/readme-admission.md
```

## Scheduler budgets per core

```{include} ../_gen/readme-budgets.md
```

## What you see on the board

```{include} ../_gen/readme-on-board.md
```

## What it costs

```{include} ../_gen/readme-costs.md
```

## Prepared but off: A/B rollback and the battery as a UPS

```{include} ../_gen/readme-prepared-off.md
```
