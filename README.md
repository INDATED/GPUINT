# GPUINT

A live GPU task manager for Windows, written in C. Black, fast, and in two languages (English and Dutch).

Een live taakbeheer voor de grafische kaart op Windows, geschreven in C. Zwart, snel, en in twee talen (Nederlands en Engels).

---

## English

### What it shows

- One panel per graphics card: load, temperature, power, video memory split by kind of process, clocks and PCIe.
- A history graph of load and video memory.
- System: CPU per thread, RAM, swap, and the processes that use the most CPU and RAM.
- All processes that use a graphics card: video memory, shared memory, paged memory, GPU load per engine (3D, copy, video, compute), runtime (D3D12, D3D11, Vulkan, CUDA and more) and RAM.
- Loaded AI model files (`.gguf`, `.safetensors` and more) behind the process that has them open.

### What it does

- Refresh speed from once per 2 seconds up to **MAX**: one sample and one frame for every frame of your display.
- Click a column head to sort. Click a process and press **STOP** (or Delete) to end it.
- Button **NL | EN** switches the language.
- No helper program and no installer. One `.exe`.

### How it measures

- Windows performance counters (GPU Engine, GPU Process Memory, GPU Adapter Memory) for memory and load per process and per card.
- NVML (`nvml.dll`, part of the NVIDIA driver) for temperature, power and clocks of an NVIDIA card.
- The Windows process list for names, RAM and CPU.
- One sample takes about 1.5 ms on the development machine.

### Requirements

- Windows 10 or 11, 64-bit.
- Any graphics card with a current WDDM driver. Temperature, power and clocks are shown for NVIDIA cards.

### Build

- Install [MSYS2](https://www.msys2.org/) with the UCRT64 toolchain (`gcc`, `windres`) in `C:\msys64`.
- Run `bouw.bat`. It makes `GPUINT.exe` and `meter_proef.exe` (prints one sample as text).

### Note

- The program is not signed. Windows SmartScreen can show a warning at the first start.
- Stopping a part of Windows can crash the PC. GPUINT asks for a second click first.

### License

[PolyForm Strict 1.0.0](LICENSE.md). You may read the code and use the program for noncommercial purposes. You may not distribute it, change it, or sell it.

Copyright (c) 2026 INDATED. The idea for a GPU task manager like this comes from [gputaskman](https://github.com/shanselman/gputaskman) by Scott Hanselman. GPUINT is separate work and contains no code from it.

---

## Nederlands

### Wat het toont

- Een paneel per grafische kaart: belasting, temperatuur, vermogen, videogeheugen per soort proces, klok en PCIe.
- Een grafiek met het verloop van belasting en videogeheugen.
- Systeem: CPU per draad, RAM, wisselgeheugen, en de processen met de meeste CPU en RAM.
- Alle processen die een grafische kaart gebruiken: videogeheugen, gedeeld geheugen, gepaged geheugen, belasting per taak (3D, copy, video, rekenen), runtime (D3D12, D3D11, Vulkan, CUDA en meer) en RAM.
- Geladen AI-modelbestanden (`.gguf`, `.safetensors` en meer) achter het proces dat ze open heeft.

### Wat het doet

- Snelheid van eens per 2 seconden tot **MAX**: een meting en een beeld bij elk beeld van je scherm.
- Klik op een kolomkop om te sorteren. Klik op een proces en dan op **STOP** (of Delete) om het te beëindigen.
- De knop **NL | EN** wisselt de taal.
- Geen hulpprogramma en geen installatie. Eén `.exe`.

### Hoe het meet

- Windows-tellers (GPU Engine, GPU Process Memory, GPU Adapter Memory) voor geheugen en belasting per proces en per kaart.
- NVML (`nvml.dll`, onderdeel van het NVIDIA-stuurprogramma) voor temperatuur, vermogen en klok van een NVIDIA-kaart.
- De proceslijst van Windows voor namen, RAM en CPU.
- Een meting kost ongeveer 1,5 ms op de pc waarop het gemaakt is.

### Wat je nodig hebt

- Windows 10 of 11, 64-bit.
- Een grafische kaart met een actueel WDDM-stuurprogramma. Temperatuur, vermogen en klok staan er voor NVIDIA-kaarten.

### Bouwen

- Installeer [MSYS2](https://www.msys2.org/) met de UCRT64-gereedschappen (`gcc`, `windres`) in `C:\msys64`.
- Start `bouw.bat`. Dat maakt `GPUINT.exe` en `meter_proef.exe` (drukt een meting af als tekst).

### Let op

- Het programma is niet ondertekend. Windows SmartScreen kan bij de eerste start een waarschuwing tonen.
- Een onderdeel van Windows stoppen kan de pc laten vastlopen. GPUINT vraagt eerst om een tweede klik.

### Licentie

[PolyForm Strict 1.0.0](LICENSE.md). Je mag de code lezen en het programma gebruiken voor niet-commerciële doelen. Je mag het niet verspreiden, aanpassen of verkopen.

Copyright (c) 2026 INDATED. Het idee voor zo'n taakbeheer komt van [gputaskman](https://github.com/shanselman/gputaskman) van Scott Hanselman. GPUINT is eigen werk en bevat geen code daarvan.
