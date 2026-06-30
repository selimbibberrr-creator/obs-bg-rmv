# OBS Smart Matte

[![Build OBS Smart Matte](https://github.com/selimbibberrr-creator/obs-bg-rmv/actions/workflows/build.yml/badge.svg)](https://github.com/selimbibberrr-creator/obs-bg-rmv/actions/workflows/build.yml)

OBS Smart Matte is a native OBS Studio video filter for realtime background removal with a non-destructive manual mask editor.

**Current version: 0.3.0 technical preview**

## What is included in 0.3

- GPU-rendered adaptive background matte.
- Automatic background sampling from the four image corners.
- Threshold, feather, edge shift and center-subject protection.
- Spatial smoothing and motion-aware temporal stabilization.
- Green-spill reduction and mask inversion.
- Preview modes for the final mask, automatic mask and manual mask.
- Dedicated OBS dock with three brush tools:
  - **Erase**: always removes the painted area.
  - **Restore**: clears local manual corrections.
  - **Protect**: always retains the painted area.
- Brush size, hardness and opacity.
- Undo, redo, clear, zoom, pan and fit-to-view.
- A separate persistent PNG mask for every filter instance.
- English and Dutch UI strings.
- Windows x64 build and release packaging through GitHub Actions.

## Important scope note

Version 0.3 uses a fast adaptive colour-distance matte. It works best with a mostly uniform or static background and with the subject away from the image corners. The manual editor is designed to correct difficult regions.

A neural person-segmentation backend is planned as the next major processing upgrade. The current repository does not pretend that the adaptive matte is an AI model.

## Install a GitHub Actions build

1. Open **Actions** in this repository.
2. Open the newest successful **Build OBS Smart Matte** run.
3. Download `obs-smart-matte-0.3.0-windows-x64` under **Artifacts**.
4. Extract the ZIP.
5. Close OBS Studio.
6. Copy the extracted folders into the OBS Studio installation directory, normally:
   `C:\Program Files\obs-studio`
7. Start OBS Studio again.

Tagged versions also appear as downloadable ZIP files under **Releases**.

## Use the filter

1. Add a camera or video source in OBS.
2. Open **Filters** for that source.
3. Add **OBS Smart Matte** under **Effect Filters**.
4. Start with **Automatic + manual corrections**.
5. Tune **Background tolerance** and **Edge feather** first.
6. Open **Docks → Smart Matte Mask Editor**.
7. Paint unwanted regions with **Erase** and important regions with **Protect**.
8. Use **Restore** to remove a local correction.

### Suggested tuning order

1. Background tolerance
2. Edge feather
3. Center protection
4. Edge shift
5. Spatial smoothing
6. Temporal smoothing
7. Motion sensitivity
8. Despill

## Build architecture

```text
OBS source texture
      ↓
Adaptive GPU matte shader
      ↓
Spatial + temporal stabilization
      ↓
Manual erase/protect texture
      ↓
Transparent filtered output
```

The mask editor runs in the OBS frontend through Qt. Rendering stays on the OBS graphics path. Every filter instance owns its shader state, temporal history and persistent manual mask.

## Build locally

The project is overlaid onto the official OBS plugin template by CI. For local development:

1. Clone this repository.
2. Clone `obsproject/obs-plugintemplate`.
3. Replace the template's `CMakeLists.txt`, `buildspec.json`, `src` and `data` with this repository's versions.
4. Follow the official template build instructions for your platform.

The current automated package target is Windows x64. The source and CMake structure are prepared for later Linux and macOS jobs.

## Version roadmap

- **0.0** — native filter lifecycle, shader loading, settings and logging.
- **0.1** — realtime automatic matte and transparent output.
- **0.2** — edge controls, spatial cleanup, temporal stabilization and previews.
- **0.3** — persistent erase, restore and protect editor with undo/redo.
- **Next** — neural segmentation backend, improved live preview and tracked corrections.

## Known limitations

- The adaptive matte can fail when the background has many colours or resembles the subject.
- Automatic corner sampling assumes the subject does not cover all four corners.
- The editor currently displays the correction mask rather than a live camera composite.
- Windows x64 is the only packaged target in 0.3.
- The build baseline is the OBS version pinned in `buildspec.json`; newer OBS versions still require practical compatibility testing.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
