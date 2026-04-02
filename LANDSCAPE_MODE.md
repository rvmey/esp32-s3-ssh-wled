# Landscape Mode Solution

## Status

Landscape mode is working on the JC3248W535.

The working solution does **not** rely on rotating the AXS15231 controller with `MADCTL`.
Instead, the display stays in its native portrait addressing mode and landscape is implemented in software during text rendering.

## Final Working Approach

### 1. Keep the LCD controller in portrait mode

- The AXS15231 is left in its known-good native orientation.
- `screen_set_landscape(true)` only changes the logical orientation state.
- No `MADCTL` rotation is applied for landscape.

This avoids the controller-specific QSPI behavior that previously caused partial updates, edge strips, or blank screens.

### 2. Always write the physical panel as `320 x 480`

- The panel is always addressed with the full physical window:
  - `CASET = 0..319`
  - `RASET = 0..479`
- Pixel data is always streamed as physical portrait rows.
- Row writes use the known-good path:
  - first row: `RAMWR (0x2C)`
  - remaining rows: `RAMWRC (0x3C)`

This is the same physical write path that already worked reliably in portrait mode.

### 3. Treat landscape as a logical `480 x 320` canvas

- Portrait logical size: `320 x 480`
- Landscape logical size: `480 x 320`

The renderer maps logical coordinates into the physical portrait framebuffer.

### 4. Rotate text in software

For each physical pixel `(x, y)` written to the panel, the landscape renderer converts it back to a logical coordinate before testing whether that logical pixel should be foreground or background.

The working landscape mapping is:

```c
logical_x = (LCD_PHYS_H - 1) - y;
logical_y = x;
```

That is a 90-degree clockwise software rotation from the logical landscape view into the physical portrait memory layout.

## Why This Works

Previous attempts depended on controller-side rotation and QSPI-specific addressing behavior. Those attempts ran into multiple hardware/driver edge cases:

- `MADCTL` rotation did not produce a reliable full-screen landscape render.
- `RASET` / `CASET` behavior in QSPI mode was difficult to reason about and easy to misapply.
- Holding CS active across row transactions introduced additional SPI-driver constraints.
- Some combinations produced only a thin strip of pixels along the edge.

The software-rotation approach removes those variables.

Only one thing changes in landscape mode:

- how logical text coordinates are mapped into the already-working physical portrait write path.

That makes the behavior deterministic and much easier to maintain.

## Practical Rule

If portrait works and landscape breaks again, do **not** start by changing the SPI write path.

First check only these items:

1. `s_landscape` state
2. logical width/height (`lcd_w()` / `lcd_h()`)
3. the software coordinate transform used during text rendering

The physical LCD write path should remain portrait-only unless there is a strong reason to revisit controller-side rotation.

## Relevant Files

- `main/screen_control.c`
- `main/screen_control.h`
- `main/ssh_server.c`
