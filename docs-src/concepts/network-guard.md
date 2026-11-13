# Network guard

The network guard watches the Bluetooth and Wi-Fi traffic the board already receives, for floods of new Bluetooth addresses and rotating Wi-Fi access points. For the first 2 hours after every start it only learns and blocks nothing. A block only does safe, reversible things, and it never cuts your adapter during a drive. The messages it shows are in the [manual](../manuals/edgeos-2.1.0/os.md#the-network-guard).

## How the intrusion detector works

```{include} ../_gen/readme-ids.md
```

:::{note}
Since 2.1.0 the rotation detector is in the firmware as the network guard, bit-exact against the Mac. See the status table on [AI services](ai-services.md).
:::
