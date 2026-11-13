# C API of the firmware modules

The public headers of the main firmware modules in `main/`. For each header the page shows its opening comment as written in the source, and a table of the functions it declares with the comment the header gives each one. The pages are built from the headers each time the site is built.

Most rules live in plain C files with no ESP-IDF calls (the `_core` headers), so the host tests check them on a computer. The other headers connect those rules to the firmware.

```{include} ../_gen/c-api-toc.md
```
