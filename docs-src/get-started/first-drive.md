# First drive

Both boards start logging on their own once the adapter is paired and the car's computer answers. This page lists what to look at on a first drive and what to send back after it.

## Before you set off

On the ESP32-S3, check that page 2 shows the adapter as "(saved)" and that page 1 shows RPM once the engine runs. On the ESP-Mosaico, wait for "Parked, health good" on the Board page, about a minute after the start.

## While you drive

Page 5 of the S3 shows what the model sees, its class (stopped, braking, accelerating or cruising) and its estimates of brake pressure, torque and fuel rate. On the ESP-Mosaico the Drive and AI apps show the same. While a drive runs, the ESP-Mosaico refuses a model switch, a clock change and a format, and says why. The list of those messages is in [Messages](../reference/messages.md).

## After the drive

Once everything is on the card, page 3 of the S3 shows "Safe to turn off" in green. The ESP-Mosaico keeps its drives on the NAND. Import them with the [desktop app](../guides/desktop-app.md) over the USB cable.

Checks that need a car are in [Field checks](../research/field-checks.md), with tables to fill in. The ESP-Mosaico manual lists what to send back after a test drive in its [troubleshooting chapter](../manuals/edgeos-2.1.0/trouble.md).
