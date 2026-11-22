// Sound's example view (component sound; never in a release): pads for every behaviour on the left, and on
// the right a listener with sources around it (drag to move the listener). The sounds are made by
// ../../tools/sounds.py (`sounds.py example Sounds`): no one else's, so no licences.
#include "yea_sound_example.h"

#include "../../modules/sound/SoundLink.h"

#include <math.h>
#include <string.h>

namespace sound_example
{
// ---- Its sounds ----

SOUND(click, "bus interface")
#embed "Sounds/click.wav"
SOUND_END

SOUND(jump, "")
#embed "Sounds/jump.wav"
SOUND_END

SOUND(step, "range 2 30")
#embed "Sounds/step_1.wav"
SOUND_AND
#embed "Sounds/step_2.wav"
SOUND_AND
#embed "Sounds/step_3.wav"
SOUND_AND
#embed "Sounds/step_4.wav"
SOUND_END

SOUND(coin, "cooldown 0.15")
#embed "Sounds/coin.wav"
SOUND_END

SOUND(laser, "max 4 cooldown 0")
#embed "Sounds/laser.wav"
SOUND_END

SOUND(boom, "max 2 priority high")
#embed "Sounds/explosion.wav"
SOUND_END

SOUND(engine, "")
#embed "Sounds/engine.wav"
SOUND_INTRO
#embed "Sounds/engine_start.wav"
SOUND_OUTRO
#embed "Sounds/engine_stop.wav"
SOUND_END

SOUND(charge, "stop release")
#embed "Sounds/charge.wav"
SOUND_OUTRO
#embed "Sounds/charge_end.wav"
SOUND_END

SOUND(hum, "loop bus ambience fadein 1 stop fade 2")
#embed "Sounds/hum.wav"
SOUND_END

SOUND(beacon, "loop range 2 30 max 3")
#embed "Sounds/beacon.wav"
SOUND_END

SOUND(talk, "bus voice")
#embed "Sounds/babble.wav"
SOUND_END

SOUND(calm, "music bpm 96")
#embed "Sounds/calm.ogg"
SOUND_END

SOUND(chase, "music bpm 140 switch bar 0.5")
#embed "Sounds/chase.ogg"
SOUND_END

SOUND(win, "bus music")
#embed "Sounds/win.ogg"
SOUND_END

namespace
{
// ---- Drawing: the host's quads in the screen pass, and a 5x7 font ----

struct Rgb
{
    float r, g, b;
};
const Rgb kBack = { 0.035f, 0.04f, 0.05f }, kPad = { 0.12f, 0.13f, 0.15f }, kText = { 0.85f, 0.87f, 0.9f }, kDim = { 0.45f, 0.47f, 0.5f };
const Rgb kRows[] = { { 0.95f, 0.65f, 0.25f }, { 0.35f, 0.75f, 0.95f }, { 0.95f, 0.4f, 0.45f }, { 0.6f, 0.85f, 0.4f }, { 0.75f, 0.55f, 0.95f } };

constexpr uint32_t kMaxQuads = 4096;
YeaQuad            gQuads[kMaxQuads];
uint32_t           gQuadN = 0;

void quad(float x, float y, float w, float h, Rgb c, float a = 1.0f, float layer = 0.0f, float turn = 0.0f)
{
    if (gQuadN < kMaxQuads)
        gQuads[gQuadN++] = YeaQuad{ x + w * 0.5f, y + h * 0.5f, w, h, c.r, c.g, c.b, a, turn, layer, { 0, 0 } };
}

const char* const kChars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!.:-/?";
const uint8_t     kGlyphs[][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },
};
float textWidth(const char* s, float px) { return (float)strlen(s) * 6.0f * px - px; }
void  text(const char* s, float x, float y, float px, Rgb c, float layer = 3.0f)
{
    for (float left = x; *s; ++s, left += 6.0f * px)
    {
        const char* at = strchr(kChars, *s);
        if (*s == ' ' || !at)
            continue;
        const uint8_t* g = kGlyphs[at - kChars];
        for (int r = 0; r < 7; ++r)
            for (int b = 0; b < 5;)
            {
                if (!(g[r] >> (4 - b) & 1))
                {
                    ++b;
                    continue;
                }
                int e = b;
                while (e < 5 && (g[r] >> (4 - e) & 1))
                    ++e;
                quad(left + (float)b * px, y + (float)r * px, (float)(e - b) * px, px, c, 1.0f, layer);
                b = e;
            }
    }
}

// ---- The pads ----

enum Action
{
    Jump, Step, Coins, Lasers, Boom,
    Engine, Charge, Hum,
    CutNow, CutFade, CutOutro, CutRelease,
    Calm, Chase, MusicOff, Win, Talk,
    Beacons, Walk,
    ActionCount
};
struct Pad
{
    const char* label;
    int         row, col;
};
const char* const kRowNames[] = { "ONE-SHOTS", "LOOPS", "CUT THE ENGINE", "MUSIC", "3D" };
const Pad         kPads[ActionCount] = {
    { "JUMP", 0, 0 },    { "STEP", 0, 1 },     { "COIN X6", 0, 2 }, { "LASER X12", 0, 3 }, { "BOOM", 0, 4 },
    { "ENGINE", 1, 0 },  { "CHARGE", 1, 1 },   { "HUM", 1, 2 },
    { "NOW", 2, 0 },     { "FADE 2S", 2, 1 },  { "OUTRO", 2, 2 },   { "RELEASE", 2, 3 },
    { "CALM", 3, 0 },    { "CHASE", 3, 1 },    { "OFF", 3, 2 },     { "WIN", 3, 3 },       { "TALK", 3, 4 },
    { "BEACONS", 4, 0 }, { "WALK", 4, 1 },
};

// Layout, from the last frame's size.
float gW = 0, gH = 0;
float gPadW = 0, gPadH = 0, gGap = 0, gLeft = 0, gTop = 0, gRowH = 0, gPx = 0;
float gSceneX = 0, gSceneY = 0, gScale = 1; // the scene's centre (pixels) and pixels per world unit

void layout(float w, float h)
{
    gW = w, gH = h;
    gPx = floorf(fmaxf(2.0f, h / 360.0f));
    gPadW = 60.0f * gPx, gPadH = 13.0f * gPx, gGap = 3.0f * gPx;
    gLeft = 10.0f * gPx, gRowH = gPadH + 12.0f * gPx + gGap;
    gTop = (h - 5.0f * gRowH) * 0.5f;
    const float panel = gLeft + 5.0f * (gPadW + gGap);
    gSceneX = panel + (w - panel) * 0.5f, gSceneY = h * 0.5f;
    gScale = fminf(w - panel, h) / 44.0f;
}

void padRect(int i, float* x, float* y)
{
    *x = gLeft + kPads[i].col * (gPadW + gGap);
    *y = gTop + kPads[i].row * gRowH + 10.0f * gPx;
}

// Voices of one-shots and the moments pads were pressed, for lighting them (wiped by a code reload: fine).
snd::Voice gLast[ActionCount] = {};
snd::Voice gLasers[12] = {};
double     gPressed[ActionCount] = {};
int        gLasersLeft = 0, gCoinsLeft = 0;
float      gBurstIn = 0;
bool       gClickHeld = false, gDragging = false;

Demo& demo() { return Demos[0]; }

yea_float2 beaconAt(int k, float orbit)
{
    const float r = k == 0 ? 4.0f : k == 1 ? 10.0f : 17.0f, speed = k == 0 ? 0.9f : k == 1 ? -0.5f : 0.3f;
    return yea_float2{ r * cosf(orbit * speed + k * 2.1f), r * sinf(orbit * speed + k * 2.1f) };
}
yea_float2 walkerAt(float walk) { return yea_float2{ 12.0f * cosf(walk), 7.0f * sinf(walk) }; }

void press(int a)
{
    Demo& d = demo();
    gPressed[a] = yea::host()->time();
    snd::play(click);
    switch (a)
    {
    case Jump: gLast[a] = snd::play(jump); break;
    case Step: gLast[a] = snd::play(step); break;
    case Coins: gCoinsLeft = 6, gBurstIn = 0; break;
    case Lasers: gLasersLeft = 12, gBurstIn = 0; break;
    case Boom: gLast[a] = snd::play(boom); break;
    case Engine:
        if (snd::playing(d.engine))
            snd::stop(d.engine); // its declared cut-off: into the outro
        else
            d.engine = snd::play(engine);
        break;
    case Charge:
        if (snd::playing(d.charge))
            snd::stop(d.charge); // "stop release": the pass plays out, then the chord
        else
            d.charge = snd::play(charge);
        break;
    case Hum:
        if (snd::playing(d.hum))
            snd::stop(d.hum); // "stop fade 2"
        else
            d.hum = snd::play(hum);
        break;
    case CutNow: snd::stop(d.engine, snd::Now); break;
    case CutFade: snd::stop(d.engine, snd::Fade, 2.0f); break;
    case CutOutro: snd::stop(d.engine, snd::Outro); break;
    case CutRelease: snd::stop(d.engine, snd::Release); break;
    case Calm: d.music = 1; break;
    case Chase: d.music = 2; break;
    case MusicOff: d.music = 0, snd::music(); break;
    case Win: gLast[a] = snd::sting(win); break;
    case Talk: gLast[a] = snd::play(talk); break;
    case Beacons:
        if (snd::playing(d.beacon_a))
            snd::stop(d.beacon_a), snd::stop(d.beacon_b), snd::stop(d.beacon_c);
        else
        {
            const float pitch[3] = { -5.0f, 0.0f, 7.0f };
            uint32_t*   v[3] = { &d.beacon_a, &d.beacon_b, &d.beacon_c };
            for (int k = 0; k < 3; ++k)
            {
                const yea_float2 p = beaconAt(k, d.orbit);
                *v[k] = snd::play(beacon, yea_float3{ p.x, p.y, 0.0f }, 0.0f, pitch[k]);
            }
        }
        break;
    case Walk: d.walking = !d.walking, d.step_in = 0; break;
    }
}

bool lit(int a)
{
    const Demo& d = demo();
    const bool  recent = yea::host()->time() - gPressed[a] < 0.15;
    switch (a)
    {
    case Lasers:
        for (snd::Voice v : gLasers)
            if (snd::playing(v))
                return true;
        return gLasersLeft > 0;
    case Coins: return gCoinsLeft > 0 || recent;
    case Engine: return snd::playing(d.engine);
    case Charge: return snd::playing(d.charge);
    case Hum: return snd::playing(d.hum);
    case CutNow: case CutFade: case CutOutro: case CutRelease: return recent;
    case Calm: return d.music == 1;
    case Chase: return d.music == 2;
    case MusicOff: return d.music == 0;
    case Beacons: return snd::playing(d.beacon_a);
    case Walk: return d.walking != 0;
    default: return snd::playing(gLast[a]) || recent;
    }
}
} // namespace

