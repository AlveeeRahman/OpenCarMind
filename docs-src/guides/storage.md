# Storage and files

Everything the board records stays on the board. On the ESP32-S3 that is a FAT-formatted SD card. The ESP-Mosaico writes its drive logs to a 128 MB NAND and keeps its state files in a FAT volume on the flash. The file formats are in [Configuration and file formats](../reference/configuration.md).

## Files on the SD card

```{include} ../_gen/config-sd-files.md
```

## ESP-Mosaico: storage that survives resets

```{include} ../_gen/mosaico-storage.md
```

## ESP-Mosaico: the 128 MB NAND

```{include} ../_gen/mosaico-nand.md
```
