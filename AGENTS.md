# sound (component)

Sound effects and music for any game. Module `sound` (the mixer); example view module `sound_example`. A feature that plays sounds uses it through a link module of its own in `modules/`, which declares its sounds and plays them.

## Rules

- Every sound a game plays is a file in a module's `Sounds/` folder, declared with `SOUND` (below) and played through `SoundLink.h`. Never play audio any other way, never add another audio library, and never keep sound files anywhere else.
- Sound belongs to its view: it plays only while the view is current and stops when the view is left or Reset (kit rule 11). This is automatic: nothing to code.
- Files: Ogg Vorbis (`.ogg`), WAV or FLAC. Never MP3: its padding breaks loops. A loop's file must loop seamlessly by itself.
- Files sent to the Sound Editor (the chat's "Panel pressed: Sound Editor"): each becomes a sound of the game's module that plays it, in its `Sounds/`, with its `SOUND` line and two knobs (an MP3 or other format converted to `.ogg`; a zip's files each; a variation of a sound already there: `SOUND_AND`). When which module, or what the sound is for, isn't clear, ask the orchestrator. Never keep a file the editor can't list: `yea publish` warns of one no `SOUND` line embeds.
- Sounds someone else made are licensed assets (kit rules 12 and 13). Placeholders: `tools/sounds.py` makes them from nothing (`python3 components/sound/tools/sounds.py list`), with no licence needed.

## Hooking it up

- The view needs one feature that uses the component: a link module in `modules/` whose `module.yea` holds only `use sound` and a preset for each of the component's knobs (`yea component presets sound` writes them). Its feature's modules are the link module and `sound`.
- Any module of the game then plays sounds of its own, so their knobs show under its own feature. Its `module.yea` gets:

```
import sound u64 api(u32 version) = 0
category Sound
setting float jump_volume 0 -40 6
setting float jump_pitch 0 -12 12
```

  and its code, inside the module's namespace:

```cpp
#include "yea_player.h"
#include "../../components/sound/modules/sound/SoundLink.h"

namespace player
{
SOUND(jump, "max 4")
#embed "Sounds/jump.wav"
SOUND_END

void update(uint32_t phase, float dt)
{
    if (phase == YEA_PHASE_INPUT)
        snd::update();
    ...
    if (jumped)
        snd::play(jump);
}
}
```

- Each sound: `SOUND(name, "options")`, one `#embed "Sounds/<file>"` line per file, `SOUND_END`. More files: `SOUND_AND` before each further variation, `SOUND_INTRO` and `SOUND_OUTRO` before a loop's intro and outro (main file first). The handle is `name`; in another `.cpp` of the module, `extern snd::Sound& name;`.
- Every sound needs its two knobs, `<name>_volume` (dB) and `<name>_pitch` (semitones), under `category Sound`: it doesn't compile without them. Make Default writes them back to these lines.
- Call `snd::update()` once per update: it registers the module's sounds (again whenever the component restarts) and sends knob changes.
- The import is optional (`= 0`): in a view without the Sound feature, the module runs silently.

## Playing

```cpp
snd::play(jump);                           // flat: music, the interface, the player's own sounds
snd::play(step, pos);                      // at a place (yea_float2 or yea_float3): panned, quieter with distance
snd::play(hit, pos, -3.0f, 2.0f);          // with extra volume (dB) and pitch (semitones)
snd::Voice v = snd::play(engine, pos);     // keep it in a table (a u32 field) to steer or stop it
snd::move(v, pos); snd::tune(v, 0, rpm * 0.002f); snd::playing(v);
snd::stop(v);                              // its declared cut-off
snd::stop(v, snd::Now); snd::stop(v, snd::Fade, 0.5f); snd::stop(v, snd::Outro); snd::stop(v, snd::Release);
snd::music(calm);                          // fine every frame; another switches on the bar; snd::music() fades it out
snd::sting(win);                           // on the music's next beat
snd::listener(cameraCentre);               // the module owning the camera, every frame (3D: at, forward, up)
snd::bus(snd::Bus::Music, -6);             // a game's own options menu, on top of the component's knobs
```

A play that is skipped (cooldown, limit, no device) returns 0, and every call on 0 or a finished voice does nothing: no checks needed. A `snd::Voice` stays valid across code reloads.

## Options

The defaults suit most sounds: write only what differs.

| Option | Meaning | Default |
| --- | --- | --- |
| `loop`, `once` | Loops, or plays once | once; loop with an intro or outro, and for music |
| `music` | The view's music (one at a time, with `snd::music`) | |
| `bus effects\|interface\|voice\|ambience\|music` | Its bus; Voice lowers Music and Ambience by 8 dB | effects; music for `music` |
| `jitter <semitones> <dB>` | Random pitch (±) and volume (down) per play | 0.5 1.5 on one-shot effects and ambience, else 0 |
| `max <n>` | At most this many at once | 8 |
| `steal oldest\|farthest\|quietest\|none` | Who gives way at `max` | one-shots oldest (placed: farthest), loops none |
| `cooldown <s>` | A play this soon after the last is skipped | 0.03 for one-shots, 0 for loops |
| `priority low\|normal\|high` | Who is heard when 64 sounds play | normal; high for music and voice |
| `range <near> <far>` | Full volume to near, silent at far (world units) | 2 40 |
| `stop now\|fade <s>\|outro\|release` | Its cut-off: at once, a fade, the outro now, or the loop's pass first | outro if it has one, else a fade (0.15 s loops, 0.05 s one-shots) |
| `fadein <s>` | Fades in | none |
| `stream`, `load` | Streams the file, or decodes it whole | streams music and files of 10 s or more |
| `bpm <n> [<beats>]` | Music's tempo and beats per bar | 120 4 |
| `switch bar\|beat\|now\|end [<s>]` | When music switches to this one, and its crossfade | bar 1 |

## The rest

- Knobs: `volume` (master, dB) and one per bus (`music`, `effects`, `interface`, `voice`, `ambience`), under Mix; each sound's own two.
- Reset: every sound stops; nothing is kept.
- F1's Pause holds every sound where it is.
- A change to the game's code keeps playing sounds playing; a sound whose line or files changed fades out and the next play uses the new one. A change to this component's own code fades everything out for a moment and resumes loops and music where they were.
- Cost: nothing per frame while silent; 64 sounds mixed at once, 256 tracked. Decoded sounds stay in memory while their view runs; music streams.
- The Sound Editor (panel, `modules/sound/panels/Editor.cpp`): a dropdown of every sound of the view in the order declared, its search inside; under it the chosen sound's files and waveform, Play (a preview on Interface, through Pause), the declaring module's Volume and Pitch knobs; then the component's Mix. Add Sounds is its header + (`header`), which sends files to the agents. It reads `snd::Api`'s `count`, `info`, `wave`, `preview` and `position`.
- Example view: `sound_example`, pads for every behaviour and a listener with sources around it.
