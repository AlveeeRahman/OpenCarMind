# Battery charge, health and anomaly

The ESP-Mosaico has a small cell and a fuel gauge. OpenCarMind EdgeOS 2.1.0 estimates the cell's state of charge and state of health, and checks it for faults. You see the results in plain words in the [Health app](health-app.md). The fixed [battery protection](edgeos.md#the-parts-and-why-each-exists) stays in charge of power: none of these models acts on it.

## How the charge estimator works

```{include} ../_gen/readme-soc.md
```

## How the health estimate works

```{include} ../_gen/readme-soh.md
```

## How the anomaly detector works

```{include} ../_gen/readme-anomaly.md
```

:::{note}
The status of each model on the board is in the table on [AI services](ai-services.md). Where the expert text above and that table differ, the table is the newer one: since 2.1.0 the partial-charge regressor and the anomaly detector are in the firmware.
:::
