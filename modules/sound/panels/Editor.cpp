// The Sound Editor: a panel in F1 (never in a release of the game). The sound edited first, in a dropdown of
// every sound the view's game declared (its search inside it, a playing one's dot lit), through the sound module's
// own registry (snd::Api's count and info), so a sound that can play always shows; right under it what is edited
// about it: its files and waveform, Play, and its Volume and Pitch, the declaring module's own knobs; the
// component's Mix after. A press plays the chosen file once on the Interface bus (snd::Api's preview: through
// Pause, outside cooldown and limits); its waveform comes from the peaks the module keeps per file. Add Sounds (the
// header's +), and files dropped on the panel, go to the agents through the chat: the panel never writes anything.
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
int        gMix = 0;     // Mix folded open

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

// The file's waveform, the part played in the accent, and where the preview is. true: pressed.
bool waveform(const YeaUi* u, const snd::Wave& w, float at)
{
    YeaUiCanvas in;
    u->canvas("##wave", 64.0f, &in);
    if (in.w <= 0.0f)
        return false;
    const float mid = in.h * 0.5f, half = in.h * 0.5f - 6.0f;
    if (!w.peaksReady || !w.peaks)
    {
        u->line(8.0f, mid, in.w - 8.0f, mid, 1.0f, 0.878f, 0.871f, 0.957f, 0.25f);
        return in.pressed != 0;
    }
    const int   bars = in.w >= 256.0f * 3.0f ? 256 : in.w >= 128.0f * 3.0f ? 128 : 64, step = 256 / bars;
    const float bw = (in.w - 16.0f) / float(bars), played = w.seconds > 0.0f ? at / w.seconds : 0.0f;
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
    return in.pressed != 0;
}
} // namespace

void panel_sound_editor()
{
    const YeaUi*    u = yea::ui();
    const snd::Api* a = (const snd::Api*)(uintptr_t)api(snd::kApi);
    if (!u || !a)
        return;
    u->add("Add Sounds", "");

    // Every sound, in the order it was declared, in one dropdown: the one edited first, what is edited about it under it.
    const uint32_t n = a->count();
    uint32_t       rows = 0;
    int            chosen = -1;
    snd::Info      sel = {};
    for (uint32_t i = 0; i < n && rows < kMostSounds; ++i)
    {
        snd::Info s;
        if (!a->info(i, &s))
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
        gSel = gIds[picked], gFile = 0;
        for (uint32_t i = 0; i < n; ++i) // the one picked, edited from this frame on
            if (snd::Info s; a->info(i, &s) && s.id == gSel)
                sel = s, chosen = picked;
    }
    if (chosen < 0) // the one shown went (its module stopped): the first
    {
        gSel = gIds[0], gFile = 0;
        return;
    }

    // The sound chosen: its files, its waveform, Play, and its knobs.
    if (sel.files > 1)
    {
        const char* names[16];
        char        buf[16][12];
        const int   files = sel.files < 16 ? int(sel.files) : 16;
        int         v = 0;
        for (int f = 0; f < files; ++f)
        {
            snd::Wave w = {};
            a->wave(sel.id, uint32_t(f), &w);
            if (w.kind == 1)
                snprintf(buf[f], sizeof(buf[f]), "Intro");
            else if (w.kind == 2)
                snprintf(buf[f], sizeof(buf[f]), "Outro");
            else
                snprintf(buf[f], sizeof(buf[f]), "%d", ++v);
            names[f] = buf[f];
        }
        gFile = u->tabs("##file", names, files, gFile < files ? gFile : 0);
    }
    snd::Wave w = {};
    a->wave(sel.id, uint32_t(gFile), &w);
    const bool playing = gPlay && gPlayOf == sel.id && a->playing(gPlay);
    const float at = playing ? a->position(gPlay) : 0.0f;
    const bool  pressed = waveform(u, w, at); // a press on it plays it from the start, as Play does
    char pos[48], len[16], now[16];
    seconds(w.seconds, len, sizeof(len));
    seconds(at, now, sizeof(now));
    snprintf(pos, sizeof(pos), "%s / %s", now, len);
    u->row(2, nullptr);
    if (u->button(playing ? "Stop##play" : "Play##play", 0, 1) || pressed)
    {
        if (playing && !pressed)
            a->stop(gPlay, snd::Now, 0.0f), gPlay = 0;
        else
            gPlay = a->preview(sel.id, uint32_t(gFile)), gPlayOf = sel.id;
    }
    u->label(pos, 1);
    char knob[96];
    snprintf(knob, sizeof(knob), "Volume##%s.%s_volume", sel.owner, sel.name);
    u->knob(knob);
    snprintf(knob, sizeof(knob), "Pitch##%s.%s_pitch", sel.owner, sel.name);
    u->knob(knob);

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
