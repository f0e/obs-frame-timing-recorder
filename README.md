# obs-frame-timing-recorder

An OBS plugin that logs when each recorded frame was really drawn, so
[blur](https://github.com/f0e/blur) can interpolate recordings on the game's real timeline instead of
assuming their frames are evenly spaced.

Every recording and saved replay gets a `<file>.frametiming` file next to it. For blur to use it, keep it
next to the video. If something moves your videos, move the log with them.

- **Recordings:** the log is opened beside the recording and appended to as it runs, so a recording of any
  length is covered, and finished when the recording stops or splits into a new file. If OBS remuxes
  recordings automatically, the log is also written under the remuxed file's name.
- **Replays:** the log is written when the replay is saved.

## What it touches

Nothing in the game.

- Timing comes from OBS itself and from Windows' event tracing, which is what PresentMon and similar tools
  use.
- Process names come from a process list snapshot, which doesn't open the game.
- OBS's own capture hook is unchanged.

## Setup

1. Install the plugin: copy `obs-frame-timing-recorder.dll` to
   `C:\ProgramData\obs-studio\plugins\obs-frame-timing-recorder\bin\64bit\`, or build and install it (below).
2. Restart OBS. Its log (Help → Log Files) should say `[obs-frame-timing-recorder] loaded`.
3. Add the filter: right-click your **Game Capture** (or **Window Capture**) source → **Filters** → **+**
   under Effect Filters → **Frame Timing Probe**. It has to go on the capture source itself: that's both
   what the plugin measures and how it knows which process is the game. Display Capture isn't supported -
   it doesn't belong to one process.
4. Record and save replays as usual.

## Permissions (optional, recommended)

To see when the game draws its frames, the plugin needs permission to use Windows' event tracing. That's the
most useful part of the log when the game runs at a higher framerate than you record at. Without it, the
plugin still logs everything else, and blur can still correct for OBS itself falling behind.

If the plugin doesn't have permission, OBS shows a warning when it starts, and its log says why. You can turn
the warning off from the warning itself.

Either of these gives it permission:

### Option 1: run OBS as administrator

Right-click OBS → **Run as administrator**. To make it permanent: right-click the OBS shortcut →
**Properties** → **Compatibility** → tick **Run this program as an administrator**.

This also lets OBS raise its own GPU priority, which OBS recommends for games that keep the GPU busy anyway.

### Option 2: join the "Performance Log Users" group

This lets your normal account trace events, so OBS doesn't need to run as administrator. Setting it up needs
administrator rights once:

1. Open PowerShell **as administrator** and run:

   ```powershell
   Add-LocalGroupMember -Group "Performance Log Users" -Member $env:USERNAME
   ```

   Or, in an administrator Command Prompt:

   ```bat
   net localgroup "Performance Log Users" %USERNAME% /add
   ```

   On Windows Pro you can also use **Computer Management** (`lusrmgr.msc`) → Groups → Performance Log Users →
   Add.

2. **Sign out of Windows and back in**, or restart. Group changes only apply to new sign-ins.

To undo it, run `Remove-LocalGroupMember -Group "Performance Log Users" -Member $env:USERNAME` as
administrator.

### Warnings

Warnings you've turned off are remembered in
`%APPDATA%\obs-studio\plugin_config\obs-frame-timing-recorder\settings.json`. Delete that file to see them
again.

The plugin warns when:

- it has no permission to trace the game's frames (when OBS starts)
- a recording or replay is saved without the Frame Timing Probe filter having measured anything

## How it works

1. **Ticks.** A tick callback logs every OBS video tick: its QPC time, `obs_get_video_frame_time()` and the
   total and lagged frame counters.
2. **When OBS read the picture.** The probe filter follows the game capture's draw with an
   `ID3D11DeviceContext3::Flush1` carrying an event, which the GPU sets once it has run everything flushed up
   to there, the draw included. A worker thread timestamps those events, so the graphics thread never waits
   on the GPU. Only the first render of each tick counts - the later ones are previews and projectors.
3. **When the game drew its frames.** An event tracing session runs PresentMon's `PresentData`, which reports
   every present with its CPU start, time in present, GPU start and ready times, and the simulation start the
   game reports (Intel PresentMon markers or NVIDIA Reflex) where there is one, and when it reached the
   screen. Only the game matters, so the plugin asks the source the probe is on which process it captured
   (both Game Capture and Window Capture answer), and logs that process's presents and nothing else.
4. **Which file frame is which tick.** Every encoded video packet is logged with its size and timestamps. The
   saved file is matched to the log afterwards by its sequence of packet sizes, because OBS gives no signal
   that ties a saved file to the frames it holds.

## Sidecar format

Version 6, little endian. Blur reads only this version.

- **Header (40 bytes):** `BLURFTIM`, u32 version, u32 game timing status (0 tracing, 1 no permission, 2
  failed, 3 not started), i64 QPC frequency, i64 QPC everything in the log is relative to, u32 fps num, u32
  fps den.
- **Batches:** the rest of the file is batches, each an 8-byte header (4-byte tag, u32 count) followed by its
  records. A replay writes one batch per tag; a recording appends more as it goes, so a tag turns up once per
  batch written and the reader joins them. The records are the structs in `src/records.hpp`, all 8-byte
  fields:
  - `TICK`: qpc, frame_time, total_frames, lagged_frames.
  - `READ`: frame_time, the QPC the flush after the draw was submitted, and the QPC the GPU reported it
    finished (0 if it never did).
  - `PCKT`: pts, dts, dts_usec, sys_dts_usec, size, keyframe, cts, fer, ferc, received_qpc.
  - `PRES`: present_start, time_in_present, gpu_start, ready, gpu_duration, swap_chain, process_id, runtime,
    present_mode, final_state, flags, app_sim_start, reflex_sim_start, screen_time, window.
  - `GAME`: process_id, capture kind (0 game capture, 1 window capture, 2 neither), the captured window,
    then a 120-byte UTF-8 exe name - one for each game captured while the log was running.

A replay's log covers the replay buffer's length plus 10 seconds; a recording's covers the whole file. The
logs kept in memory hold about 12 minutes at 360fps, which is why a recording's log is written as it runs
rather than at the end.

## Building

Needs Visual Studio (2022 or 2026), CMake 3.28+ and Windows 10 SDK 10.0.20348 or newer. The dependencies
(OBS sources and prebuilt libraries) are downloaded on the first configure.

```powershell
cmake --preset windows-x64          # or windows-vs2026-x64 for a Visual Studio 2026 toolset
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo
```

The install goes to `C:\ProgramData\obs-studio\plugins\obs-frame-timing-recorder\`, where OBS finds it.

## Third-party code

`third_party/presentmon` is the `PresentData` library from
[PresentMon](https://github.com/GameTechDev/PresentMon) (MIT, see its `LICENSE.txt`), at the commit in
`third_party/presentmon/COMMIT`. PresentData uses a few pieces of PresentMon's much larger utility library.
The files under `IntelPresentMon/CommonUtilities` cover those pieces: `Hash`, `Meta.h`,
`SampleStatistics.h` and `Qpc.h` are copied as they are, and the rest (logging, exceptions, the precision
waiter, `Shim.cpp`) are small stand-ins.
