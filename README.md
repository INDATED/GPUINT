# GPUINT

A live GPU task manager for Windows, written in C. Black, fast, and in two languages (English and Dutch).

## What it shows

- One panel per graphics card: load, temperature, power, video memory split by kind of process, clocks and PCIe.
- A history graph of load and video memory.
- System: CPU per thread, RAM, swap, and the processes that use the most CPU and RAM.
- All processes that use a graphics card: video memory, shared memory, paged memory, GPU load per engine (3D, copy, video, compute), runtime (D3D12, D3D11, Vulkan, CUDA and more) and RAM.
- Loaded AI model files (`.gguf`, `.safetensors` and more) behind the process that has them open.

## What it does

- Refresh speed from once per 2 seconds up to **MAX**: one sample and one frame for every frame of your display.
- Click a column head to sort. Click a process and press **STOP** (or Delete) to end it.
- Button **NL | EN** switches the language of the window.
- No helper program and no installer. One `.exe`.

## How it measures

- Windows performance counters (GPU Engine, GPU Process Memory, GPU Adapter Memory) for memory and load per process and per card.
- NVML (`nvml.dll`, part of the NVIDIA driver) for temperature, power and clocks of an NVIDIA card.
- The Windows process list for names, RAM and CPU.
- One sample takes about 1.5 ms on the development machine.

## Requirements

- Windows 10 or 11, 64-bit.
- Any graphics card with a current WDDM driver. Temperature, power and clocks are shown for NVIDIA cards.

## Android

The folder `android` has GPUINT for Android: the same style, also in C, as a NativeActivity without Java.

- It shows the graphics card (load, temperature, clock, video memory, throttling), a history graph, the processor (load and clock per core), RAM, swap and temperatures.
- It needs no permissions. It reads kernel files that every app may read. Measured on a Galaxy S26 Ultra (Adreno 840).
- There is no process list. Android does not let an app see which other apps use the graphics card.
- The load of the card gets one new value per second. That is the rate of the kernel.
- Text is drawn with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain / MIT), file `android/stb_truetype.h`.

## Build

- Install [MSYS2](https://www.msys2.org/) with the UCRT64 toolchain (`gcc`, `windres`) in `C:\msys64`.
- Run `bouw.bat`. It makes `GPUINT.exe` and `meter_proef.exe` (prints one sample as text). It also makes the icons for Android.
- Android: install the Android SDK with NDK 25.1.8937393, build-tools 34.0.0 and platform android-34, and Java. Then run `android\bouw.bat`. It makes `android\uit\GPUINT.apk`, signed with the Android debug key.

The source code has Dutch names and comments.

## Note

- The program is not signed. Windows SmartScreen can show a warning at the first start.
- Stopping a part of Windows can crash the PC. GPUINT asks for a second click first.

## License

[PolyForm Strict 1.0.0](LICENSE.md). You may read the code and use the program for noncommercial purposes. You may not distribute it, change it, or sell it.

Copyright (c) 2026 INDATED. The idea for a GPU task manager like this comes from [gputaskman](https://github.com/shanselman/gputaskman) by Scott Hanselman. GPUINT is separate work and contains no code from it.
