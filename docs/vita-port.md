# Vita port foundation

This tree now has the first, deliberately small, Vita layer for the UZDoom
5.0.1 port.  It is a staging point for the selected path A: keep UZDoom's
software renderer and add a GLES/VitaGL presentation path.  It is not yet a
bootable Vita build.

## What is prepared

- `-DVITA=ON` selects a separate platform source branch in `src/CMakeLists.txt`.
- Persistent user data is rooted at `ux0:/data/uzdoom`; screenshots, saves,
  demos, cache, config and `autoexec.cfg` no longer use Unix `$HOME` paths.
- The initial Vita profile enables the software renderer and GLES2, while
  disabling Vulkan, OpenAL, updater, telemetry, PCH, OpenMP, libvpx and the
  optional FluidSynth backend.
- `USE_LIBVPX=OFF` is a real build option.  It removes the VP8/VP9 IVF path
  while retaining the other movie decoders; libvpx is not part of rendering.
- `VITAGL_ROOT` is a cache variable for the existing VitaGL checkout/library.
  The CMake hook validates `source/vitaGL.h` and links `libvitaGL.a` when both
  are supplied.

## Configure once the SDK shell is available

From the UZDoom source directory, first configure the native host tools.  This
requires a native C/C++ compiler and produces the generators needed by the
cross build (`re2c`, `lemon` and `zipdir`):

```text
cmake -S . -B build-host -G Ninja
cmake -S . -B build-vita -G Ninja -DVITA=ON -DIMPORT_EXECUTABLES=<absolute-path>/build-host/ImportExecutables.cmake -DCMAKE_TOOLCHAIN_FILE=<VITASDK>/share/vita.toolchain.cmake -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DVITAGL_ROOT=<vitaGL>
cmake --build build-vita
```

The local SDK is present and its GCC is 15.2.0; a direct C++20 syntax probe
passes.  The VitaGL checkout has now produced `libvitaGL.a` with the SDK
toolchain.  The current shell still lacks a native host C/C++ compiler, so a
complete UZDoom cross-build has not been claimed; the next infrastructure step
is to create the real `build-host/ImportExecutables.cmake` export.

## Remaining port slices before first boot

1. Replace the desktop POSIX entry/system/input assumptions with Vita-specific
   startup, paths and SDL event/input handling.
2. Replace `SDL_GL_CreateContext`/`SDL_GL_SwapWindow` with the VitaGL lifecycle
   (`vglInitExtended`, `vglStartRendering`, `vglStopRendering`).
3. Make the GLES path use VitaGL's available entry points rather than the
   desktop loader, then remove or bypass unsupported FBO/VAO/shader features
   for the software-compositor milestone.
4. Connect the software renderer's 32-bit canvas to the VitaGL presentation
   texture without freeing VitaGL-owned memory.
5. Add the Vita launcher/VPK packaging and validate cold start, WAD search,
   controls, saves and screenshots on Vita hardware.

Each step is intentionally separate so a failure in hardware rendering does
not obscure the simpler software-renderer bring-up.
