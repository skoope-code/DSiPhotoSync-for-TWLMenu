<img width="1280" height="334" alt="Group 21-3" src="https://github.com/user-attachments/assets/d81a0f1d-e561-4a24-ad9d-66839e0acad4" />



# DSiPhotoSync for TWiLight Menu++

A tiny Nintendo DSi homebrew app that attempts to restore the seamless feel of the stock top screen slideshow functionality on a modded DSi system ([TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu)). 
> DISCLAIMER: This app is not affiliated with TWiLightMenu++ in any way.

## What this app does
Unlike the stock DS menu, TWiLightMenu++ does not source the top screen slideshow photos from the NAND or DCIM folder on the SD. Instead, photos are sourced from the following subfolder:

    sd:/_nds/TWiLightMenu/dsimenu/photos/

This is great as it allows you to add custom photos to the top screen as you please. However, this requires accessing your SD card and manually moving files. Additionally, for photos to appear correctly, they must be PNG files that are no larger than 208x156. 

**The Problem:** The DSiWare Camera app shoots JPG files at 640x480. This makes it a hassle to add photos you've taken on your DSi to the top screen photo placeholder and is one way that a modded DSi falls short of the stock experience.

**The Solution:** This app allows you to quickly convert your DSiWare Camera app photos to meet TWLMenu's requirments and add them to the correct subfolder.
> **Scope:** this is built for **DSi-taken photos** (640×480, shot to SD).
> It is not a general camera-photo converter; large phone/DSLR JPEGs are out
> of scope and oversized files are skipped.

**The Result** All of your personal photos taken on the DSi will now begin cycling through the top screen just as they do on the stock menu.

<img width="1280" height="523" alt="Group 23-4" src="https://github.com/user-attachments/assets/f5ba2db1-be8c-4119-b251-f5edbd6394ac" />

## What it does *not* do

- It does **not** read the internal NAND Photo app store. Shoot to SD so the JPEGs land in `DCIM`.
- It does **not** delete or alter your original photos. Removing a photo only
  deletes the generated PNG from the slideshow folder; the original JPEG stays
  put, so anything you remove can be added back later.

## Using the app

When launched, DSiPhotoSync shows a menu with three options:

- **Add photos** — pick which new JPEGs to sync (all checked by default).
- **View gallery** — browse, view, or remove already-synced photos.
- **Settings** — set your default view and toggle [Quick mode](line 61).

> From the main menu, you can also press **START** at any time to sync every new photo without going through the Add list.

### List view and grid view

Both **Add photos** and **View gallery** can be browsed two ways, and you can
switch between them at any time with **SELECT**:

- **List view** — one photo per row with a preview on the right.
- **Grid view** — a 3×2 grid of thumbnails per page.

Which one opens first is set by **Default view** in Settings.

### Quick mode

Turn **Quick mode** on in Settings and the app changes its boot behaviour: on
launch it immediately syncs every new photo, showing a single "Syncing…" line
while it works, then exits straight back to TWiLight Menu++ — no menu, no
browsing. It's the grab-and-go option for "I just took some photos, put them
on the carousel."

To get the normal menu while Quick mode is on, **hold SELECT during boot**.

> Optional: make it feel as close to stock as possible
>TWiLight can launch a `.nds` on boot, or you can just run DSiPhotoSync whenever you've taken new pics. I am currently not aware of any way to >hook "on camera photo taken," so a quick manual launch (or boot-launch) is as close to the stock auto-cycle as homebrew can get. Pairing a boot->launch with **Quick mode** gets you closest: power on, photos sync, you're back at the menu.

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

## Settings file

Settings are stored on the SD card at:

    sd:/_nds/TWiLightMenu/dsimenu/dsiphotosync.ini

It holds two keys — `default_grid` (0 = list, 1 = grid) and `quick_mode`
(0 = off, 1 = on) — and is recreated with defaults if missing.

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

## App icon and title

The icon and the three lines of title text under it are set in the `Makefile`
via `GAME_TITLE`, `GAME_SUBTITLE1`, `GAME_SUBTITLE2`, and `GAME_ICON`. Edit
those lines to change the banner; replace `icon.png` (32×32) to change the
icon. The build converts the icon to the 4bpp BMP that `ndstool` expects.

## License

GPL3 — see [LICENSE](LICENSE). picojpeg is public domain (see its header); that
dedication is unaffected by this project's license.
