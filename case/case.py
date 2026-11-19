# /// script
# requires-python = "==3.13.*"
# dependencies = ["build123d==0.13.0"]
# ///
"""Two-part case for the FYSETC SD-WIFI (non-Pro). Only the micro-USB port is open.

    uv run case/case.py            # writes case/out/{base,lid}.stl and case.step

Coordinates, looking down at the board with its front (ESP module, buttons) up:
x across the board's 24 mm width, y along its length with y=0 at the USB/button end and
y=board_l at the SD contacts, z up. The back of the board carries the micro-USB port
(at the USB end, near the left edge from this view), the microSD slot and the switch.

The board is held only by its SD-card half, which has no parts on either face: rails in the
base support it from below and posts in the lid press on it from above.

Measure the values marked MEASURE with calipers before printing; the defaults are estimates
from photos and the board's published 49.5 x 24 mm size.
"""

from pathlib import Path

from build123d import Align, Box, Location, Pos, export_step, export_stl

# --- Board (MEASURE) ---
board_w = 24.0  # published
board_l = 49.5  # published
board_t = 1.0  # MEASURE: PCB thickness
# Gap under the board. At least the tallest part on the back (micro-USB, microSD slot, switch)
# + 0.4, MEASURE that; the default is higher so the USB plug's ~8 mm moulded body stays above the
# case's bottom face instead of tipping it up.
under = 4.4
over = 4.0  # MEASURE: tallest part on the front (ESP module, buttons) + 0.4
usb_x = 4.2  # MEASURE: micro-USB centre from the board's left edge, front up
usb_drop = 1.4  # MEASURE: micro-USB centre below the PCB's back face
grip_from = 24.0  # where the part-free SD-card half starts, from the USB end

# --- Case ---
fit = 0.3  # gap between board and case walls
wall = 2.0  # leaves the lid's skirt about 1 mm, over two 0.4 mm nozzle lines
floor_t = 1.2
lid_t = 1.2
lip_w = 0.8  # inner part of the wall that rises into the lid's skirt
lip_h = 2.0
skirt_gap = 0.15  # lid skirt to lip, per side
usb_hole = (9.5, 4.5)  # micro-USB plug's metal shell, with margin
usb_recess = (12.0, 8.0)  # the plug's moulded body sits in this
usb_wall = 0.9  # wall left behind the recess, so the plug reaches the connector
rail_w = 2.0

cav_w = board_w + 2 * fit
cav_l = board_l + 2 * fit
out_w = cav_w + 2 * wall
out_l = cav_l + 2 * wall
pcb_z = floor_t + under  # PCB back face
base_h = pcb_z + board_t + over


def base():
    body = Box(out_w, out_l, base_h, align=(Align.MIN, Align.MIN, Align.MIN))
    body -= Pos(wall, wall, floor_t) * Box(cav_w, cav_l, base_h, align=(Align.MIN, Align.MIN, Align.MIN))

    # Lip: the wall's outer part stops lip_h short of the top, so the lid's skirt slides over.
    ring_outer = Box(out_w, out_l, lip_h, align=(Align.MIN, Align.MIN, Align.MIN))
    ring_inner = Pos(wall - lip_w, wall - lip_w, 0) * Box(
        cav_w + 2 * lip_w, cav_l + 2 * lip_w, lip_h, align=(Align.MIN, Align.MIN, Align.MIN)
    )
    body -= Pos(0, 0, base_h - lip_h) * (ring_outer - ring_inner)

    # Rails under the board's SD-card half, along both long edges.
    rail_l = board_l - grip_from
    for x in (wall + fit, wall + fit + board_w - rail_w):
        body += Pos(x, wall + fit + grip_from, floor_t) * Box(
            rail_w, rail_l, under, align=(Align.MIN, Align.MIN, Align.MIN)
        )

    # Micro-USB: a hole for the shell through the end wall, and a recess outside for the plug body.
    cx = wall + fit + usb_x
    cz = pcb_z - usb_drop
    body -= Location((cx, wall / 2, cz)) * Box(usb_hole[0], wall * 3, usb_hole[1])
    recess_d = wall - usb_wall
    body -= Location((cx, recess_d / 2 - 0.01, cz)) * Box(usb_recess[0], recess_d + 0.02, usb_recess[1])
    return body


def lid():
    plate = Box(out_w, out_l, lid_t, align=(Align.MIN, Align.MIN, Align.MIN))
    skirt_outer = Box(out_w, out_l, lip_h - 0.2, align=(Align.MIN, Align.MIN, Align.MIN))
    inset = wall - lip_w - skirt_gap
    skirt_inner = Pos(inset, inset, 0) * Box(out_w - 2 * inset, out_l - 2 * inset, lip_h, align=(Align.MIN, Align.MIN, Align.MIN))
    body = plate + Pos(0, 0, lid_t) * (skirt_outer - skirt_inner)

    # Posts press the board's SD-card half against the rails. The lid prints upside down,
    # so they stand up from the plate here; `over` is the gap from the PCB's front to the lid.
    rail_l = board_l - grip_from
    for x in (wall + fit, wall + fit + board_w - rail_w):
        body += Pos(x, wall + fit + grip_from, lid_t) * Box(
            rail_w, rail_l, over, align=(Align.MIN, Align.MIN, Align.MIN)
        )
    return body


if __name__ == "__main__":
    out = Path(__file__).parent / "out"
    out.mkdir(exist_ok=True)
    b, l = base(), lid()
    assert b.is_valid and l.is_valid
    export_stl(b, str(out / "base.stl"))
    export_stl(l, str(out / "lid.stl"))
    export_step(b + Pos(out_w + 5, 0, 0) * l, str(out / "case.step"))
    plug_bottom = pcb_z - usb_drop - usb_recess[1] / 2
    assert plug_bottom >= 0, f"USB plug body would hang {-plug_bottom:.1f} mm below the case; raise `under`"
    print(f"base {out_w:.1f} x {out_l:.1f} x {base_h:.1f} mm, volume {b.volume:.0f} mm3")
    print(f"lid  {out_w:.1f} x {out_l:.1f} x {lid_t + over:.1f} mm, volume {l.volume:.0f} mm3")
