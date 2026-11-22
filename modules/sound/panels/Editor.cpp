// The Sound Editor: a panel in F1 (never in a release of the game). The sound edited first, in a dropdown of
// every sound the view's game declared (its search inside it, a playing one's dot lit), through the sound module's
// own registry (snd::Api's count and info), so a sound that can play always shows; then its version (a dropdown,
// when it has versions), its files and waveform, the transport (back to start, play and pause, repeat; a press on
// the waveform seeks), and the knobs its module declared for that version; the component's Mix after. The preview
// plays the chosen file alone on the Interface bus, as its knobs make it (snd::Api's preview: through Pause,
// outside cooldown and limits). Add Sounds (the header's +), files dropped on the panel, and More Knobs (the header's gear) go to
// the agents: the panel never writes anything.
#include "yea_sound.h"

#include "../SoundLink.h"

#include <yea/ui.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace sound
{
namespace
{
// What the panel keeps while it is open (wiped by a new version of this module, which only resets the choice).
uint32_t   gSel = 0;     // the sound shown (its id), 0 none
int        gFile = 0;    // its file shown
snd::Voice gPlay = 0;    // the preview playing
uint32_t   gPlayOf = 0;  // its sound
int        gPlayFile = -1;
int        gHeld = 0;    // the preview paused
int        gRepeat = 0;  // the preview repeats
float      gFrom = 0;    // where the next play starts (seconds): a seek while stopped
uint32_t   gVersion = 0; // the version shown (its id), 0 the sound itself
int        gMix = 0;     // Mix folded open
YeaUiRow   gVersionRows[64];
uint32_t   gVersionIds[64];
char       gVersionNames[64][64];

const char* const kBusKnobs[] = { "effects", "interface", "voice", "ambience", "music" };
constexpr uint32_t kMostSounds = 4096; // the dropdown's rows at most

// The dropdown's rows, kept (no allocation): each sound's name, and its length and variations.
YeaUiRow gRows[kMostSounds];
uint32_t gIds[kMostSounds];
char     gNames[kMostSounds][64], gRights[kMostSounds][32];

// "engine_start": "Engine Start".
void pretty(const char* in, char* out, size_t cap)
{
    size_t n = 0;
    bool   start = true;
    for (; *in && n + 1 < cap; ++in)
    {
        const char c = *in == '_' ? ' ' : *in;
        out[n++] = start ? char(toupper((unsigned char)c)) : c;
        start = c == ' ';
    }
    out[n] = 0;
}

void seconds(float s, char* out, size_t cap) { snprintf(out, cap, s < 10.0f ? "%.2f s" : "%.0f s", s); }

// The file's waveform, the part played in the accent, and the playhead at `at` (or none below 0). Returns where
// a press on it was, 0..1 (a seek), else -1.
float waveform(const YeaUi* u, const snd::Wave& w, float at)
{
    YeaUiCanvas in;
    u->canvas("##wave", 64.0f, &in);
    if (in.w <= 0.0f)
        return -1.0f;
    const float mid = in.h * 0.5f, half = in.h * 0.5f - 6.0f;
    const float seek = in.pressed && in.w > 16.0f ? (in.mx - 8.0f) / (in.w - 16.0f) : -1.0f;
    const float pick = seek < 0.0f ? -1.0f : seek > 1.0f ? 1.0f : seek;
    if (!w.peaksReady || !w.peaks)
    {
        u->line(8.0f, mid, in.w - 8.0f, mid, 1.0f, 0.878f, 0.871f, 0.957f, 0.25f);
        return pick;
    }
    const int   bars = in.w >= 256.0f * 3.0f ? 256 : in.w >= 128.0f * 3.0f ? 128 : 64, step = 256 / bars;
    const float bw = (in.w - 16.0f) / float(bars), played = w.seconds > 0.0f && at > 0.0f ? at / w.seconds : 0.0f;
    for (int b = 0; b < bars; ++b)
    {
        float lo = 0.0f, hi = 0.0f;
        for (int k = b * step; k < (b + 1) * step; ++k)
            lo = w.peaks[2 * k] < lo ? w.peaks[2 * k] : lo, hi = w.peaks[2 * k + 1] > hi ? w.peaks[2 * k + 1] : hi;
        const float x = 8.0f + bw * float(b), y0 = mid - hi * half, y1 = mid - lo * half;
        const bool  done = at > 0.0f && (float(b) + 0.5f) / float(bars) <= played;
        u->rect(x, y0, bw > 2.0f ? bw - 1.0f : bw, y1 - y0 < 1.0f ? 1.0f : y1 - y0, done ? 0.929f : 0.878f, done ? 0.878f : 0.871f, done ? 0.612f : 0.957f,
                done ? 0.9f : 0.45f, 0.0f);
    }
    if (at > 0.0f)
    {
        const float x = 8.0f + (in.w - 16.0f) * (played > 1.0f ? 1.0f : played);
        u->line(x, 4.0f, x, in.h - 4.0f, 1.0f, 0.929f, 0.878f, 0.612f, 1.0f);
    }
    return pick;
}
} // namespace

void panel_sound_editor()
{
    const YeaUi*    u = yea::ui();
    const snd::Api* a = (const snd::Api*)(uintptr_t)api(snd::kApi);
    if (!u || !a)
        return;
    u->header("Add", "Add Sounds", ""); // pressed: the file picker, then the chat with this panel

    // Every sound, in the order it was declared, in one dropdown: the one edited first, what is edited about it under it.
    const uint32_t n = a->count();
    uint32_t       rows = 0;
    int            chosen = -1;
    snd::Info      sel = {};
    for (uint32_t i = 0; i < n && rows < kMostSounds; ++i)
    {
        snd::Info s;
        if (!a->info(i, &s) || s.of) // versions: under their sound
            continue;
        snd::Wave w = {};
        char      name[48], len[16];
        a->wave(s.id, 0, &w);
        pretty(s.name, name, sizeof(name));
        snprintf(gNames[rows], sizeof(gNames[rows]), "%s##%u", name, s.id);
        seconds(w.seconds, len, sizeof(len));
        uint32_t variations = 0;
        for (uint32_t f = 0; f < s.files; ++f)
            variations += a->wave(s.id, f, &w) && w.kind == 0 ? 1 : 0;
        if (variations > 1)
            snprintf(gRights[rows], sizeof(gRights[rows]), "%u \xC3\x97 \xC2\xB7 %s", variations, len);
        else
            snprintf(gRights[rows], sizeof(gRights[rows]), "%s", len);
        gRows[rows] = { gNames[rows], gRights[rows], s.voices > 0 };
        gIds[rows] = s.id;
        if (!gSel)
            gSel = s.id;
        if (s.id == gSel)
            chosen = int(rows), sel = s;
        ++rows;
    }
    if (!rows)
    {
        u->label("No Sounds Yet", 1);
        return;
    }
    const int picked = u->dropdown("##sound", gRows, int(rows), chosen);
    if (picked != chosen && picked >= 0 && picked < int(rows))
    {
        gSel = gIds[picked], gFile = 0, gVersion = 0, gFrom = 0;
        for (uint32_t i = 0; i < n; ++i) // the one picked, edited from this frame on
            if (snd::Info s; a->info(i, &s) && s.id == gSel)
                sel = s, chosen = picked;
    }
    if (chosen < 0) // the one shown went (its module stopped): the first
    {
        gSel = gIds[0], gFile = 0, gVersion = 0;
        return;
    }

    // Its versions (the same files, knobs of their own; the game picks one): the one edited.
    uint32_t  versions = 0;
    int       vChosen = 0;
    snd::Info ed = sel; // the sound or version edited
    gVersionRows[0] = { "Base##0", nullptr, sel.voices > 0 }, gVersionIds[0] = sel.id, versions = 1;
    for (uint32_t i = 0; i < n && versions < 64; ++i)
    {
        snd::Info s;
        if (!a->info(i, &s) || s.of != sel.id)
            continue;
        pretty(s.version, gVersionNames[versions], sizeof(gVersionNames[versions]) - 12);
        snprintf(gVersionNames[versions] + strlen(gVersionNames[versions]), 12, "##%u", s.id);
        gVersionRows[versions] = { gVersionNames[versions], nullptr, s.voices > 0 };
        gVersionIds[versions] = s.id;
        if (s.id == gVersion)
            vChosen = int(versions), ed = s;
        ++versions;
    }
    if (versions > 1)
    {
        const int v = u->dropdown("##version", gVersionRows, int(versions), vChosen);
        if (v != vChosen && v >= 0 && v < int(versions))
        {
            gVersion = v ? gVersionIds[v] : 0, gFrom = 0;
            for (uint32_t i = 0; i < n; ++i)
                if (snd::Info s; a->info(i, &s) && s.id == gVersionIds[v])
                    ed = s;
        }
    }
    else
        gVersion = 0;

    // Its files, its waveform and the transport.
    if (ed.files > 1)
    {
        const char* names[16];
        char        buf[16][12];
        const int   files = ed.files < 16 ? int(ed.files) : 16;
        int         v = 0;
        for (int f = 0; f < files; ++f)
        {
            snd::Wave w = {};
            a->wave(ed.id, uint32_t(f), &w);
            if (w.kind == 1)
                snprintf(buf[f], sizeof(buf[f]), "Intro");
            else if (w.kind == 2)
                snprintf(buf[f], sizeof(buf[f]), "Outro");
            else
                snprintf(buf[f], sizeof(buf[f]), "%d", ++v);
            names[f] = buf[f];
        }
        const int was = gFile;
        gFile = u->tabs("##file", names, files, gFile < files ? gFile : 0);
        if (gFile != was)
            gFrom = 0;
    }
    snd::Wave w = {};
    a->wave(ed.id, uint32_t(gFile), &w);
    const bool  mine = gPlay && gPlayOf == ed.id && gPlayFile == gFile;
    const bool  playing = mine && a->playing(gPlay);
    if (gPlay && !a->playing(gPlay)) // it played out: back to the start
        gPlay = 0, gHeld = 0, gFrom = 0;
    const float at = playing ? a->position(gPlay) : gFrom;
    const float seek = waveform(u, w, at > 0.0f ? at : -1.0f);
    const uint32_t flags = (gRepeat ? 1u : 0u) | (gHeld ? 2u : 0u);
    auto           start = [&](float from) {
        gPlay = a->preview(ed.id, uint32_t(gFile), from, flags), gPlayOf = ed.id, gPlayFile = gFile, gFrom = from;
        if (!gPlay)
            gHeld = 0;
    };
    if (seek >= 0.0f) // a press on the waveform: the preview goes there (playing, or from there when it next plays)
    {
        if (playing)
            start(seek * w.seconds);
        else
            gFrom = seek * w.seconds;
    }
    const YeaUiIcon buttons[3] = { { "BackToStart", nullptr, "Back to Start", nullptr, -1 },
                                   { "Resume", "Pause", "Play", "Pause", playing && !gHeld ? 1 : 0 },
                                   { "Repeat", nullptr, "Repeat", nullptr, gRepeat } };
    switch (u->icons("##transport", buttons, 3))
    {
    case 0:
        if (playing)
            start(0.0f);
        else
            gFrom = 0;
        break;
    case 1:
        if (!playing)
            gHeld = 0, start(gFrom >= w.seconds ? 0.0f : gFrom);
        else
            gHeld = !gHeld, a->hold(gPlay, gHeld);
        break;
    case 2:
        gRepeat = !gRepeat;
        if (playing)
            a->repeat(gPlay, gRepeat);
        break;
    default: break;
    }
    char pos[48], len[16], now[16];
    seconds(w.seconds, len, sizeof(len));
    seconds(at, now, sizeof(now));
    snprintf(pos, sizeof(pos), "%s / %s", now, len);
    u->label(pos, 1);

    // Its knobs, as its module declared them for this version; the others it could have go to the agents.
    char     base[96], knob[160], missing[512] = "";
    uint32_t more = 0;
    snprintf(base, sizeof(base), "%s.%s%s%s", ed.owner, ed.name, ed.version[0] ? "_" : "", ed.version);
    u->space(1);
    for (uint32_t k = 0; k < snd::kKnobs; ++k)
    {
        if (ed.knobMask & (1u << k))
        {
            snprintf(knob, sizeof(knob), "%s##%s_%s", snd::kKnobSpecs[k].title, base, snd::kKnobSpecs[k].suffix);
            u->knob(knob);
        }
        else if (strlen(missing) + 24 < sizeof(missing))
            snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s\"%s\"", more++ ? "," : "", snd::kKnobSpecs[k].suffix);
    }
    if (more && u->header("Settings", "More Knobs", nullptr)) // the others it could have: asked of the agents
    {
        char json[1024];
        snprintf(json, sizeof(json), "{\"module\":\"%s\",\"sound\":\"%s\",\"version\":\"%s\",\"knobs\":[%s]}", ed.owner, ed.name, ed.version,
                 missing);
        yea::send("Sound Editor: More Knobs", json);
    }

    // The component's own mix: the master volume and each bus.
    u->space(2);
    u->fold("Mix", &gMix);
    if (gMix)
    {
        u->knob("Volume##volume");
        for (const char* b : kBusKnobs)
            u->knob(b);
    }
}
} // namespace sound
