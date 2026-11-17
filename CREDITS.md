# Credits

NOOK's own code (app.c, dsp/, tests/, tools/) is new, under the MIT licence.

| Project | Author | Licence | Used for |
| --- | --- | --- | --- |
| [fm1-base](https://github.com/charlesvestal/fm1-base) | Charles Vestal | MPL-2.0 | the platform: screen, fonts, panel, audio, MIDI, storage, simulator, browser emulator, build tools (git submodule) |
| [felucca-bsp](https://github.com/hugelton/felucca-bsp) | Leo Kuroshita (Hügelton Instruments) and contributors | MPL-2.0 | the hardware layer, the update loader, packaging |
| JieLi AC79 SDK | JieLi Technology | Apache-2.0 | three files in each `.fwsc` package (fetched at build time, never copied here) |

[OSR-1 / Blop](https://github.com/OSR-Experiment/OSR-1) by Obsimo was the reference for how a firmware sits on
fm1-base: the project layout, drawing the screen in two 240 × 120 bands, and building the browser emulator with Zig.
No code was copied from it.
