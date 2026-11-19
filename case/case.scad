// Two-part case for the FYSETC SD-WIFI (non-Pro). Only the micro-USB port is open.
//
// Open in OpenSCAD and use the Customizer (Window > Customizer) to set the measured values,
// or render from the command line:
//   openscad -D 'part="base"' -o base.stl case.scad
//   openscad -D 'part="lid"'  -o lid.stl  case.scad
//
// Coordinates, looking down at the board with its front (ESP module, buttons) up: x across the
// board's 24 mm width, y along its length with y=0 at the USB/button end and y=board_l at the
// SD contacts, z up. The back of the board carries the micro-USB port (at the USB end, near the
// left edge from this view), the microSD slot and the switch.
//
// The board is held only by its SD-card half, which has no parts on either face: rails in the
// base support it from below and posts in the lid press on it from above.

/* [What to render] */
part = "both"; // [base, lid, both]

/* [Board: measure these] */
// Published
board_w = 24.0;
// Published
board_l = 49.5;
// PCB thickness
board_t = 1.0;
// Gap under the board: the tallest part on the back (micro-USB, microSD slot, switch) + 0.4, or more. The default keeps a USB plug's ~8 mm moulded body above the case's bottom face.
under = 4.4;
// Tallest part on the front (ESP module, buttons) + 0.4
over = 4.0;
// Micro-USB centre from the board's left edge, front up, USB end towards you
usb_x = 4.2;
// Micro-USB centre below the PCB's back face
usb_drop = 1.4;
// Where the part-free SD-card half starts, from the USB end
grip_from = 24.0;

/* [Case] */
// Gap between board and case walls
fit = 0.3;
// Leaves the lid's skirt about 1 mm, over two 0.4 mm nozzle lines
wall = 2.0;
floor_t = 1.2;
lid_t = 1.2;
// Inner part of the wall that rises into the lid's skirt
lip_w = 0.8;
lip_h = 2.0;
// Lid skirt to lip, per side. Raise it if the lid is too tight.
skirt_gap = 0.15;
// The micro-USB plug's metal shell, with margin
usb_hole = [9.5, 4.5];
// The plug's moulded body sits in this
usb_recess = [12.0, 8.0];
// Wall left behind the recess, so the plug reaches the connector
usb_wall = 0.9;
rail_w = 2.0;

/* [Hidden] */
cav_w = board_w + 2 * fit;
cav_l = board_l + 2 * fit;
out_w = cav_w + 2 * wall;
out_l = cav_l + 2 * wall;
pcb_z = floor_t + under;  // PCB back face
base_h = pcb_z + board_t + over;
usb_z = pcb_z - usb_drop;
rail_l = board_l - grip_from;
rail_xs = [wall + fit, wall + fit + board_w - rail_w];

assert(usb_z - usb_recess[1] / 2 >= 0,
       str("USB plug body would hang ", -(usb_z - usb_recess[1] / 2), " mm below the case; raise `under`"));
assert(wall - lip_w - skirt_gap >= 0.8, "the lid's skirt would be under 0.8 mm; raise `wall`");

module base() {
    difference() {
        union() {
            difference() {
                cube([out_w, out_l, base_h]);
                translate([wall, wall, floor_t]) cube([cav_w, cav_l, base_h]);
                // Lip: the wall's outer part stops lip_h short of the top, so the lid's skirt slides over.
                translate([0, 0, base_h - lip_h]) difference() {
                    cube([out_w, out_l, lip_h + 1]);
                    translate([wall - lip_w, wall - lip_w, -1]) cube([cav_w + 2 * lip_w, cav_l + 2 * lip_w, lip_h + 3]);
                }
            }
            // Rails under the board's SD-card half, along both long edges.
            for (x = rail_xs) translate([x, wall + fit + grip_from, floor_t - 0.01]) cube([rail_w, rail_l, under + 0.01]);
        }
        // Micro-USB: a hole for the shell through the end wall, and a recess outside for the plug body.
        cx = wall + fit + usb_x;
        translate([cx - usb_hole[0] / 2, -1, usb_z - usb_hole[1] / 2]) cube([usb_hole[0], wall + 2, usb_hole[1]]);
        translate([cx - usb_recess[0] / 2, -1, usb_z - usb_recess[1] / 2]) cube([usb_recess[0], 1 + wall - usb_wall, usb_recess[1]]);
    }
}

// Built as printed: flat face down, skirt and posts up. To close the case, flip it over its long
// side, so the posts stay over the SD-card half; the arrow on its face points at the USB end.
module lid() {
    inset = wall - lip_w - skirt_gap;
    difference() {
        cube([out_w, out_l, lid_t]);
        // Arrow engraved in the outer face, pointing at the USB end.
        translate([out_w / 2, 6, -0.01]) linear_extrude(0.41) polygon([[0, -3], [-3, 2], [3, 2]]);
    }
    // Skirt and posts start 0.01 mm inside the plate so the union is one solid.
    translate([0, 0, lid_t - 0.01]) difference() {
        cube([out_w, out_l, lip_h - 0.2 + 0.01]);
        translate([inset, inset, -1]) cube([out_w - 2 * inset, out_l - 2 * inset, lip_h + 2]);
    }
    // Posts press the board's SD-card half against the rails. `over` is the gap from the PCB's
    // front to the lid. The rails sit symmetrically, so the flip leaves the posts over them.
    for (x = rail_xs) translate([x, wall + fit + grip_from, lid_t - 0.01]) cube([rail_w, rail_l, over + 0.01]);
}

if (part == "base") base();
else if (part == "lid") lid();
else {
    base();
    translate([out_w + 5, 0, 0]) lid();
}
