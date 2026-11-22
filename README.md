# Sound

Sound effects and music for any game, with the conventions of professional game audio built in:

- **Variations**: several files for one sound play shuffled, never the same twice in a row, each a little higher or lower and louder or softer.
- **Limits**: a sound plays at most so many times at once (the oldest gives way), and not again within a moment of itself, so a burst never turns into a roar.
- **Loops** with an optional intro and outro, such as an engine that starts, runs and winds down.
- **Clean cut-offs**: at once, fading, into the outro, or at the end of the loop, never with a click.
- **Buses**: Music, Effects, Interface, Voice and Ambience, each with its own volume; voices lower the music while they speak.
- **Music** that changes on the next bar with a crossfade, and short flourishes that land on the beat.
- **3D**: sounds placed in the world, panned and quieter with distance from the listener.

Every sound gets a Volume and a Pitch knob. Sound plays only in its view, and F1's Pause holds it.

`tools/sounds.py` makes placeholder sounds and chiptune music from nothing, free to use anywhere.
