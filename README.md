<img width="1280" height="334" alt="Group 21-3" src="https://github.com/user-attachments/assets/d81a0f1d-e561-4a24-ad9d-66839e0acad4" />



# DSiPhotoSync for TWiLight Menu++

A tiny Nintendo DSi homebrew app that restores the stock top-screen slide show on a [TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu)
setup. DISCLAIMER: This app is in no way affiliated with TWiLightMenu++

It scans the SD card's `DCIM` folder (where the DSi camera writes JPEGs when
you shoot to SD), decodes each photo, downscales it to fit TWiLight's
top-screen photo limit (208×156 max), and writes a PNG into:

    sd:/_nds/TWiLightMenu/dsimenu/photos/

…which is the folder TWiLight's `dsimenu` theme auto-cycles on the top screen.
A photo counts as already synced once its output PNG exists, so each run only
processes new shots — and deleting a PNG from that folder makes the next run
regenerate it.

> **Scope:** this is built for **DSi-taken photos** (640×480, shot to SD).
> It is not a general camera-photo converter; large phone/DSLR JPEGs are out
> of scope and oversized files are skipped.

<img width="846" height="523" alt="Group 23-3" src="https://github.com/user-attachments/assets/a53304fd-1ba7-488a-9c82-36b616809959" />


## What it does *not* do

- It does **not** read the internal NAND Photo app store (those are stored in
  the encrypted DSi photo format). Shoot to SD so the JPEGs land in `DCIM`.
- It does **not** delete or alter your original photos. Removing a photo only
  deletes the generated PNG from the slideshow folder; the original JPEG stays
  put, so anything you remove can be added back later.

## Using the app

When launched, DSiPhotoSync shows a menu on the bottom screen. The top screen
shows a "Riddle of the day" while you're in the menus and list views, and
switches to a full controls legend when you're browsing a grid.

The menu has three items:

- **Add photos** — pick which new JPEGs to sync (all checked by default),
  then **START** to sync the checked ones.
- **View gallery** — browse already-synced photos; expand one full screen,
  remove one, or remove all.
- **Settings** — set your default view and toggle Quick mode.

From the menu, press **START** at any time to instantly sync every new photo
(one-tap "Sync new") without going through the Add list.

### List view and grid view

Both **Add photos** and **View gallery** can be browsed two ways, and you can
switch between them at any time with **SELECT**:

- **List view** — one photo per row with a preview on the right.
- **Grid view** — a 3×2 grid of thumbnails per page.

Which one opens first is set by **Default view** in Settings. Thumbnails load
progressively in the background, and recently seen pages stay cached so paging
is instant.

### Quick mode

Turn **Quick mode** on in Settings and the app changes its boot behaviour: on
launch it immediately syncs every new photo, shows a single "Syncing…" line
while it works, then exits straight back to TWiLight Menu++ — no menu, no
browsing. It's the grab-and-go option for "I just took some photos, put them
on the carousel."

To get the normal menu while Quick mode is on, **hold SELECT during boot**.

## Controls

**Menu**
- **Up/Down** — move the highlight
- **A** — open the highlighted item
- **START** — sync all new photos
- **SELECT** — quit the app

**Add photos**
- **Up/Down** — move the highlight (in list view a preview loads when you pause)
- **L / R** (or D-pad Left/Right) — page back / forward
- **A** — toggle the highlighted photo's checkbox
- **Hold A + Up/Down** — drag-select: toggle each photo as you move over it
- **Y** — toggle all on/off
- **X** — expand the highlighted photo full screen
- **SELECT** — switch between list and grid view
- **START** — sync the checked photos
- **B** — back to the menu

**View gallery**
- **Up/Down** — move the highlight
- **L / R** (or D-pad Left/Right) — page back / forward
- **A** — expand / collapse the highlighted photo full screen
- **X** — remove the highlighted photo (with confirmation)
- **Y** — remove all photos (with confirmation)
- **SELECT** — switch between list and grid view
- **B** — back to the menu

**Settings**
- **Up/Down** — move between settings
- **Left/Right** — change the highlighted setting
- **START** — save and return to the menu
- **B** — back without saving

An asterisk (`*`) appears next to any setting you've changed since opening the
screen, so it's clear what will be saved.

**Boot**
- **Hold SELECT** — skip Quick mode and open the normal menu (only relevant
  when Quick mode is on)

Synced photos are written to `sd:/_nds/TWiLightMenu/dsimenu/photos/` at 208×156,
and the dsimenu theme cycles them on the top screen.

## Optional: make it feel automatic

TWiLight can launch a `.nds` on boot, or you can just run DSiPhotoSync whenever
you've taken new pics. There's no DSi API to hook "on camera photo taken," so a
quick manual launch (or boot-launch) is as close to the stock auto-cycle as
homebrew can get. Pairing a boot-launch with **Quick mode** gets you closest:
power on, photos sync, you're back at the menu.

## How it works (implementation notes)

- **JPEG decode** uses [picojpeg](https://github.com/richgel999/picojpeg)
  (public domain, vendored), reading the file as a stream so it never loads a
  whole image into RAM at once. Full syncs decode at full resolution; hover
  previews and grid thumbnails decode in picojpeg's reduce mode (1/8 each axis)
  so browsing stays responsive.
- **PNG output** is written by a small hand-rolled encoder using stored
  (uncompressed) deflate blocks — larger on disk than a typical PNG, but valid
  and fast to write on-device. The byte format is validated against a standard
  PNG decoder.
- **Output size** is held to **208×156** because TWiLight renders larger
  top-screen images as black.
- **Both LCDs** are used: the bottom screen is the interactive UI, and the top
  screen shows the riddle (in menus / list views) or the controls legend (in
  grid views).
- **Thumbnails** are cached in a sliding window (a set kept around the photo
  you're viewing) and decoded a little at a time per frame, so opening a grid
  and paging through it never stalls input.
- The UI repaints only the regions that change (dirty-flag rendering) to avoid
  flicker. At startup both screens are held black while everything initialises,
  then revealed in one step so the first thing you see is the finished menu —
  no partial fill or flash. Full-screen images (expanded photos) are composed
  off-screen and pushed in one pass so they appear all at once.

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
    ├── main.c          # UI state machine, decode, scale, PNG, thumbnails
    ├── font6x8.h       # bitmap font for the on-screen UI
    ├── picojpeg.c      # JPEG decoder (public domain, vendored)
    └── picojpeg.h
```

## Settings file

Settings are stored on the SD card at:

    sd:/_nds/TWiLightMenu/dsimenu/dsiphotosync.ini

It holds two keys — `default_grid` (0 = list, 1 = grid) and `quick_mode`
(0 = off, 1 = on) — and is recreated with defaults if missing.

## App icon and title

The icon and the three lines of title text under it are set in the `Makefile`
via `GAME_TITLE`, `GAME_SUBTITLE1`, `GAME_SUBTITLE2`, and `GAME_ICON`. Edit
those lines to change the banner; replace `icon.png` (32×32) to change the
icon. The build converts the icon to the 4bpp BMP that `ndstool` expects.

## License

GPL3 — see [LICENSE](LICENSE). picojpeg is public domain (see its header); that
dedication is unaffected by this project's license.
