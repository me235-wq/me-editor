# Video Editor (working title)

Minimalist, fast, CapCut-style video editor for **Linux and Windows**.
C++17 · Qt 6 (Widgets + OpenGL) · FFmpeg (libav* for preview, `ffmpeg` for export).

## Milestone 5 (Batch 3): text, tracks, copy/paste, timeline previews, proxies

**Text** (`T`, the *Text* toolbar button, or Edit → Add text): a text clip on a video track. In the inspector: the text itself (multi-line), **font**, **size**, **bold / italic / underline**, fill colour, **outline** width + colour. Position, size, rotation and opacity work like any picture, with ◇ keyframes, and you move / resize / rotate text directly in the preview with the same box. Effects (shadow, border, blur, shake, swoosh, black flash ...) and the mask work on text too. Text is drawn by the same renderer for preview and export.

**Tracks:** right-click a track header → *Add video/audio track above/below*, *Delete track* (asks first if it has clips; undoable). Edit menu: *Add video track* (on top) / *Add audio track* (at the bottom). Up to 12 of each; one of each kind always stays; video tracks stay above audio; tracks are renumbered (V3 V2 V1 / A1 A2 ...), clips follow their track. The timeline scrolls when there are many tracks.

**Copy / paste:** Ctrl+C, Ctrl+X, Ctrl+V, and Ctrl+D (duplicate). Pastes at the playhead on the same tracks, keeping the clips' spacing, effects, keyframes and masks; if something is in the way, the group slides to the first free spot. Locked tracks are never pasted onto.

**Timeline previews:** video clips show a **filmstrip of thumbnails** and a **waveform strip** along the bottom; audio clips show a full-height waveform. They follow trims and speed changes, and are made in the background after import.

**Automatic proxies** (File → *Automatic preview proxies*, on by default): only when playback of a heavy video (1440p or more) can't keep up for two seconds in a row, a light 720p copy is built in the background and used for the *preview only*; the status bar tells you. Export always reads the original. Copies are kept between sessions.

## Direct manipulation in the preview (CapCut-style)
Select a clip (in the timeline, or just click the picture in the preview) and a thin box appears on it:
- **Move:** drag inside the box. It snaps gently to the canvas centre lines (a dashed guide shows while snapped).
- **Resize:** drag a corner or edge handle. Size changes proportionally about the centre (the editor has one uniform size, not separate width/height).
- **Rotate:** drag the round handle above the box. It snaps to 15° steps (hold Shift to always snap).
- **Live:** the preview updates while you drag, and Position X/Y, Size and Rotation in the inspector update at the same time. Typing a value in the inspector moves the box. Each drag is a single undo step.
- **Keyframes:** a value that is animated (gold ◇) gets a keyframe at the playhead when you drag; values that are not animated just change. Dragging never touches the other properties.
- Only the single selected picture can be manipulated; it is hidden for locked or hidden tracks, effect/adjustment layers, audio, and when the playhead is outside the clip. The box shows the clip's own position/size/rotation (not shake or swoosh), so it stays steady while you edit.
- Text objects use the same box.

