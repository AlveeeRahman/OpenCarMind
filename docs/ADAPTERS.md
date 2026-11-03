# OBD-II adapters

OpenCarMind talks to the car through a Bluetooth LE adapter that speaks the ELM327 command set. This page lists the adapters the firmware was written to support, what each one uses on the radio, and where to buy it.

Only the LELink has been driven with this board. Every other row is based on the adapter's published Bluetooth layout or on what other open-source OBD projects use for it. If you try one, please report the result with the Field check issue template so the table can move from "expected" to "tested".

## What the board needs from an adapter

- Bluetooth LE. The ESP32-S3 has no Classic Bluetooth radio, so adapters that only offer Bluetooth 3.0 (SPP) cannot connect. Many cheap adapters sold as "Android only" are Classic.
- ELM327 or STN commands. Adapters that only work with their own app and speak a private protocol will connect, fail the ELM327 reset, and be skipped.

## How the board finds and talks to an adapter

1. It scans and collects candidates for 1.5 s, then connects to the one with the strongest signal. A device counts as a candidate when its name contains a word adapters use (OBD, ELM327, Vgate, vLink, Veepeak, Carista, Kiwi, LELink, Konnwei, Viecar and others), when its name starts with an entry of `OBD_BLE_NAME_PREFIXES`, or when it advertises a service that OBD adapters use (FFF0, 18F0, BEEF, or the Vgate E7810A71 service).
2. It reads the GATT table and picks the serial line. Known layouts are tried first, then the first non-standard service with a characteristic it can subscribe to (notify or indicate) and one it can write to.
3. It subscribes. If the adapter refuses because the link is not encrypted, the board pairs with Just Works and bonds. If Just Works is refused, it tries once more with the fixed PIN 123456, which OBDLink documents for its CX.
4. It sends `ATZ`. The address is stored only after the device answers like an ELM327. A device that has no serial line, cannot be subscribed to, or never answers is skipped for 10 minutes and the scan moves on.
5. Writes are cut to the negotiated MTU, and each piece waits for its acknowledgement.
6. If the ECU does not answer under automatic protocol search, the board tries each protocol in turn (CAN 11-bit 500k, CAN 11-bit 250k, J1850 PWM, CAN 29-bit 500k, CAN 29-bit 250k, J1850 VPW, ISO 9141-2, ISO 14230 slow and fast init).

## Known serial layouts