int load(int)
{
    if (!Demos.size())
        Demos.add();
    return 0;
}

void unload() {}

void update(uint32_t phase, float dt)
{
    if (phase != YEA_PHASE_INPUT || !Demos.size())
        return;
    snd::update();
    Demo&          d = demo();
    const YeaHost* h = yea::host();
    // Input: a press on a pad, or a drag in the scene to move the listener.
    const float mx = h->input("yea_cursor_x"), my = h->input("yea_cursor_y");
    const bool  click = h->input("yea_click") > 0.5f;
    if (click && !gClickHeld && gW > 0)
    {
        for (int a = 0; a < ActionCount; ++a)
        {
            float x, y;
            padRect(a, &x, &y);
            if (mx >= x && mx < x + gPadW && my >= y && my < y + gPadH)
                press(a);
        }
        gDragging = mx > gLeft + 5.0f * (gPadW + gGap);
    }
    gClickHeld = click;
    if (!click)
        gDragging = false;
    if (gDragging && gScale > 0)
        d.listener = yea_float2{ (mx - gSceneX) / gScale, (gSceneY - my) / gScale };
    snd::listener(d.listener);
    // Bursts: coins 50 ms apart (the cooldown lets about one in three through), lasers likewise (four at most).
    gBurstIn -= dt;
    if ((gCoinsLeft || gLasersLeft) && gBurstIn <= 0)
    {
        gBurstIn = 0.05f;
        if (gCoinsLeft)
            snd::play(coin), --gCoinsLeft;
        if (gLasersLeft)
            gLasers[--gLasersLeft] = snd::play(laser);
    }
    // Music: asked for every frame, which is fine: the same music again changes nothing.
    if (d.music == 1)
        snd::music(calm);
    else if (d.music == 2)
        snd::music(chase);
    // Sources moving around the listener.
    d.orbit += dt;
    const snd::Voice beacons[3] = { d.beacon_a, d.beacon_b, d.beacon_c };
    for (int k = 0; k < 3; ++k)
    {
        const yea_float2 p = beaconAt(k, d.orbit);
        snd::move(beacons[k], p);
    }
    if (d.walking)
    {
        d.walk += dt * 0.35f;
        d.step_in -= dt;
        if (d.step_in <= 0)
            d.step_in = 0.42f, snd::play(step, walkerAt(d.walk));
    }
}

