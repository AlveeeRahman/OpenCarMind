# Wi-Fi and the clock

Wi-Fi does one thing on both boards: while the car is parked on a known network, the board asks one HTTPS time server for the time. It checks the server's certificate, sends nothing else and listens on no port. You can compile Wi-Fi out with `OCM_WIFI_CLOCK_SYNC`.

On the ESP32-S3 you add networks in the desktop app. On the ESP-Mosaico you can also search for a network and join it from the touch screen. See the manual chapters for [the S3](../manuals/esp32-s3/wifi.md) and [OpenCarMind EdgeOS 2.1.0](../manuals/edgeos-2.1.0/wifi.md).

## Saved networks

```{include} ../_gen/config-wifi.md
```

## The clock

```{include} ../_gen/config-clock.md
```

## Clock, brightness and theme on the ESP-Mosaico

```{include} ../_gen/mosaico-clock.md
```

On the ESP-Mosaico, OpenCarMind EdgeOS 2.1.0 also refuses a Wi-Fi time that jumps more than 24 hours ahead or 10 minutes back. See [Pairing safety](../concepts/pairing-safety.md).
