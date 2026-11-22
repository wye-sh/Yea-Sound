// The Sound Editor: a panel in F1 (never in a release of the game). It lists every sound the view's game
// declared, through the sound module's own registry (snd::Api's count and info), so a sound that can play
// always shows. A press plays the selected file once on the Interface bus (snd::Api's preview: through Pause,
// outside cooldown and limits); its waveform comes from the peaks the module keeps per file. Its Volume and
// Pitch are the declaring module's own knobs. Add Sounds, and files dropped on the panel, go to the agents
// through the chat: the panel never writes anything itself.
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
char       gSearch[64] = "";
int        gBus = 0;     // 0 all, then snd::Bus + 1
uint32_t   gSel = 0;     // the sound shown (its id), 0 none
int        gFile = 0;    // its file shown
snd::Voice gPlay = 0;    // the preview playing
uint32_t   gPlayOf = 0;  // its sound
int        gMix = 0;     // Mix folded open

const char* const kBuses[] = { "All", "Effects", "Interface", "Voice", "Ambience", "Music" };
const char* const kBusKnobs[] = { "effects", "interface", "voice", "ambience", "music" };
const float       kSearchRow[2] = { 1.6f, 1.0f };

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

bool matches(const char* name)
{
    if (!gSearch[0])
        return true;
    for (const char* s = name; *s; ++s)
    {
        size_t i = 0;
        while (gSearch[i] && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)(gSearch[i] == ' ' ? '_' : gSearch[i])))
            ++i;
        if (!gSearch[i])
            return true;
    }
    return false;
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
    u->row(2, kSearchRow);
    u->field("##search", gSearch, sizeof(gSearch), "Search");
    u->upload("Add Sounds", "");
    gBus = u->tabs("##bus", kBuses, 6, gBus);
    u->space(1);

    // Every sound, in the order it was declared.
    const uint32_t n = a->count();
    bool           any = false, selShown = false;
    snd::Info      sel = {};
    for (uint32_t i = 0; i < n; ++i)
    {
        snd::Info s;
        if (!a->info(i, &s) || (gBus && s.bus + 1 != uint32_t(gBus)) || !matches(s.name))
            continue;
        snd::Wave w = {};
        char      name[64], label[96], right[32], len[16];
        a->wave(s.id, 0, &w);
        pretty(s.name, name, sizeof(name));
        snprintf(label, sizeof(label), "%s##%u", name, s.id);
        seconds(w.seconds, len, sizeof(len));
        uint32_t variations = 0;
        for (uint32_t f = 0; f < s.files; ++f)
            variations += a->wave(s.id, f, &w) && w.kind == 0 ? 1 : 0;
        if (variations > 1)
            snprintf(right, sizeof(right), "%u \xC3\x97 \xC2\xB7 %s", variations, len);
        else
            snprintf(right, sizeof(right), "%s", len);
        if (!gSel)
            gSel = s.id;
        if (u->item(label, right, s.id == gSel, s.voices > 0))
            gSel = s.id, gFile = 0;
        if (s.id == gSel)
            sel = s, selShown = true;
        any = true;
    }
    if (!any)
    {
        u->label(n ? "No Matches" : "No Sounds Yet", 1);
        return;
    }
    if (!selShown)
        return;

    // The sound chosen: its files, its waveform, Play, and its knobs.
    char name[64];
    pretty(sel.name, name, sizeof(name));
    u->space(2);
    u->heading(name);
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
