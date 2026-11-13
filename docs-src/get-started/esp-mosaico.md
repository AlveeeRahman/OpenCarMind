# ESP-Mosaico

The ESP-Mosaico build is OpenCarMind EdgeOS 2.1.0. It comes from the same source tree as the S3 build and keeps what mossREV 1.2.0 had: both models, the touch apps, Wi-Fi for the clock and the desktop app over USB. Version 2.1.0 adds the [Health app](../concepts/health-app.md) and the [AI services](../concepts/ai-services.md) behind it.

## What you need

```{include} ../_gen/mosaico-hardware.md
```

You also need a Bluetooth LE OBD-II adapter, a USB-C cable, Python with pyserial and esptool 5.6.0 or later. The board needs no SD card: the drives go to its NAND.

## Flash the firmware

```{include} ../_gen/mosaico-flashing.md
```

## Release image and going back

```{include} ../_gen/readme-flash-back.md
```

## Pair the adapter

With no saved adapter, the board offers a new adapter only at -70 dBm or better and saves nothing until you say yes. The screen shows "OBD adapter found: &lt;name&gt;, signal strong. Use this adapter?" with "Not this one" and "Use this adapter", and counts down 30 s. The [pairing chapter of the manual](../manuals/edgeos-2.1.0/start.md) has every question the board can ask, and [Pairing safety](../concepts/pairing-safety.md) explains the rules behind them.

## First drive

About 65 s after a start the board rates its own health. The Board page then says "Parked, health good" in green. Start the engine and drive: the board turns to Driving once the car's computer answers and rows are logged. See [First drive](first-drive.md) and the [user manual](../manuals/edgeos-2.1.0/index.md).
