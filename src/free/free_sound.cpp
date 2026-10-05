// Sound effects and music of the free asset set, synthesised from scratch:
// oscillators, noise and envelopes, and a small tune generator.
#include <algorithm>
#include <cmath>
#include <string>
#include <initializer_list>

#include "free/free_data.hpp"

namespace ab {

namespace {

constexpr float kRate = 22050.0f;
constexpr float kTwoPi = 6.2831853f;
using Wave = std::vector<float>;

struct Random {
    std::uint32_t state;
    float next() {  // 0-1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8) & 0xFFFF) / 65535.0f;
    }
    int below(int n) { return std::min(n - 1, static_cast<int>(next() * static_cast<float>(n))); }
};

enum class Shape { Sine, Square, Triangle, Saw, Noise };

// A note whose pitch glides from f0 to f1 and whose loudness falls away.
void note(Wave& out, float start, float length, float f0, float f1, Shape shape, float volume, float attack = 0.004f, float curve = 2.0f,
          float duty = 0.5f) {
    const auto first = static_cast<std::size_t>(start * kRate), count = static_cast<std::size_t>(length * kRate);
    if (out.size() < first + count) out.resize(first + count, 0.0f);
    float phase = 0.0f;
    Random rnd{static_cast<std::uint32_t>(first) * 7919u + 17u};
    float held = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float f = f0 + (f1 - f0) * t;
        phase += f / kRate;
        phase -= std::floor(phase);
        float v = 0.0f;
        switch (shape) {
            case Shape::Sine: v = std::sin(phase * kTwoPi); break;
            case Shape::Square: v = phase < duty ? 1.0f : -1.0f; break;
            case Shape::Triangle: v = 4.0f * std::abs(phase - 0.5f) - 1.0f; break;
            case Shape::Saw: v = 2.0f * phase - 1.0f; break;
            case Shape::Noise:
                // Noise held for a time that follows the "frequency": low values rumble.
                if (i % std::max<std::size_t>(1, static_cast<std::size_t>(kRate / std::max(40.0f, f))) == 0) held = rnd.next() * 2.0f - 1.0f;
                v = held;
                break;
        }
        const float seconds = static_cast<float>(i) / kRate;
        const float env = std::min(1.0f, seconds / attack) * std::pow(1.0f - t, curve);
        out[first + i] += v * env * volume;
    }
}

float pitch(int midi) { return 440.0f * std::pow(2.0f, static_cast<float>(midi - 69) / 12.0f); }

// A loop of eight bars: bass, a wandering lead on a five-note scale, a quiet
// arpeggio and drums. Every note ends inside the loop, so it repeats cleanly.
Wave tune(std::uint32_t seed, float bpm, int root, bool minor, bool drums) {
    Random rnd{seed};
    const float beat = 60.0f / bpm, eighth = beat / 2.0f;
    const int scale[5] = {0, minor ? 3 : 2, minor ? 5 : 4, 7, minor ? 10 : 9};
    const int chords[4] = {0, minor ? -4 : 5, minor ? -2 : -3, minor ? 3 : 7};
    Wave out(static_cast<std::size_t>(32.0f * beat * kRate), 0.0f);
    int degree = 2;
    for (int bar = 0; bar < 8; ++bar) {
        const int chord = root + chords[(bar % 4)];
        for (int e = 0; e < 8; ++e) {
            const float at = (static_cast<float>(bar) * 4.0f + static_cast<float>(e) / 2.0f) * beat;
            note(out, at, eighth * 0.9f, pitch(chord - 24 + (e % 4 == 3 ? 7 : 0)), pitch(chord - 24 + (e % 4 == 3 ? 7 : 0)), Shape::Triangle, 0.30f, 0.004f, 1.0f);
            note(out, at, eighth * 0.45f, pitch(chord + scale[(e * 2) % 5]), pitch(chord + scale[(e * 2) % 5]), Shape::Square, 0.045f, 0.002f, 1.5f, 0.125f);
            // The lead rests now and then, holds a note for a beat sometimes, and moves by small steps.
            if (rnd.next() < 0.22f && e != 0) continue;
            degree = std::clamp(degree + rnd.below(5) - 2, 0, 9);
            const int midi = root + 12 * (degree / 5) + scale[degree % 5];
            const float held = (e % 2 == 0 && rnd.next() < 0.3f) ? beat * 0.95f : eighth * 0.85f;
            note(out, at, held, pitch(midi), pitch(midi), Shape::Square, 0.13f, 0.006f, 1.2f, 0.25f);
            if (bar == 7 && e >= 6) degree = 2;  // come home before the loop point
        }
        if (!drums) continue;
        for (int b = 0; b < 4; ++b) {
            const float at = (static_cast<float>(bar) * 4.0f + static_cast<float>(b)) * beat;
            if (b % 2 == 0) note(out, at, 0.16f, 140.0f, 45.0f, Shape::Sine, 0.5f, 0.001f, 2.0f);
            else note(out, at, 0.12f, 5000.0f, 2500.0f, Shape::Noise, 0.16f, 0.001f, 3.0f);
            note(out, at + eighth, 0.04f, 9000.0f, 9000.0f, Shape::Noise, 0.06f, 0.001f, 2.0f);
        }
    }
    out.resize(static_cast<std::size_t>(32.0f * beat * kRate));
    return out;
}

std::vector<std::int16_t> finish(const Wave& w, float gain = 1.0f) {
    float peak = 0.0001f;
    for (float v : w) peak = std::max(peak, std::abs(v));
    const float k = std::min(1.0f, 0.9f / peak) * gain;  // never clipped, never boosted
    std::vector<std::int16_t> out(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) out[i] = static_cast<std::int16_t>(std::clamp(w[i] * k, -1.0f, 1.0f) * 32000.0f);
    return out;
}

}  // namespace

