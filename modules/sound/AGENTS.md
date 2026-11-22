# sound

The component's one module (see `../../AGENTS.md`).

- `SoundLink.h` (registers a module's sounds in the order declared): what modules that play sounds include: `snd::Api` (the one export, `api`, returns it), the `SOUND` macros, and `snd::play` and the rest. Raise `snd::kApi` with any change to `Api` or the structs it takes, and the component's major version with it.
- `Sound.cpp` (main thread): the registry (`gDefs`, a sound per owner and name, keyed by its options and files), options (`parse`), the policy of each play (`playDef`: cooldown, limit and stealing, variations, jitter), files (`clipFor`: decoded whole or kept to stream; peaks for a panel), streams decoded in `draw` (`topUp`), the device (opened last in `load`, closed first in `unload`), F1's Pause (`draw` without `update` in the same frame), and table `Voice`: loops and music written at `unload` and resumed by the next version once their sounds are registered again (only in the same run of Yea).
- `Mixer.cpp` (the device's thread): voices, buses, ducking, the limiter. Yea's crash guard doesn't cover this thread, so it stays small: it reads only commands and memory the main thread keeps alive for it, checks every index, and never calls the host, allocates, locks or waits.
- `panels/Editor.cpp`: the Sound Editor (`panel Sound Editor` in `module.yea`), only through `snd::Api` and `yea::ui()`; development only (a release of the game doesn't build `panels/`). Its state (search, bus, sound and file chosen, the preview) is plain globals: a new version only resets the choice.
- `Codec.cpp`: miniaudio and stb_vorbis compiled once (`Miniaudio.h` has the options: device, decoders, no MP3, no engine).
- Pitfalls: the export runs in the calling module's guard and heap, so nothing here uses `new` or the host's state: `malloc`, freed by `unload` at the latest. A buffer is freed only after its sound's voices have ended (`gDefVoices`) and no stream decoder holds it.
