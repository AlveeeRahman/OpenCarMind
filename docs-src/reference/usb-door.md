# USB door protocol

The desktop app talks to the board over its USB cable through an authenticated door. Every line the app reads starts with `OCM1 `. Log lines share the cable and the app ignores them. The door opens only for an HMAC-SHA256 of a fresh random challenge, keyed with the paired app's token, or, to pair a new computer, for the 6-digit code on the board's screen. Everything that changes the board or reads its storage is refused while the car link is up.

The command list below is the comment at the top of `main/usb_door.h`, shown as it is in the source. How the board and the app trust each other is in [Security](../contributing/security.md).

```{literalinclude} ../../main/usb_door.h
:language: text
:end-before: "#pragma once"
```