FreeSounds makeFreeSounds() {
    FreeSounds s;
    auto add = [&](const std::string& name, std::initializer_list<int> ids, const Wave& w, float gain = 1.0f) {
        s.samples[name] = finish(w, gain);
        for (int id : ids) s.ids.emplace_back(id, name);
    };
    Wave w;
    // Menus.
    w.clear(), note(w, 0, 0.07f, 660, 660, Shape::Square, 0.3f), note(w, 0.07f, 0.12f, 990, 990, Shape::Square, 0.3f);
    add("select", {10}, w, 0.6f);
    w.clear(), note(w, 0, 0.04f, 880, 880, Shape::Square, 0.25f, 0.002f, 1.0f, 0.25f);
    add("move", {20}, w, 0.4f);
    w.clear(), note(w, 0, 0.25f, 130, 110, Shape::Saw, 0.4f, 0.004f, 0.6f);
    add("refuse", {40}, w, 0.6f);
    // Bombs.
    w.clear(), note(w, 0, 0.12f, 420, 140, Shape::Sine, 0.6f), note(w, 0, 0.03f, 3000, 2000, Shape::Noise, 0.15f);
    add("drop", {100}, w, 0.7f);
    w.clear(), note(w, 0, 0.10f, 300, 200, Shape::Sine, 0.5f), note(w, 0, 0.05f, 2000, 2000, Shape::Noise, 0.1f);
    add("poop", {550}, w, 0.6f);
    w.clear(), note(w, 0, 0.04f, 2500, 1500, Shape::Noise, 0.4f), note(w, 0.02f, 0.14f, 200, 520, Shape::Triangle, 0.5f);
    add("kick", {120}, w, 0.7f);
    w.clear(), note(w, 0, 0.09f, 500, 180, Shape::Triangle, 0.5f, 0.002f, 3.0f);
    add("stop", {130}, w, 0.6f);
    w.clear(), note(w, 0, 0.22f, 220, 660, Shape::Sine, 0.5f, 0.004f, 1.5f), note(w, 0.08f, 0.2f, 330, 990, Shape::Sine, 0.25f);
    add("jelly", {135}, w, 0.6f);
    for (int v = 0; v < 3; ++v) {
        w.clear(), note(w, 0, 0.2f, 110.0f - 10.0f * static_cast<float>(v), 50, Shape::Sine, 0.7f), note(w, 0, 0.08f, 1200, 500, Shape::Noise, 0.35f);
        add("wall" + std::to_string(v), {140 + v}, w, 0.8f);
    }
    w.clear(), note(w, 0, 0.18f, 4000, 700, Shape::Noise, 0.35f, 0.01f, 1.5f), note(w, 0, 0.1f, 260, 420, Shape::Triangle, 0.3f);
    add("punch", {150}, w, 0.7f);
    w.clear(), note(w, 0, 0.14f, 180, 480, Shape::Sine, 0.5f, 0.003f, 2.0f);
    add("bounce", {160}, w, 0.6f);
    w.clear(), note(w, 0, 0.06f, 520, 780, Shape::Square, 0.25f, 0.003f, 1.0f, 0.25f), note(w, 0.06f, 0.08f, 780, 1040, Shape::Square, 0.25f, 0.003f, 1.5f, 0.25f);
    add("grab", {170}, w, 0.5f);
    for (int v = 0; v < 3; ++v) {
        const float low = 70.0f - 8.0f * static_cast<float>(v);
        w.clear();
        note(w, 0, 0.9f, 900.0f - 150.0f * static_cast<float>(v), 90, Shape::Noise, 0.9f, 0.002f, 2.2f);
        note(w, 0, 0.5f, low * 2.2f, low * 0.6f, Shape::Sine, 0.9f, 0.002f, 1.6f);
        note(w, 0, 0.05f, 7000, 3000, Shape::Noise, 0.5f, 0.001f, 1.0f);
        add("boom" + std::to_string(v), {200 + v}, w);
    }
    // Players.
    w.clear(), note(w, 0, 0.55f, 700, 90, Shape::Square, 0.35f, 0.004f, 1.2f, 0.3f), note(w, 0, 0.3f, 1500, 300, Shape::Noise, 0.2f);
    add("die", {300}, w, 0.8f);
    w.clear(), note(w, 0, 0.35f, 150, 700, Shape::Sine, 0.5f, 0.004f, 1.2f), note(w, 0.1f, 0.3f, 300, 1400, Shape::Sine, 0.2f);
    add("spring", {350}, w, 0.7f);
    w.clear(), note(w, 0, 0.12f, 240, 120, Shape::Triangle, 0.7f, 0.001f, 2.0f), note(w, 0, 0.03f, 1800, 1200, Shape::Noise, 0.3f);
    add("bonk", {360}, w, 0.7f);
    for (int v = 0; v < 2; ++v) {
        w.clear();
        const int base = 72 + 5 * v;
        for (int n = 0; n < 4; ++n) note(w, 0.055f * static_cast<float>(n), 0.12f, pitch(base + (n == 3 ? 12 : 3 * n + n)), pitch(base + (n == 3 ? 12 : 3 * n + n)), Shape::Square, 0.25f, 0.003f, 1.5f, 0.25f);
        add("pickup" + std::to_string(v), {400 + v}, w, 0.55f);
    }
    w.clear();
    for (int n = 0; n < 8; ++n) note(w, 0.045f * static_cast<float>(n), 0.1f, pitch(72 + n * 2), pitch(72 + n * 2), Shape::Square, 0.22f, 0.003f, 1.5f, 0.25f);
    add("awesome", {1400}, w, 0.55f);
    w.clear();
    for (int n = 0; n < 6; ++n) note(w, 0.07f * static_cast<float>(n), 0.1f, 500.0f - 50.0f * static_cast<float>(n) + (n % 2 != 0 ? 60.0f : 0.0f), 300.0f - 30.0f * static_cast<float>(n), Shape::Saw, 0.25f);
    add("disease", {2300}, w, 0.6f);
    w.clear();
    for (int n = 0; n < 12; ++n) note(w, 0.04f * static_cast<float>(n), 0.06f, 300.0f + 110.0f * static_cast<float>((n * 5) % 7), 300.0f + 110.0f * static_cast<float>((n * 3) % 7), Shape::Sine, 0.3f);
    add("warp", {1330}, w, 0.6f);
    w.clear();
    for (int n = 0; n < 6; ++n) note(w, 0.16f * static_cast<float>(n), 0.11f, n % 2 == 0 ? 880.0f : 660.0f, n % 2 == 0 ? 880.0f : 660.0f, Shape::Square, 0.3f, 0.003f, 0.4f);
    add("hurry", {2700}, w, 0.6f);
    // The bonus wheel.
    w.clear(), note(w, 0, 0.03f, 1500, 1500, Shape::Square, 0.3f, 0.001f, 2.0f);
    add("tick", {1300}, w, 0.5f);
    w.clear();
    for (int n = 0; n < 5; ++n) note(w, 0.11f * static_cast<float>(n), n == 4 ? 0.5f : 0.12f, pitch(67 + (n == 4 ? 12 : n * 2 + (n > 1 ? 1 : 0))), pitch(67 + (n == 4 ? 12 : n * 2 + (n > 1 ? 1 : 0))), Shape::Square, 0.3f, 0.004f, 1.0f, 0.25f);
    add("fanfare", {1310}, w, 0.6f);
    w.clear(), note(w, 0, 0.6f, 95, 80, Shape::Saw, 0.5f, 0.004f, 0.5f);
    add("buzzer", {1320}, w, 0.6f);
    // Leaving the program: a short falling phrase.
    w.clear();
    for (int n = 0; n < 4; ++n) note(w, 0.22f * static_cast<float>(n), n == 3 ? 0.8f : 0.2f, pitch(76 - n * 3 - (n == 3 ? 3 : 0)), pitch(76 - n * 3 - (n == 3 ? 3 : 0)), Shape::Triangle, 0.5f, 0.006f, 1.0f);
    add("goodbye", {2600}, w, 0.7f);
    // Music: looping tunes, each from its own seed, tempo and key.
    add("tune_menu", {1000, 1010}, tune(11, 112, 60, false, true), 0.75f);
    add("tune_setup", {1020}, tune(23, 96, 57, true, false), 0.7f);
    add("tune_result", {1130}, tune(37, 104, 65, false, false), 0.7f);
    add("tune_a", {1100, 1103, 1106, 1109}, tune(41, 138, 62, false, true), 0.75f);
    add("tune_b", {1101, 1104, 1107, 1110}, tune(59, 126, 57, true, true), 0.75f);
    add("tune_c", {1102, 1105, 1108}, tune(73, 148, 64, true, true), 0.75f);
    return s;
}

}  // namespace ab
