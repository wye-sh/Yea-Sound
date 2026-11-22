// Baked knobs (snd::Knob from KnobSpeed on): a sound's file rendered again with them, once per set of values,
// on the main thread, into this module's own malloc. Trim, speed (WSOLA: tempo at the same pitch), bass and
// treble shelves, echo, reverb, then fades. A loop's echo and reverb tails wrap round onto its start, so it
// still loops seamlessly; a loop's own pass gets no fades (its intro and outro do).
#include "Bake.h"

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace sound
{
namespace
{
constexpr float kPi = 3.14159265f;

struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    // warm: a loop's pass, run once for its state first, so its start continues from its end
    void  run(float* x, uint64_t n, uint32_t ch, uint32_t c, bool warm) const
    {
        float z1 = 0, z2 = 0;
        for (uint64_t i = 0; warm && i < n; ++i)
        {
            const float in = x[i * ch + c], y = b0 * in + z1;
            z1 = b1 * in - a1 * y + z2, z2 = b2 * in - a2 * y;
        }
        for (uint64_t i = 0; i < n; ++i)
        {
            const float in = x[i * ch + c], y = b0 * in + z1;
            z1 = b1 * in - a1 * y + z2, z2 = b2 * in - a2 * y;
            x[i * ch + c] = y;
        }
    }
};

// RBJ shelves, slope 1.
Biquad shelf(float hz, float db, bool high, float rate)
{
    const float A = powf(10.0f, db / 40.0f), w = 2.0f * kPi * std::min(hz, 0.45f * rate) / rate, c = cosf(w), s = sinf(w);
    const float alpha = s * 0.70710678f, k = 2.0f * sqrtf(A) * alpha;
    float       b0, b1, b2, a0, a1, a2;
    if (high)
    {
        b0 = A * ((A + 1) + (A - 1) * c + k), b1 = -2 * A * ((A - 1) + (A + 1) * c), b2 = A * ((A + 1) + (A - 1) * c - k);
        a0 = (A + 1) - (A - 1) * c + k, a1 = 2 * ((A - 1) - (A + 1) * c), a2 = (A + 1) - (A - 1) * c - k;
    }
    else
    {
        b0 = A * ((A + 1) - (A - 1) * c + k), b1 = 2 * A * ((A - 1) - (A + 1) * c), b2 = A * ((A + 1) - (A - 1) * c - k);
        a0 = (A + 1) + (A - 1) * c + k, a1 = -2 * ((A - 1) + (A + 1) * c), a2 = (A + 1) + (A - 1) * c - k;
    }
    return Biquad{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// WSOLA: windows of the input laid at half-window steps of the output, each taken where it best continues
// the last (searched within a quarter window of where the tempo puts it), so the pitch stays.
float* stretch(const float* x, uint64_t n, uint32_t ch, uint32_t rate, float speed, uint64_t* outN)
{
    const int64_t N = rate >= 32000 ? 1024 : 512, H = N / 2, tol = N / 4;
    const int64_t m = std::max<int64_t>(1, int64_t(double(n) / speed));
    float*        out = (float*)calloc(size_t(m + N) * ch, sizeof(float));
    float*        sum = (float*)calloc(size_t(m + N), sizeof(float));
    if (!out || !sum)
        return free(out), free(sum), nullptr;
    auto guide = [&](int64_t i) {
        if (i < 0 || i >= int64_t(n))
            return 0.0f;
        float g = 0;
        for (uint32_t c = 0; c < ch; ++c)
            g += x[i * ch + c];
        return g;
    };
    int64_t prev = 0;
    for (int64_t o = 0; o < m; o += H)
    {
        int64_t at = int64_t(double(o) * speed);
        if (o > 0)
        {
            // Where the last window's natural continuation goes on: the candidate most like it wins.
            const int64_t want = prev + H;
            float         best = -1e30f;
            int64_t       pick = at;
            for (int64_t d = -tol; d <= tol; d += 2)
            {
                const int64_t c = at + d;
                if (c < 0 || c >= int64_t(n))
                    continue;
                float score = 0;
                for (int64_t j = 0; j < H; j += 2)
                    score += guide(want + j) * guide(c + j);
                if (score > best)
                    best = score, pick = c;
            }
            at = pick;
        }
        for (int64_t j = 0; j < N && o + j < m + N; ++j)
        {
            const float w = 0.5f - 0.5f * cosf(2.0f * kPi * float(j) / float(N));
            sum[o + j] += w;
            if (at + j < int64_t(n))
                for (uint32_t c = 0; c < ch; ++c)
                    out[(o + j) * ch + c] += w * x[(at + j) * ch + c];
        }
        prev = at;
    }
    for (int64_t i = 0; i < m; ++i)
        for (uint32_t c = 0; c < ch; ++c)
            out[i * ch + c] = sum[i] > 1e-3f ? out[i * ch + c] / sum[i] : 0.0f;
    free(sum);
    *outN = uint64_t(m);
    return out;
}

// Adds a tail of `tail` frames: the echo's repeats and the reverb's room. In place into y (n + tail frames).
void echo(float* y, uint64_t n, uint64_t tail, uint32_t ch, uint32_t rate, float amount)
{
    const uint64_t d = uint64_t(0.28f * rate);
    const float    fb = 0.3f + 0.3f * amount, wet = 0.6f * amount;
    float*         line = (float*)calloc(d * ch, sizeof(float));
    if (!line)
        return;
    for (uint64_t i = 0, k = 0; i < n + tail; ++i, k = (k + 1) % d)
        for (uint32_t c = 0; c < ch; ++c)
        {
            const float in = i < n ? y[i * ch + c] : 0.0f, out = line[k * ch + c];
            line[k * ch + c] = in + fb * out;
            y[i * ch + c] = in + wet * out;
        }
    free(line);
}

void reverb(float* y, uint64_t n, uint64_t tail, uint32_t ch, uint32_t rate, float amount)
{
    // Freeverb's shape: parallel damped combs, then allpasses, per channel (the right one a little longer).
    static const int kComb[6] = { 1116, 1188, 1277, 1356, 1422, 1491 }, kAll[2] = { 556, 441 };
    const float      scale = float(rate) / 44100.0f, room = 0.7f + 0.25f * amount, damp = 0.3f, wet = 0.45f * amount;
    for (uint32_t c = 0; c < ch; ++c)
    {
        float* comb[6];
        float* all[2];
        int    cl[6], al[2], ci[6] = {}, ai[2] = {};
        float  store[6] = {};
        bool   ok = true;
        for (int k = 0; k < 6; ++k)
            cl[k] = std::max(1, int((kComb[k] + 23 * int(c)) * scale)), ok &= (comb[k] = (float*)calloc(cl[k], sizeof(float))) != nullptr;
        for (int k = 0; k < 2; ++k)
            al[k] = std::max(1, int((kAll[k] + 23 * int(c)) * scale)), ok &= (all[k] = (float*)calloc(al[k], sizeof(float))) != nullptr;
        for (uint64_t i = 0; ok && i < n + tail; ++i)
        {
            const float in = (i < n ? y[i * ch + c] : 0.0f) * 0.02f;
            float       out = 0;
            for (int k = 0; k < 6; ++k)
            {
                const float o = comb[k][ci[k]];
                store[k] = o * (1 - damp) + store[k] * damp;
                comb[k][ci[k]] = in + store[k] * room;
                ci[k] = (ci[k] + 1) % cl[k];
                out += o;
            }
            for (int k = 0; k < 2; ++k)
            {
                const float b = all[k][ai[k]];
                all[k][ai[k]] = out + b * 0.5f;
                out = b - out;
                ai[k] = (ai[k] + 1) % al[k];
            }
            y[i * ch + c] = (i < n ? y[i * ch + c] : 0.0f) + wet * out;
        }
        for (int k = 0; k < 6; ++k)
            free(comb[k]);
        for (int k = 0; k < 2; ++k)
            free(all[k]);
    }
}
} // namespace

bool baked(const float* v)
{
    for (uint32_t k = 0; k < kBaked; ++k)
        if (v[k] != kBakedDefaults[k])
            return true;
    return false;
}

bool bake(const mix::Buffer& in, const float* v, uint32_t role, mix::Buffer* out)
{
    const uint32_t ch = in.channels, rate = in.rate;
    const float    speed = std::max(0.25f, std::min(4.0f, v[0] == v[0] ? v[0] : 1.0f));
    auto           at = [](float x, float lo, float hi) { return x == x ? std::max(lo, std::min(hi, x)) : 0.0f; };
    const float    bass = at(v[1], -24, 24), treble = at(v[2], -24, 24), echoAmount = at(v[3], 0, 1), roomAmount = at(v[4], 0, 1);
    const float    trimStart = at(v[5], 0, 3600), trimEnd = at(v[6], 0, 3600), fadeIn = at(v[7], 0, 3600), fadeOut = at(v[8], 0, 3600);
    const bool     loop = role & BakeLoop;

    // Trim, keeping at least 10 ms.
    const uint64_t keep = std::max<uint64_t>(1, rate / 100);
    uint64_t       a = std::min<uint64_t>(in.count, uint64_t(trimStart * rate)), b = in.count - std::min<uint64_t>(in.count, uint64_t(trimEnd * rate));
    if (b < a + keep)
        b = std::min<uint64_t>(in.count, a + keep), a = b > keep ? std::min(a, b - keep) : 0;
    uint64_t n = b - a;
    float*   x = (float*)malloc(n * ch * sizeof(float));
    if (!x)
        return false;
    memcpy(x, in.frames + a * ch, n * ch * sizeof(float));

    if (fabsf(speed - 1.0f) > 1e-3f && !loop)
    {
        uint64_t m = 0;
        float*   y = stretch(x, n, ch, rate, speed, &m);
        free(x);
        if (!y)
            return false;
        x = y, n = m;
    }
    else if (fabsf(speed - 1.0f) > 1e-3f)
    {
        // A loop: three passes stretched as one, the middle kept, so its end flows on into its start.
        float* three = (float*)malloc(3 * n * ch * sizeof(float));
        if (!three)
            return free(x), false;
        for (int k = 0; k < 3; ++k)
            memcpy(three + k * n * ch, x, n * ch * sizeof(float));
        uint64_t m = 0;
        float*   y = stretch(three, 3 * n, ch, rate, speed, &m);
        free(three);
        const uint64_t one = std::max<uint64_t>(1, uint64_t(double(n) / speed + 0.5));
        float*         mid = y && m >= 2 * one ? (float*)malloc(one * ch * sizeof(float)) : nullptr;
        if (mid)
        {
            memcpy(mid, y + one * ch, one * ch * sizeof(float));
            // Its last 20 ms eased into the frames that lead into its start, so the seam is continuous.
            const uint64_t ease = std::min<uint64_t>(one / 4, rate / 50);
            for (uint64_t i = 0; i < ease; ++i)
                for (uint32_t c = 0; c < ch; ++c)
                {
                    const float t = float(i + 1) / float(ease + 1);
                    float&      e = mid[(one - ease + i) * ch + c];
                    e = e * (1.0f - t) + y[(one - ease + i) * ch + c] * t; // into what comes just before its start
                }
        }
        free(y);
        free(x);
        if (!mid)
            return false;
        x = mid, n = one;
    }
    for (uint32_t c = 0; c < ch; ++c)
    {
        if (bass != 0.0f)
            shelf(200.0f, bass, false, float(rate)).run(x, n, ch, c, loop);
        if (treble != 0.0f)
            shelf(4000.0f, treble, true, float(rate)).run(x, n, ch, c, loop);
    }

    // Tails: the echo's repeats till 60 dB down, the reverb's room; at most 4 s.
    float seconds = 0;
    if (echoAmount > 0)
        seconds += 0.28f * logf(0.001f / (0.6f * echoAmount)) / logf(0.3f + 0.3f * echoAmount);
    if (roomAmount > 0)
        seconds += 0.6f + 2.4f * roomAmount;
    const uint64_t tail = uint64_t(std::min(4.0f, std::max(0.0f, seconds)) * rate);
    if (tail)
    {
        float* y = (float*)realloc(x, (n + tail) * ch * sizeof(float));
        if (!y)
            return free(x), false;
        x = y;
        memset(x + n * ch, 0, tail * ch * sizeof(float));
        if (echoAmount > 0)
            echo(x, n, tail, ch, rate, echoAmount);
        if (roomAmount > 0)
            reverb(x, n, tail, ch, rate, roomAmount);
        const uint64_t last = std::min<uint64_t>(tail, uint64_t(0.3f * rate)); // the cap: the tail's end fades
        for (uint64_t i = 0; i < last; ++i)
            for (uint32_t c = 0; c < ch; ++c)
                x[(n + tail - 1 - i) * ch + c] *= float(i) / float(last);
        if (loop)
            for (uint64_t i = 0; i < tail; ++i) // round onto its start: seamless
                for (uint32_t c = 0; c < ch; ++c)
                    x[(i % n) * ch + c] += x[(n + i) * ch + c];
        else
            n += tail;
    }

    // Fades, on a curve that sounds even (a quarter sine).
    if (!loop || (role & (BakeFirst | BakeLast)))
    {
        const uint64_t fi = (role & BakeFirst) ? std::min<uint64_t>(n, uint64_t(fadeIn * rate)) : 0;
        const uint64_t fo = (role & BakeLast) ? std::min<uint64_t>(n, uint64_t(fadeOut * rate)) : 0;
        for (uint64_t i = 0; i < fi; ++i)
            for (uint32_t c = 0; c < ch; ++c)
                x[i * ch + c] *= sinf(0.5f * kPi * float(i) / float(fi));
        for (uint64_t i = 0; i < fo; ++i)
            for (uint32_t c = 0; c < ch; ++c)
                x[(n - 1 - i) * ch + c] *= sinf(0.5f * kPi * float(i) / float(fo));
    }
    *out = mix::Buffer{ x, uint32_t(std::min<uint64_t>(n, 0xffffffffu)), ch, rate };
    return true;
}
} // namespace sound
