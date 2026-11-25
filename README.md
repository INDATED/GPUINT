# GPUINT

GPU task manager for Windows and Android. Written in C.

It shows load, temperature, power and video memory of every graphics card, and which programs use it. On Windows you can stop a program from the list.

- Refresh rate from 0.5 Hz up to the refresh rate of your screen
- Dutch and English
- Standalone executable of about 0.5 MB, no dependencies

**Windows:** Win32 and GDI+. Reads the PDH counters `GPU Engine` and `GPU Process Memory`, NVML for NVIDIA cards and IGCL for Intel. One sample takes about 1.5 ms.

**Android:** a `NativeActivity` without Java. Reads `/sys/class/kgsl` (Adreno), `cpufreq`, `cpuidle` and `/sys/class/thermal`. No app list, because Android does not allow that.

## Build

- Windows: [MSYS2](https://www.msys2.org/) (UCRT64) in `C:\msys64`, then run `bouw.bat`
- Android: Android SDK with NDK 25.1.8937393 and Java, then run `android\bouw.bat`

## License

[PolyForm Strict 1.0.0](LICENSE.md): you may read the code and use the program. You may not share, change or sell it.

Idea: [gputaskman](https://github.com/shanselman/gputaskman) by Scott Hanselman. Text on Android: [stb_truetype](https://github.com/nothings/stb) by Sean Barrett.

Copyright (c) 2026 INDATED
