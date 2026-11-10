# TB Copicat IC300 — modern Mac version

The same plugin as [TB-Copicat](https://github.com/tonybuzzcock/TB-Copicat)
(same tape echo model, panel and presets), built for current Macs:

- **CLAP, VST3 and AU (AUv2)**: Reaper, FL Studio, Logic, GarageBand, Ableton Live and others
- **Universal**: native on Apple Silicon and Intel, **macOS 11 Big Sur and later**
- Built and tested on GitHub Actions, including Apple's `auval`, `pluginval` and `clap-validator`
- Personal, non-commercial use

The plugin is written once as a CLAP (`Source/Plugin`); [clap-wrapper](https://github.com/free-audio/clap-wrapper)
builds the VST3 and AU from the same code, so all three behave identically.
Settings are saved with your DAW session, and the plugin reports its latency
(40 samples) and a tail for the repeats.

## Install

Download the latest green run under **Actions → Artifacts → TB-Copicat-IC300-mac**
(or a Release), unzip it, and follow `INSTALL.txt`: copy the formats you want into
`~/Library/Audio/Plug-Ins/VST3`, `.../Components` or `.../CLAP`, then run the one
Terminal command it gives to clear macOS's download quarantine (the plugins are
ad-hoc signed, not notarised).

## Controls

| # | Name | Control | Range |
|---|------|---------|-------|
| 1 | Gain | Input gain (stands in for the four input pots) | 0 – 10 |
| 2 | Swell | Echo level | 0 – 10 |
| 3 | Sustain | Repeats: about 4 at 5, about 10 at 7, long and steady at 9, runs away near 9.5 | 0 – 10 |
| 4–6 | Head 1/2/3 | Playback head buttons, any combination: 1 = 70 ms, 2 = 220 ms, 3 = 330 ms | In / Out |
| 7 | Tone | Tone switch (the 4n7 across the heads): On is darker | Off / On |
| 8 | Echo | Echo in/out | In / Out |
| 9 | Speed | *plugin only*: tape speed trim, shown as button 1's echo time | 95 – 52 ms |
| 10 | Wow & Flutter | *plugin only*: wow and flutter | 0 – 10 |
| 11 | Tape Age | *plugin only*: worn tape: duller, more hiss and dropouts, a bigger splice bump | 0 – 10 |
| 12 | Output | *plugin only*: output trim | ±24 dB |

## The panel

800 × 600 (4:3): Gain on the left, an animated tape loop over the three head
buttons in the middle (it runs at the Speed setting and wavers with Wow &
Flutter; each head lights when its button is in), Swell and Sustain on the
right, Tone and Echo below. The strip along the bottom has the tape-transport
knobs with the presets and readout underneath.

Drag knobs up/down (hold Shift for fine control), double-click a knob to reset
it, or use the scroll wheel. Click a head button to latch it in (orange) or out.
Rocker switches: press the top half for On/In, the bottom half for Off/Out.

**Presets:** click SAVE, then a slot 1–8. Click a saved slot to recall it.
Presets are shared by every instance and all three formats, in
`~/Library/Application Support/TB/TB Copicat IC300/Presets.txt`.

## What's modelled

See [TB-Copicat](https://github.com/tonybuzzcock/TB-Copicat) for the full notes:
the IC300's circuit from its drawing (input stage, record amp with Sustain
feedback, three series playback heads with the Tone 4n7, playback EQ, output
mixer), plus a tape loop with saturation, losses, wow and flutter, dropouts and
the splice. The head times and tape speed are estimates, set by ear against a real unit.

## Build

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

On Linux this builds the CLAP and the tests; on a Mac also the VST3 and AU.