## Fixes after Batch 2
- **Mirror** is now a single clip effect that flips the picture left ↔ right (about the picture's own centre). No direction options, not available as a layer.
- **Black flash** now flickers rhythmically between the normal picture and dark frames: flashes per second, how much of each flash is dark, edge softness (0 = hard cut), *steady* or *double* flash pattern, plus a glitch (horizontal tearing + red/blue split) that rides along with every flash. Each flash tears differently.
- **Chroma key** compares real colour (brightness counts a little), so only the colour you pick (and close shades of it) disappears; other colours, greys and whites stay. *Pick the colour to remove from the preview* is an eyedropper: click it, then click the colour in the picture.
- **Free values:** every number can be typed freely or stepped by 1 (0.1 for seconds). Size, shake strength and the others can go down to 0; ranges are much wider (position ±20000 px, speed 0.1×–20×, volume up to 400 %). Mouse-wheel over the inspector scrolls it instead of changing a value.
- **Keyframes can be moved:** drag a gold diamond on a clip sideways to retime it (every value keyed at that moment moves with it; it snaps to frames and to the playhead; undo restores it).
- **Audio:** the sound code was reworked (device and thread handling, a format fallback, error messages) and **File → Test audio output** plays a test tone and tells you which device it used or what went wrong.

## Milestone 4 (Batch 2): effects, masks, adjustments

**Two ways to use every effect:** drop it **on a clip** (it affects only that picture) or, for blur / mirror / black flash / adjustments, as a **separate layer** on a track (it affects *everything below it* on the timeline, like in CapCut and After Effects). Layers are purple (effects) or gold (adjustment) bars; drag their edges to choose where they apply. The new top video track **V3** is meant for them.

| Effect | Clip | Layer | Strength 0–100 means | Extra controls |
|---|---|---|---|---|
| Drop shadow | yes | – | shadow opacity | offset X / Y, softness, colour |
| Border | yes | – | thickness | colour |
| Blur | yes | yes | blur radius | (use the mask to choose where) |
| Mirror | yes | no | one switch: flips the picture left ↔ right (top and bottom stay) |
| Chroma key | yes | no | key amount (100 = fully removed) | key colour (pick it with the eyedropper on the preview), tolerance, edge softness, spill removal |
| Black flash | yes | yes | darkness | flashes per second, dark part of each flash, edge softness, pattern (steady flicker / double flash), glitch, start time |
| Adjustment | – | yes | overall amount | temperature, tint, saturation, exposure, contrast, highlights, shadows, sharpen, clarity, vignette |

**Rectangle mask** (every picture, effect layer and adjustment layer): position, width, height, rotation, feather, invert. On a picture it crops it; on a layer it limits where the effect applies (e.g. blur just a small part). The mask is drawn as a yellow outline on the preview while you edit it.

**Keyframes everywhere:** every number above (strength, every effect value, every mask value, plus the earlier position/size/rotation/opacity/volume/shake) has a ◇ button.

**One renderer:** all of this runs in the same GPU compositor for preview and export, so the export matches what you see. Notes: mirror and masks work on the canvas (not on the clip's own edges); the border is an inner stroke along the picture's edge, so it frames the picture when it is smaller than the canvas; shadow likewise needs a picture smaller than the canvas to show. Without OpenGL 3.3 the preview and export fall back to CPU, which shows/uses only position, size, rotation and opacity (no effects).

**Tests:** the self-test renders synthetic images through the compositor and checks exact pixels for the mask (incl. feather, invert, rotation), blur (with and without mask), mirror (all modes), chroma key, black flash, shadow, border, every adjustment, and the "effect layer only affects what is below it" rule; plus a real export through an adjustment layer and a black-flash layer. `-DVE_BUILD_UI_TESTS=ON` builds `ve_uismoke`, which drives the real window.

## Milestone 3 (Batch 1): editing foundation

**Timeline**
- Select clips by clicking, Ctrl/Shift-click, **dragging a box** over empty space, or Ctrl+A. Clicking one clip of a group narrows the selection to it.
- Every track has three buttons: **hide** (eye: not rendered, not heard), **lock** (nothing on it can be selected, moved, split, deleted or dropped onto), **mute** (audio off, picture stays). All undoable.
- Keyframes show as small gold diamonds on the clip.

**Inspector (select a clip)** — every value with a **◇** button can be animated:
- Video: position X/Y, size, rotation, opacity, **speed** (0.1×–8×; the clip's length on the timeline changes, and it refuses if it would run into the next clip).
- Audio (the video's own sound, or an audio clip): volume (0–200 %, animatable), mute, fade in, fade out. Speed also applies to audio.
- Effects: shake strength is animatable; swoosh length is editable.
- **Keyframes:** click ◇ to add/remove a key at the playhead (gold outline = this value is animated; filled = a key sits here). Once a value has keys, changing it at any time creates/updates a key there. Linear interpolation, no graph. Splitting, trimming and speed changes keep keys in the right place.
- Dragging a spin box is one undo step.

**Media panel**
- **Import files**, **Import folder** (recursive, mirrors your folder structure), or drop files/folders on the window.
- Folders: *New folder*, F2 to rename, drag files/folders between folders. **Del** removes selected files/folders from the project (asks first, also removes their clips, undoable; your files on disk are never touched).
- Formats: mp4 m4v mov mkv webm avi flv wmv mpg mpeg ts mts m2ts 3gp ogv **gif** · mp3 wav aac flac ogg opus m4a wma aiff · png jpg webp bmp tiff (anything else FFmpeg can read also works via *All files*).

**Canvas / aspect ratio** (File → Canvas): YouTube 16:9, Shorts/Reels/TikTok 9:16, 1:1, 4:5, 4:3, 21:9 or **any custom ratio**, at 480p–4K, 24–60 fps.

**Audio**
- You can now **hear** the timeline while editing (needs Qt Multimedia; without it playback is silent and everything else still works).
- One mixer feeds both playback and export, so volume, fades, mute, keyframes, speed and freeze frames sound identical in the file. Freeze frames are silent and audio resumes at the correct source position (sync is unit-tested).
- Speed changes pitch (like a tape), it is not pitch-corrected yet.

**Export fix.** I could not reproduce the "thousands of hours" duration here (every test file came out at exactly the right length), so the fix is defensive:
1. Audio is rendered to a finished file of the exact length (no endless silent source is ever given to FFmpeg), video is exactly N frames, and the output is hard-capped with `-t`.
2. After every export the file's timing is checked the way media players read it (MP4 `mvhd`/`tkhd`/`mdhd` boxes + every packet). If anything is off it is repaired automatically (stream-copy remux), and if it still can't be fixed you get a clear error instead of a bad file. Tested by deliberately corrupting a header to 1,193 hours.
If it still happens for you, tell me the file types on the timeline, the encoder you picked, and the player.

## Milestone 2: shake, swoosh, freeze frame

- **Shake** (Light / Medium / Heavy): deterministic multi-frequency jitter in position + a little rotation, with motion blur, auto-zoomed so the frame edge never shows. Intensity is editable per clip.
- **Swoosh in / Swoosh out** (from/to left, right, top, bottom): the clip slides in or out with a fast ease, a 30 % zoom and a strong directional **motion blur**. Length is editable per clip (default 0.45 s). Splitting a clip hands the swoosh-in to the first half and the swoosh-out to the second.
- **Freeze frame** (`F`, or the toolbar button): splits the clip at the playhead and inserts a silent 2-second still of that exact frame; the rest of the track moves right. Drag the freeze clip's right edge to change its length.
- Apply effects by dragging a preset from the **Effects** tab onto a clip (or select a clip and double-click the preset). Edit or remove them in the inspector on the right.
- **What you see is what you export:** preview and export use the *same* OpenGL compositor (`src/media/Compositor.cpp`). Export renders every frame on the GPU and pipes it to FFmpeg together with the mixed audio.
- Without OpenGL 3.3 the preview falls back to the CPU (shows the motion, no blur) and export falls back to a pure FFmpeg path (freeze frames work, shake/swoosh are skipped, and the app tells you).

## Milestone 1: core editor

- Import video / audio / images (background import + thumbnails; drag files from your file manager onto the window)
- Multi-track timeline: V2, V1, A1, A2 · drag & drop from the media panel · move (also between tracks) · trim both edges · split · delete · **ripple delete**
- Snapping to playhead / clip edges / zero · zoom (Ctrl + wheel) · scroll (wheel)
- Unlimited undo/redo (every edit is a command; moves/trims are one undo step)
- **OpenGL preview** (every layer is a GPU texture, alpha-composited in a shader), with an automatic CPU fallback
- Hardware video decode when available (VAAPI/CUDA/VDPAU on Linux, D3D11VA/DXVA2/CUDA on Windows), silent software fallback
- Preview quality Full / 1/2 / 1/4, accurate seeking, frame stepping, play/pause
- Non-destructive project files (`.vproj`, JSON, atomic saves), autosave + crash recovery, auto-relink of moved media
- Export to MP4/MOV/MKV: H.264 / H.265 on CPU, plus NVENC / AMF / QuickSync if your FFmpeg has them. Multi-layer compositing + mixed audio, with progress and cancel
- Handles phone-video rotation metadata, variable sizes/aspect ratios (letterboxed into the project canvas), vertical presets (1080×1920)

## Shortcuts
Space play · S split · Delete delete · Shift+Delete ripple delete · ←/→ frame step · Home/End · Ctrl+Z / Ctrl+Shift+Z · Ctrl+I import · Ctrl+S save · Ctrl+E export

## Not yet built (next milestones)
- Relink dialog (missing media is detected and reported; auto-relink only checks next to the project file)
- Custom shortcuts, proxy files, render cache

## Build — Linux (Ubuntu/Debian)
```bash
sudo apt install build-essential cmake ninja-build pkg-config ffmpeg \
  qt6-base-dev libqt6opengl6-dev libgl1-mesa-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  qt6-multimedia-dev
cmake -S . -B build -G Ninja && cmake --build build
./build/videoeditor
ctest --test-dir build --output-on-failure      # engine self-test (CPU parts)
# full test including the GPU renderer (needs OpenGL: run in a desktop session, or on a headless box):
QT_QPA_PLATFORM=xcb xvfb-run -a ./build/ve_selftest
```

## Build — Windows (MSYS2, easiest)
Install MSYS2, open the **UCRT64** shell:
```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-base,qt6-multimedia,ffmpeg}
cmake -S . -B build -G Ninja && cmake --build build
windeployqt6 build/videoeditor.exe      # copies Qt DLLs next to the exe
```
Export looks for `ffmpeg.exe` next to `videoeditor.exe` first, then in PATH.
(MSVC + vcpkg also works: `vcpkg install qtbase ffmpeg`, then configure with the vcpkg toolchain file.)

## Architecture (matches your spec: UI controls the engine, never *is* the engine)
```
src/core    Project model (assets, tracks, clips), undoable commands, save/load   — no UI, no FFmpeg
src/media   Decoder (libav, HW decode + fallback), FrameServer (worker thread), Compositor (the one GPU renderer), Exporter (GPU pipe + FFmpeg fallback)
src/ui      MainWindow, TimelineWidget (custom painted), PreviewWidget (OpenGL + CPU fallback)
tests       selftest: model, undo/redo, save/load, seeking, rotation, image import, real export round-trip
```
Effects are per-clip data (`Clip::effects`) evaluated by one pure function (`evalEffects`) and drawn by one renderer (`Compositor`) — new effects slot in there and automatically appear in both preview and export.
