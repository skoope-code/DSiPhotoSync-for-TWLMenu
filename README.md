# DSiPhotoSync for TWiLight Menu++

A tiny Nintendo DSi homebrew app that restores the stock top screen photo slideshow on a [TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu) setup.

# Installation

Simple add the .nds file to the root of your SD card.

# How it works
The app scans your SD card's `DCIM` folder, decodes each photo, downscales it 
to fit TWiLight's top-screen photo limit (208×156 max), and writes a PNG into:

    sd:/_nds/TWiLightMenu/dsimenu/photos - the folder TWiLight's `dsimenu` theme auto-cycles on the top screen.

> **Scope:** this is built for **DSi-taken photos** (640×480, shot to SD).
> It is not a general camera-photo converter; large phone/DSLR JPEGs are out
> of scope and oversized files are skipped.

## What it does *not* do

- It does **not** read the internal NAND Photo app store. Shoot to SD so the JPEGs land in `DCIM`.
- It does **not** delete or alter your original photos.

## Using the app

When launched, DSiPhotoSync shows a menu with the following options:

- **Add photos** — choose which new JPEGs to convert
- **Remove photos** — choose already-converted photos to delete from the
  slideshow folder
- **View library** — browse converted photos with previews
- **Settings** — choose whether the UI shows on the top or bottom screen

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

## Resources used

- **JPEG decode** uses [picojpeg](https://github.com/richgel999/picojpeg)
  (public domain, vendored), reading the file as a stream so it never loads a
  whole image into RAM at once. Hover previews decode in picojpeg's reduce
  mode (1/8 each axis) so scrolling stays responsive.
- **Compiled with** [devkitPro](https://github.com/devkitPro/libnds)
