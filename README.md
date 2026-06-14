# DSiPhotoSync for TWiLight Menu++

A tiny Nintendo DSi homebrew app that restores the stock *"your camera photos
cycle on the top screen"* whimsy on a [TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu)
setup.

It scans the SD card's `DCIM` folder (where the DSi camera writes JPEGs when
you shoot to SD), decodes each photo, downscales it to fit TWiLight's
top-screen photo limit (208×156 max), and writes a PNG into:

    sd:/_nds/TWiLightMenu/dsimenu/photos/

…which is the folder TWiLight's `dsimenu` theme auto-cycles on the top screen.
A photo counts as already converted once its output PNG exists, so each run
only processes new shots — and deleting a PNG from that folder makes the next
run regenerate it.

> **Scope:** this is built for **DSi-taken photos** (640×480, shot to SD).
> It is not a general camera-photo converter; large phone/DSLR JPEGs are out
> of scope and oversized files are skipped.

## What it does *not* do

- It does **not** read the internal NAND Photo app store (those are stored in
  the encrypted DSi photo format). Shoot to SD so the JPEGs land in `DCIM`.
- It does **not** delete or alter your original photos.

## Using the app

When launched, DSiPhotoSync shows a menu on the bottom screen (the top screen
stays black):

- **Add photos** — choose which new JPEGs to convert (all checked by default),
  preview each on the right, then **START** to convert.
- **Remove photos** — choose already-converted photos to delete from the
  slideshow folder (none checked by default), then **START** to remove.
- **View library** — browse converted photos with previews; **A** expands a
  photo full screen, **A**/**B** collapses it again.
- **Settings** — choose whether the UI shows on the top or bottom screen
  (Left/Right to change; the screen switches live). **START** saves; the
  choice is stored on SD and restored on the next launch.

From the menu, press **START** at any time to instantly convert every new
photo (one-tap "Sync") without going through the Add list.

### Controls

**Menu**
- **Up/Down** — move the highlight
- **A** — select an item
- **START** — sync all new photos
- **SELECT** — quit the app

**Add / Remove photos**
- **Up/Down** — move the highlight (a preview loads when you pause on a photo)
- **L / R** (or D-pad Left/Right) — page back / forward through long lists
- **A** — toggle the highlighted photo's checkbox
- **Hold A + Up/Down** — drag-select: toggle each photo as you move over it
- **Y** — toggle all on/off
- **X** — expand the highlighted photo full screen
- **START** — run the action (convert / remove)
- **B** — back to the menu

**View library**
- **Up/Down** — move the highlight
- **L / R** (or D-pad Left/Right) — page through long lists
- **A** — expand / collapse the highlighted photo full screen
- **B** — back to the menu

**Completion screen** (after a convert/remove)
- **START** — back to the menu
- **SELECT** — quit the app

Converted photos are written to `sd:/_nds/TWiLightMenu/dsimenu/photos/` at
208×156, and the dsimenu theme cycles them on the top screen.

## Optional: make it feel automatic

TWiLight can launch a `.nds` on boot, or you can just run DSiPhotoSync whenever
you've taken new pics. There's no DSi API to hook "on camera photo taken," so a
quick manual launch (or boot-launch) is as close to the stock auto-cycle as
homebrew can get.

## How it works (implementation notes)

- **JPEG decode** uses [picojpeg](https://github.com/richgel999/picojpeg)
  (public domain, vendored), reading the file as a stream so it never loads a
  whole image into RAM at once. Hover previews decode in picojpeg's reduce
  mode (1/8 each axis) so scrolling stays responsive.
- **PNG output** is written by a small hand-rolled encoder using stored
  (uncompressed) deflate blocks — larger on disk than a typical PNG, but valid
  and fast to write on-device. The byte format is validated against a standard
  PNG decoder.
- **Output size** is held to **208×156** because TWiLight renders larger
  top-screen images as black.
- The UI repaints only the regions that change (dirty-flag rendering) to avoid
  flicker, and clears the framebuffer immediately at startup so no leftover
  VRAM is shown during the SD mount.

## Files

```
.
├── Makefile
├── README.md
├── LICENSE
├── .gitignore
├── icon.png            # 32x32 app icon shown in TWiLight / the DSi menu
├── icon.bmp            # 4bpp BMP the build feeds to ndstool
└── source/
    ├── main.c          # UI state machine, decode, scale, PNG, previews
    ├── font6x8.h       # bitmap font for the on-screen UI
    ├── picojpeg.c      # JPEG decoder (public domain, vendored)
    └── picojpeg.h
```

## App icon and title

The icon and the three lines of title text under it are set in the `Makefile`
via `GAME_TITLE`, `GAME_SUBTITLE1`, `GAME_SUBTITLE2`, and `GAME_ICON`. Edit
those lines to change the banner; replace `icon.png` (32×32) to change the
icon. The build converts the icon to the 4bpp BMP that `ndstool` expects.

## License

MIT — see [LICENSE](LICENSE). picojpeg is public domain (see its header); that
dedication is unaffected by this project's license.
