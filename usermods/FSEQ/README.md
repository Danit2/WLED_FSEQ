# ✨ FSEQ Player Usermod for WLED

> Original concept based on work by **Andrej Chrcek**  
> Extended and redesigned for improved **WLED**, **SD card**, and **FPP** integration.

This usermod adds support for playing **FSEQ animation files** directly from an **SD card** in WLED.

It provides:

- a dedicated **FSEQ Player** effect
- a **Web UI** for browsing files on the SD card
- support for **WLED presets** and **boot presets**
- **FPP override support**, so an external FPP controller can take priority over local playback

---

# Features

## FSEQ Player effect

A new effect called **FSEQ Player** is added to WLED.

The effect uses:

- **Speed slider** → selects the **FSEQ file index**
- **Checkbox 1** → enables **Loop playback**

This means you can select a file by its index and optionally enable looping.

---

## Automatic FSEQ file indexing

All `.fseq` files on the SD card are scanned automatically.

Files are:

- filtered by extension
- sorted **alphabetically**
- assigned an **index number**

Example:

| Index | File |
|------:|------|
| 0 | `01-snow.fseq` |
| 1 | `02-christmas.fseq` |
| 2 | `03-candycane.fseq` |

The selected index is used by the **FSEQ Player** effect.

---

# Web UI

Open the interface:

```
http://<WLED-IP>/fsequi
```

<img width="792" height="206" alt="image" src="https://github.com/user-attachments/assets/e58693ec-afe8-4c28-8343-5b3382cd10ef" />

The page contains two file sections.

---

## 1️⃣ FSEQ files

All `.fseq` files found on the SD card are listed together with their index.

Example:

```
0 - 01-snow.fseq
1 - 02-christmas.fseq
2 - 03-candycane.fseq
```

These numbers correspond directly to the **effect index slider**.

---

## 2️⃣ Other files

All non-FSEQ files are listed in a separate section below.

This keeps animation files easy to find while still allowing normal SD card browsing.

---

## 3️⃣ Instructions

At the bottom of the page a short explanation is shown describing how the system works.

---

# Usage

## Playing a FSEQ file

1. Select effect **FSEQ Player**
2. Set the **speed slider** to the desired index
3. Enable **Checkbox 1** if you want the file to loop

Example:

| Setting | Value |
|--------|------|
| Effect | `FSEQ Player` |
| Index | `0` |
| Loop | enabled |

This will play:

```
01-snow.fseq
```

---

# Preset Support

The FSEQ Player works with normal **WLED presets**.

Presets store:

- selected effect
- index value
- loop state
- segment configuration

Example preset setup:

| Preset | Segment 1 | Segment 2 |
|--------|-----------|-----------|
| Snow/Christmas | `01-snow.fseq` | `02-christmas.fseq` |

---

# Boot Playback

To automatically start an animation after boot:

1. configure the FSEQ Player effect
2. save the setup as a preset
3. assign the preset as **Boot preset**

Location in WLED:

```
Config → LED Preferences → Boot preset
```

After reboot the animation will start automatically.

---

# Multi-Segment Support

Each segment can run its own FSEQ animation.

Example:

| Segment | Effect | Index | File |
|--------|--------|------:|------|
| Segment 1 | FSEQ Player | 0 | `01-snow.fseq` |
| Segment 2 | FSEQ Player | 1 | `02-christmas.fseq` |

This configuration can be stored as a preset.

---

# FPP Integration

This usermod supports **FPP (Falcon Player)** remote control.

When an FPP controller sends commands:

- FPP playback **overrides the local effect**
- WLED switches to **FPP controlled playback**

When FPP stops sending commands:

- WLED automatically returns to the previous local state

This allows WLED to behave as a lightweight **FPP Player device**.

---

# Recommended File Naming

Because file selection is **index based**, file order matters.

Files are sorted alphabetically.

Recommended naming style:

```
01-snow.fseq
02-christmas.fseq
03-candycane.fseq
10-finale.fseq
```

This keeps the index order stable.

---

# Important Note

If you:

- rename files
- add files between existing names
- delete files

the index order may change.

If this happens, presets referencing those indices may need to be updated.

---

# Requirements

- **ESP32**
- **WLED**
- **SD card**
- **sd_card usermod enabled**

Supported storage modes:

- SD SPI
- SD MMC

---

# Example Workflow

## Play animation locally

1. Upload `.fseq` files to the SD card
2. Open `/fsequi`
3. Check the file list and index
4. Select effect **FSEQ Player**
5. Set the index slider
6. Enable loop if needed
7. Save as preset if desired

---

## Start animation on boot

1. Configure the effect
2. Save preset
3. Set preset as boot preset

---

## Use WLED with FPP

1. Store `.fseq` files on the SD card
2. Allow FPP to send play/sync commands
3. While FPP is active it overrides local playback
4. When FPP stops the local preset resumes

---

# Web UI Structure

The Web UI contains three parts:

1. **FSEQ file list**
2. **Other SD files**
3. **Usage instructions**

This keeps the interface clean and easy to understand.

---

# Troubleshooting

### Wrong file plays

The file index may have changed.

Check `/fsequi` and adjust the preset.

---

### Preset loads wrong animation

File order may have changed.

Use numbered filenames and recreate the preset.

---

### Nothing plays

Check:

- SD card mounted
- `.fseq` files exist
- correct index selected
- file listed in `/fsequi`

---

### Loop not working

Make sure **Checkbox 1** is enabled in the effect settings.

---

# Limitations

- File selection is **index based**
- Index order depends on **alphabetical sorting**
- Changing the file list can change preset behavior

---

# Credits

Original concept and work by:

**Andrej Chrcek**

Further development and redesign for improved WLED integration.

---

# Suggested Naming Convention

```
01-snow.fseq
02-christmas.fseq
03-candycane.fseq
04-mega-tree.fseq
05-finale.fseq
```

This keeps file indices predictable.

---

# Summary

This usermod allows WLED to:

- play `.fseq` files directly from SD
- select animations by index using the **FSEQ Player effect**
- display indexed files in a dedicated Web UI
- store playback in **presets**
- start animations automatically via **boot preset**
- allow **FPP override** for remote control