void draw(uint32_t pass, const YeaFrame* frame)
{
    if (pass != YEA_PASS_SCREEN || !Demos.size())
        return;
    layout(float(frame->width), float(frame->height));
    const Demo& d = demo();
    gQuadN = 0;
    quad(0, 0, gW, gH, kBack, 1.0f, -10.0f);
    for (int r = 0; r < 5; ++r)
        text(kRowNames[r], gLeft, gTop + r * gRowH, gPx, kDim);
    for (int a = 0; a < ActionCount; ++a)
    {
        float x, y;
        padRect(a, &x, &y);
        const bool on = lit(a);
        const Rgb  c = kRows[kPads[a].row];
        quad(x, y, gPadW, gPadH, on ? c : kPad, on ? 0.9f : 1.0f, 1.0f);
        const float px = gPx;
        text(kPads[a].label, x + (gPadW - textWidth(kPads[a].label, px)) * 0.5f, y + (gPadH - 7.0f * px) * 0.5f, px, on ? kBack : kText);
    }
    // The scene: range rings, the listener (drag it), the beacons and the walker.
    auto at = [&](yea_float2 p, float* sx, float* sy) { *sx = gSceneX + p.x * gScale, *sy = gSceneY - p.y * gScale; };
    float sx, sy;
    for (int k = 0; k < 3; ++k)
    {
        const float r = (k == 0 ? 4.0f : k == 1 ? 10.0f : 17.0f) * gScale;
        for (int s = 0; s < 64; ++s)
        {
            const float t = s * 6.2831853f / 64.0f;
            quad(gSceneX + r * cosf(t) - gPx * 0.5f, gSceneY + r * sinf(t) - gPx * 0.5f, gPx, gPx, kPad, 1.0f, 0.0f);
        }
    }
    at(d.listener, &sx, &sy);
    quad(sx - 3 * gPx, sy - 3 * gPx, 6 * gPx, 6 * gPx, kText, 1.0f, 4.0f, 0.785f);
    quad(sx - gPx, sy - 6 * gPx, 2 * gPx, 3 * gPx, kText, 1.0f, 4.0f); // facing up the screen
    const snd::Voice beacons[3] = { d.beacon_a, d.beacon_b, d.beacon_c };
    for (int k = 0; k < 3; ++k)
        if (snd::playing(beacons[k]))
        {
            at(beaconAt(k, d.orbit), &sx, &sy);
            quad(sx - 2.5f * gPx, sy - 2.5f * gPx, 5 * gPx, 5 * gPx, kRows[4], 1.0f, 3.0f);
        }
    if (d.walking)
    {
        at(walkerAt(d.walk), &sx, &sy);
        quad(sx - 2 * gPx, sy - 2 * gPx, 4 * gPx, 4 * gPx, kRows[0], 1.0f, 3.0f, 0.785f);
    }
    yea::quads(YEA_PASS_SCREEN, gQuads, gQuadN);
}
} // namespace sound_example
