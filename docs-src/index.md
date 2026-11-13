---
html_theme.sidebar_secondary.remove: true
---

# OpenCarMind documentation

{.ocm-hero}
OpenCarMind is an open-source OBD2 logger and dashboard for the ESP32. It reads your car through a Bluetooth LE OBD-II adapter, runs small neural networks on the chip and keeps every drive on the board's own storage. It needs no phone, no account and no internet connection.

```{image} ../docs/assets/banner.png
:alt: OpenCarMind, an open-source OBD2 logger for the ESP32 with TinyML on the board.
:class: ocm-figure
```

## Two boards, one source tree

::::{grid} 1 1 2 2
:gutter: 3

:::{grid-item-card} ESP32-S3 board
:link: get-started/esp32-s3
:link-type: doc

Waveshare ESP32-S3-LCD-1.47B, a 1.47 inch LCD and an SD card for the drives.

Firmware: OpenCarMind 2.5.1. Beta: 2.5.1b, a pre-release not yet checked on an S3 board as a whole.
:::

:::{grid-item-card} ESP-Mosaico
:link: get-started/esp-mosaico
:link-type: doc

Espressif ESP-Mosaico V1.2 (ESP32-S31), a 480 × 480 AMOLED touch screen and a 128 MB NAND for the drives.

Firmware: OpenCarMind EdgeOS 2.1.0, with the Health app for the battery and the radios.
:::
::::

## Quick links

::::{grid} 2 2 4 4
:gutter: 2

:::{grid-item-card} Get started
:link: get-started/index
:link-type: doc
What you need, flashing, pairing the adapter and the first drive.
:::

:::{grid-item-card} Flash a board
:link: get-started/flash
:link-type: doc
The S3 image and the ESP-Mosaico image, and how each one is written.
:::

:::{grid-item-card} Desktop app
:link: guides/desktop-app
:link-type: doc
Import drives over USB, switch the model, add fill-ups.
:::

:::{grid-item-card} Releases
:link: https://github.com/AlveeeRahman/OpenCarMind/releases

Firmware images, the desktop app installers and the checksums on GitHub.
:::
::::

## What OpenCarMind is

```{include} _gen/readme-intro.md
```

## What you get

```{include} _gen/readme-what-you-get.md
```

```{toctree}
:hidden:
:maxdepth: 2

get-started/index
guides/index
concepts/index
hardware/index
reference/index
research/index
release-notes
contributing/index
```
