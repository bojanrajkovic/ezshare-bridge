# Case

A two-part case for the FYSETC SD-WIFI (non-Pro), in OpenSCAD (`case.scad`). Only the micro-USB
port is open. The microSD slot, buttons and switch are inside, so set the switch to `USB2UART`
before you close it; later updates go over the air.

The board is held by its SD-card half, which has no parts on either face: rails in the base hold
it from below and posts in the lid from above. The lid slides over a lip on the base's walls and
holds by friction.

## Before you print

The defaults come from the board's published size (49.5 × 24 mm) and photos. Measure these with
calipers and set them in OpenSCAD's Customizer (Window > Customizer), or at the top of
`case.scad`:

| Variable | What to measure |
|---|---|
| `board_t` | PCB thickness |
| `under` | Tallest part on the back (micro-USB, microSD slot, switch), plus 0.4 mm. Keep the default if it's larger. |
| `over` | Tallest part on the front (ESP module, buttons), plus 0.4 mm |
| `usb_x` | From the board's left edge to the micro-USB's centre, with the front facing up and the USB end towards you |
| `usb_drop` | From the PCB's back face to the micro-USB's centre |

Export each part from the app with `part` set to `base`, then `lid`, or render both:

```bash
mise run case   # case/out/base.stl and lid.stl
```

Rendering stops with an error if the USB plug's body would hang below the case.

## Printing

PLA or PETG, 0.2 mm layers, no supports. Print the base open side up and the lid as exported
(flat face down). If the lid is too tight or loose, change `skirt_gap`.

Print the base first and test-fit the board and a cable before printing the lid.

To close the case, flip the lid over its long side. The arrow on its face points at the USB end;
the other way round, its posts land on the ESP module.