| Layout | Service | Receive | Send | Seen on | Source |
|---|---|---|---|---|---|
| FFF0 | FFF0 | FFF1 (notify) | FFF2 (write) | OBDLink CX, Veepeak, Carista, most clones | [OBDLink CX developer notes](https://support.obdlink.com/support/solutions/articles/43000746707-obdlink-cx-adapter-notes), [Stack Overflow](https://stackoverflow.com/questions/61090365/what-ble-characteristic-should-i-use-in-a-ble-ios-device-for-obdii) |
| FFE0 | FFE0 | FFE1 | FFE1 (same characteristic) | LELink, HM-10 style clones | tested on this board |
| FFE0 split | FFE0 | FFE1 | FFE2 | Deep OBD | [EdiabasLib](https://github.com/uholeschak/ediabaslib) |
| Vgate E7810A71 | E7810A71-73AE-499D-8C15-FAA9AEF0C3F2 | BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F | same characteristic | Vgate vLinker | [EdiabasLib](https://github.com/uholeschak/ediabaslib), [LTSupportAutomotive](https://github.com/mickeyl/LTSupportAutomotive), [ha-obd-ble](https://github.com/Jmann235/ha-obd-ble) |
| Vgate 18F0 | 18F0 | 2AF0 | 2AF1 | Vgate iCar Pro BLE | [SwiftOBD2](https://github.com/kkonteh97/SwiftOBD2) |
| BEEF | BEEF | picked by property | picked by property | listed by LTSupportAutomotive | [LTSupportAutomotive](https://github.com/mickeyl/LTSupportAutomotive) |
| Nordic UART | 6E400001-B5A3-F393-E0A9-E50E24DCCA9E | 6E400003 | 6E400002 | some DIY and clone adapters | [AndrOBD](https://github.com/fr3ts0n/AndrOBD) |

## Adapters

Prices are snapshots from US listings seen on 3 November 2026 and change often. "Not found" means no current price showed up. Amazon links are plain product pages with no affiliate tag.

| Adapter | Radio | Layout | Status | Price seen | Buy |
|---|---|---|---|---|---|
| LELink² | BLE | FFE0 | Tested on this board | not found | [Amazon B0755N61PW](https://www.amazon.com/dp/B0755N61PW) |
| Veepeak OBDCheck BLE+ | BLE (Classic too, for Android) | FFF0 | Expected | about $38 to $42 at US retailers | [Amazon B076XVQMVS](https://www.amazon.com/dp/B076XVQMVS) |
| Veepeak OBDCheck BLE | BLE 4.0 | FFF0 | Expected | about $32 at a US retailer | [Amazon B073XKQQQW](https://www.amazon.com/dp/B073XKQQQW) |
| OBDLink CX | BLE 5.1 | FFF0, always encrypted | Expected, pairs on demand. Bonding is open for the first 5 minutes after the adapter powers up | $79.95 | [Amazon B08NFLL3NT](https://www.amazon.com/dp/B08NFLL3NT) |
| Vgate iCar Pro BLE 4.0 | BLE, shows up as IOS-Vlink | 18F0 | Expected | $31.49 | [Amazon B06XGB4873](https://www.amazon.com/dp/B06XGB4873) |
| Vgate vLinker MC+ | BLE and Classic, BLE side named vLinker MC-IOS | Vgate E7810A71 or generic | Expected | about $55 | [Amazon B088LW211V](https://www.amazon.com/dp/B088LW211V) |
| Vgate vLinker FS | Classic and BLE, ships in MFi mode | Vgate E7810A71 or generic | Needs a setting: switch it to "BLE+BT" with the VgateFwUpdater app first | not found | [Amazon B0BZTXV6MZ](https://www.amazon.com/dp/B0BZTXV6MZ) |
| Vgate vLinker MS | Classic and BLE, ships in MFi mode | Vgate E7810A71 or generic | Needs a setting: switch it to "BLE+BT" with the VgateFwUpdater app first | not found | [Amazon B0D6YQX4R8](https://www.amazon.com/dp/B0D6YQX4R8) |
| Carista OBD2 / Carista EVO | BLE | FFF0 | Expected | not found | [Amazon B00YVHGTBM](https://www.amazon.com/dp/B00YVHGTBM), [EVO B0CM9W4MCW](https://www.amazon.com/dp/B0CM9W4MCW) |
| PLX Kiwi 3 | BLE 4.x | generic | Expected | no current Amazon offer | [Amazon B016X3L0O2](https://www.amazon.com/dp/B016X3L0O2) |
| FIXD | Bluetooth | unknown | Unverified. One owner reports it works with other ELM327 apps | not found, first generation discontinued | [Amazon B013RIQMEO](https://www.amazon.com/dp/B013RIQMEO) |
| Generic "ELM327 BLE 4.0" clones | BLE | usually FFF0 or FFE0 | Expected for most. Some fake v2.1 clones skip commands | varies | [Amazon search](https://www.amazon.com/s?k=ELM327+Bluetooth+4.0+BLE+OBD2) |
| BlueDriver Bluetooth Pro | Bluetooth | private protocol | Not expected: reported to work only with its own app | $79.95 | [Amazon search](https://www.amazon.com/s?k=BlueDriver+Bluetooth+Pro+OBDII) |
| OBDLink MX+ | Classic Bluetooth 3.0 | SPP | Will not work, no Classic radio on the S3 | $139.95 | [Amazon B07JFRFJG6](https://www.amazon.com/dp/B07JFRFJG6) |
| OBDLink LX | Classic Bluetooth 3.0 | SPP | Will not work, no Classic radio on the S3 | not found | [Amazon search](https://www.amazon.com/s?k=OBDLink+LX+Bluetooth) |
| KONNWEI KW902 | Classic, listed for Android and Windows only | SPP | Will not work, no Classic radio on the S3 | not found | [Amazon B073F57QT3](https://www.amazon.com/dp/B073F57QT3) |

If your adapter is not listed, check it with a BLE scanner app such as nRF Connect. If it shows a service with one characteristic that notifies or indicates and one you can write, the generic fallback should find it. If its name has none of the words above and it does not advertise a known service, add its name to `OBD_BLE_NAME_PREFIXES` in `idf.py menuconfig`.

## Sources for the adapter facts

- OBDLink CX layout, encryption, PIN, MTU and write rules: [OBDLink CX adapter notes](https://support.obdlink.com/support/solutions/articles/43000746707-obdlink-cx-adapter-notes)
- OBDLink radios (CX is BLE 5.1, MX+ and LX are Bluetooth 3.0): [Which OBDLink adapter is right for me?](https://support.obdlink.com/support/solutions/articles/43000713351-which-obdlink-adapter-is-right-for-me-)
- Veepeak and Carista on FFF0: [Stack Overflow 61090365](https://stackoverflow.com/questions/61090365/what-ble-characteristic-should-i-use-in-a-ble-ios-device-for-obdii)
- Vgate iCar Pro BLE on 18F0/2AF0/2AF1: [ABRP request](https://abrp.featurebase.app/p/add-odb-support-for-vgate-icar-pro-bluetooth-40-ble-2), SwiftOBD2 source
- vLinker FS and MS MFi mode and the VgateFwUpdater switch: the Amazon listings linked above
- Kiwi 3 on BLE: [HP Tuners forum, TrackAddict developer](https://forum.hptuners.com/showthread.php?p=433373)
- BlueDriver app lock (secondary source): [OBDplanet](https://obdplanet.com/bluedriver-vs-obdlink)
- FIXD with other apps (one owner): [iamcarhacker](https://iamcarhacker.com/?p=15082)
- Name words and fallback rules: [BetterDriver](https://github.com/FlappedEar/BetterDriver), [ha-obd-ble](https://github.com/Jmann235/ha-obd-ble), [SwiftOBD2](https://github.com/kkonteh97/SwiftOBD2)
- Protocol order after a failed auto search: [python-OBD](https://github.com/brendan-w/python-OBD)
