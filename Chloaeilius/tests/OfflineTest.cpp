// Offline DSP tests for Chloaelis (no plugin wrapper).
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/DSP.h"
#if CHL_HEADLESS
 #include "../Source/PluginProcessor.h"   // (test 14 drives the whole plug-in)
#endif
#include <cstdio>

using namespace chl::dsp;
static int fails = 0;
constexpr int kNP = 16;   // the voice reads p[0..9]; the rest are modulation params
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "  ok " : "FAIL ", what); if (! ok) ++fails; }
static const char* typeName (int t) { return t == 0 ? "tape" : t == 1 ? "PT2399" : "BBD"; }

static double rms (const std::vector<float>& v, size_t a, size_t b)
{ double s = 0; for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i]; return std::sqrt (s / (double) std::max<size_t> (1, b - a)); }

// run a delay cell like the voice does (block() every 16 samples)
struct Rig
{
    DelayCell d;
    float age = 0;
    int type, n = 0;
    Rig (int t, float sr, float a = 0) : age (a), type (t)
    {
        d.prepare (sr, 1);
        d.activeType = t;
        d.tp.age = a;
        if (a > 0) d.tp.seedDamage();
    }
    float run (float x, float t, float regen, bool hpf = false, bool freeze = false, bool fade = false)
    {
        if (n++ % 16 == 0) d.block (type, age, 16.0f / 48000.0f);
        return d.process (x, t, regen, hpf, freeze, fade, type);
    }
};

// rough instantaneous frequency from zero crossings over a window
static double zcFreq (const std::vector<float>& v, size_t a, size_t b, float sr)
{
    int zc = 0; size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (v[i - 1] <= 0 && v[i] > 0) { if (zc == 0) first = i; last = i; ++zc; }
    return zc > 1 ? (zc - 1) * sr / (double) (last - first) : 0.0;
}

int main (int argc, char** argv)
{
    // (the processor remembers its last state in the user's settings: the tests start from the
    //  defaults and never touch it)
   #if JUCE_WINDOWS
    _putenv_s ("CHL_NO_LASTSTATE", "1");
   #else
    setenv ("CHL_NO_LASTSTATE", "1", 1);
   #endif
    const float sr = 48000.0f;
    juce::File outDir (argc > 1 ? argv[1] : juce::File::getCurrentWorkingDirectory().getFullPathName());

    // 1. every engine puts its echo where the time says
    for (int type : { 0, 1, 2 })
        for (float t : { 0.05f, 0.2f, 0.342f, 0.8f })
        {
            Rig r (type, sr);
            r.d.tp.setTime (t, true); r.d.pt[0].setTime (t, true); r.d.bb[0].setTime (t, true); r.d.lineTime[0] = t;
            const int N = (int) (sr * (t + 0.3f));
            int peakAt = 0; float peak = 0;
            for (int i = 0; i < N; ++i)
            {
                const float x = (i >= 100 && i < 148) ? 0.8f : 0.0f;   // 1 ms pulse
                const float y = r.run (x, t, 0.0f);
                if (i > 300 && std::abs (y) > peak) { peak = std::abs (y); peakAt = i; }
            }
            const float measured = (float) (peakAt - 124) / sr;
            char msg[160]; std::snprintf (msg, sizeof msg, "%-6s time %.0f ms -> echo at %.1f ms (peak %.2f)", typeName (type), t * 1000, measured * 1000, peak);
            check (std::abs (measured - t) < 0.008f + t * 0.03f && peak > 0.08f, msg);
        }

    // 2. freeze holds the buffer (the tape one slowly crumbles, but only slowly when new)
    for (int type : { 0, 1, 2 })
    {
        Rig r (type, sr);
        juce::Random rn (7);
        std::vector<float> out;
        for (int i = 0; i < (int) (sr * 5); ++i)
        {
            const bool fr = i > (int) (sr * 1.0f);
            const float x = i < (int) (sr * 0.9f) ? std::sin (i * 0.05f) * 0.5f * (rn.nextFloat() * 0.3f + 0.7f) : 0.0f;
            out.push_back (r.run (x, 0.3f, 0.0f, false, fr));
        }
        const double a = rms (out, (size_t) (sr * 1.5), (size_t) (sr * 2.5)), b = rms (out, (size_t) (sr * 4.0), (size_t) (sr * 5.0));
        char msg[160]; std::snprintf (msg, sizeof msg, "%-6s freeze sustains (rms %.3f -> %.3f)", typeName (type), a, b);
        check (a > 0.03 && b > a * 0.7 && b < a * 1.3, msg);
    }

    // 3. high regen self-oscillates but stays bounded; hpf path too
    for (int type : { 0, 1, 2 })
        for (bool hpf : { false, true })
        {
            Rig r (type, sr);
            juce::Random rn (3);
            float peak = 0; bool finite = true;
            for (int i = 0; i < (int) (sr * 8); ++i)
            {
                const float x = i < 2000 ? rn.nextFloat() - 0.5f : 0.0f;
                const float tm = 0.1f + 0.05f * std::sin (i / sr);
                const float y = r.run (x, tm, 1.15f, hpf);
                finite &= std::isfinite (y); peak = std::max (peak, std::abs (y));
            }
            char msg[160]; std::snprintf (msg, sizeof msg, "%-6s regen 115%% (hpf=%d) bounded: peak %.2f", typeName (type), (int) hpf, peak);
            check (finite && peak < 3.0f, msg);
        }

    // residual (noise + distortion) after removing a 440 Hz sine by least squares, in dB
    auto residual = [&] (const std::vector<float>& o, size_t a, size_t b)
    {
        double sc = 0, cs = 0, ss = 0, ccc = 0;
        for (size_t i = a; i < b; ++i)
        { const double s = std::sin (i * 2 * M_PI * 440 / sr), c = std::cos (i * 2 * M_PI * 440 / sr); sc += o[i] * s; cs += o[i] * c; ss += s * s; ccc += c * c; }
        double e = 0, tot = 0;
        for (size_t i = a; i < b; ++i)
        { const double s = std::sin (i * 2 * M_PI * 440 / sr), c = std::cos (i * 2 * M_PI * 440 / sr); const double res = o[i] - sc / ss * s - cs / ccc * c; e += res * res; tot += (double) o[i] * o[i]; }
        return 10.0 * std::log10 (e / tot);
    };

    // 4. clocked chips: longer delay = darker + noisier / crunchier
    for (int type : { 1, 2 })
    {
        auto grit = [&] (float t)
        {
            Rig r (type, sr);
            r.d.pt[0].setTime (t, true); r.d.bb[0].setTime (t, true); r.d.lineTime[0] = t;
            std::vector<float> o;
            for (int i = 0; i < (int) (sr * 2.5f); ++i) o.push_back (r.run (std::sin (i * 2 * pi * 440.0f / sr) * 0.4f, t, 0.0f));
            return residual (o, (size_t) (sr * 1.5), (size_t) (sr * 2.5));
        };
        const double s1 = grit (0.05f), s2 = grit (0.34f), s3 = grit (0.95f);
        char msg[200]; std::snprintf (msg, sizeof msg, "%-6s artefacts grow with time: %.1f dB @50ms, %.1f dB @340ms, %.1f dB @950ms", typeName (type), s1, s2, s3);
        check (s1 < s3 && s2 < s3 && s1 < -20, msg);
    }

    // 5. tape: an old tape is duller, hissier and more broken than a new one
    {
        auto tapeRun = [&] (float age, double& hf, double& noise)
        {
            Rig r (0, sr, age);
            juce::Random rn (5);
            std::vector<float> o, nz;
            for (int i = 0; i < (int) (sr * 3.0f); ++i) o.push_back (r.run ((rn.nextFloat() - 0.5f) * 0.5f, 0.2f, 0.0f));
            // high band energy relative to total (white noise in)
            Biquad hp; hp.highpass (6000.0f, sr, 0.7f);
            double h = 0, tot = 0;
            for (size_t i = (size_t) sr; i < o.size(); ++i) { const float y = hp.process (o[i]); h += y * y; tot += (double) o[i] * o[i]; }
            hf = 10.0 * std::log10 (h / tot);
            Rig q (0, sr, age);
            for (int i = 0; i < (int) (sr * 2.0f); ++i) nz.push_back (q.run (0.0f, 0.2f, 0.0f));
            noise = 20.0 * std::log10 (rms (nz, (size_t) sr, nz.size()) + 1e-9);
        };
        double hf0, n0, hf1, n1;
        tapeRun (0.0f, hf0, n0);
        tapeRun (1.0f, hf1, n1);
        char msg[200]; std::snprintf (msg, sizeof msg, "tape wear: HF share %.1f -> %.1f dB, hiss %.1f -> %.1f dBFS", hf0, hf1, n0, n1);
        check (hf1 < hf0 - 6 && n1 > n0 + 8 && n0 < -55, msg);
    }

    // 6. slew vs fade: a time change bends the pitch in slew mode, never in fade mode
    for (int type : { 0, 1, 2 })
        for (bool fade : { false, true })
        {
            Rig r (type, sr);
            std::vector<float> o;
            const int N = (int) (sr * 4.0f);
            double worst = 0;
            for (int i = 0; i < N; ++i)
            {
                const float t = i < (int) (sr * 1.5f) ? 0.2f : 0.35f;
                o.push_back (r.run (std::sin (i * 2 * pi * 440.0f / sr) * 0.3f, t, 0.0f, false, false, fade));
            }
            // pitch in 50 ms windows after the change
            for (size_t a = (size_t) (sr * 1.5f); a + 2400 < (size_t) N; a += 2400)
                worst = std::max (worst, std::abs (zcFreq (o, a, a + 2400, sr) - 440.0));
            char msg[160]; std::snprintf (msg, sizeof msg, "%-6s %s: max pitch deviation after a time change %.1f Hz", typeName (type), fade ? "fade" : "slew", worst);
            check (fade ? worst < 12.0 : worst > 20.0, msg);
        }

    // 7. tube: unity-ish when clean, asymmetric (even harmonics) when driven
    for (float drive : { 0.0f, 0.5f, 1.0f })
    {
        Tube tb; tb.prepare (sr, 16);
        std::vector<float> o;
        float buf[16];
        int ph = 0;
        for (int blk = 0; blk < 9000; ++blk)
        {
            for (int i = 0; i < 16; ++i, ++ph) buf[i] = 0.4f * std::sin (ph * 2 * pi * 200.0f / sr);
            tb.process (buf, 16, drive, drive);
            o.insert (o.end(), buf, buf + 16);
        }
        auto mag = [&] (int h)
        {
            double re = 0, im = 0; const size_t a = 48000, b = 144000;
            for (size_t i = a; i < b; ++i) { re += o[i] * std::cos (i * 2 * M_PI * 200 * h / sr); im += o[i] * std::sin (i * 2 * M_PI * 200 * h / sr); }
            return std::sqrt (re * re + im * im) * 2.0 / (double) (b - a);
        };
        const double h1 = mag (1), h2 = mag (2), h3 = mag (3);
        bool finite = true; for (auto v : o) finite &= std::isfinite (v);
        char msg[200]; std::snprintf (msg, sizeof msg, "tube drive %.1f: fund %.3f, H2 %.1f dB, H3 %.1f dB", drive, h1, 20 * std::log10 (h2 / h1 + 1e-12), 20 * std::log10 (h3 / h1 + 1e-12));
        check (finite && h1 > 0.05 && h1 < 1.5 && (drive == 0.0f ? h2 / h1 < 0.1 : h2 / h1 > 0.01), msg);
    }

    // 8. Lorenz: bounded, keeps moving, and the two outputs differ
    {
        Lorenz lz;
        float mn = 9, mx = -9; double diff = 0; bool finite = true;
        for (int i = 0; i < 20000; ++i)
        {
            lz.step (0.01, 28.0, 8.0 / 3.0);
            const float a = lz.outX (28.0, 8.0 / 3.0), b = lz.outY (28.0, 8.0 / 3.0);
            finite &= std::isfinite (a) && std::isfinite (b);
            mn = std::min (mn, a); mx = std::max (mx, a); diff += std::abs (a - b);
        }
        char msg[160]; std::snprintf (msg, sizeof msg, "lorenz: x spans %.2f..%.2f, mean |x-y| %.2f", mn, mx, diff / 20000);
        check (finite && mn < -0.5f && mx > 0.5f && diff / 20000 > 0.05, msg);
    }

    // 9. whole voice, every engine and both orders, with the poly copy on; write demos
    for (int type : { 0, 1, 2 })
        for (int order : { 0, 1 })
        {
            ChloeVoice v; v.prepare (sr, 16); v.reset();
            float p[kNP] = { 0.0f, 20000.0f, 0.15f, 0.0f, 0.3f, 0.33f, 0.5f, 0.4f, 0.0f };
            ChloeVoice::Flags f; f.type = type; f.order = order; f.width0 = f.width1 = 0.6f;
            v.chains[v.active].start (order, type, false, 0.33f); v.chains[v.active].inGain = 1.0f;   // begin already in this setup
            juce::AudioBuffer<float> demo (1, (int) sr * 6);
            float buf[16]; int ph = 0; bool finite = true;
            for (int blk = 0; blk < demo.getNumSamples() / 16; ++blk)
            {
                for (int i = 0; i < 16; ++i, ++ph)
                {
                    const float env = std::exp (-(float) (ph % 24000) / 3000.0f);
                    buf[i] = env * 0.5f * (2.0f * std::fmod (ph * 110.0f / sr, 1.0f) - 1.0f);
                }
                v.process (buf, 16, p, p, f);
                for (int i = 0; i < 16; ++i) { finite &= std::isfinite (buf[i]); demo.setSample (0, blk * 16 + i, buf[i]); }
            }
            char msg[120]; std::snprintf (msg, sizeof msg, "voice renders (%s, order %d, poly on)", typeName (type), order);
            check (finite && demo.getMagnitude (0, demo.getNumSamples()) > 0.05f, msg);
            if (order == 0)
            {
                auto fl = outDir.getChildFile (juce::String ("chloaelis_demo_") + typeName (type) + ".wav");
                fl.deleteFile();
                if (auto os = std::unique_ptr<juce::OutputStream> (fl.createOutputStream()))
                {
                    juce::WavAudioFormat wav;
                    if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
                        w->writeFromAudioSampleBuffer (demo, 0, demo.getNumSamples());
                }
            }
        }

    // 10. slam every parameter between its extremes with a square LFO, for each engine,
    //     fade mode on, poly on: nothing may explode
    {
        const char* nm[] = { "in att", "cutoff", "res", "morph", "drive", "time", "regen", "wet", "out" };
        const float lo[] = { -48, 20, 0, 0, 0, 0.03f, 0, 0, -48 }, hi[] = { 12, 20000, 1, 3, 1, 1, 1.15f, 1, 12 };
        const float def[] = { 0, 20000, 0.15f, 0, 0.3f, 0.33f, 0.4f, 0.35f, 0 };
        for (int type : { 0, 1, 2 })
        {
            float worstAll = 0; int worstP = 0;
            for (int m = 0; m < 9; ++m)
                for (float base : { 0.0f, 1.0f })     // also with resonance maxed
                {
                    ChloeVoice v; v.prepare (sr, 16); v.reset(); juce::Random r (1);
                    ChloeVoice::Flags f; f.type = type; f.fade = base > 0; f.width0 = f.width1 = 1.0f; f.order = m % 2;
                    float p0[kNP] {}, p1[kNP] {}; std::copy (def, def + 9, p0);
                    if (base > 0) { p0[vRes] = 1.0f; p0[vCut] = 800.0f; p0[vDrive] = 1.0f; }
                    float buf[16], ph = 0;
                    for (int b = 0; b < (int) (sr * 8 / 16); ++b)
                    {
                        std::copy (p0, p0 + kNP, p1);
                        ph += 16 * 3.0f / sr;
                        p1[m] = std::fmod (ph, 1.0f) < 0.5f ? lo[m] : hi[m];
                        f.freeze = std::fmod (ph * 0.37f, 1.0f) < 0.2f;
                        for (int i = 0; i < 16; ++i) buf[i] = r.nextFloat() * 0.8f - 0.4f;
                        v.process (buf, 16, p0, p1, f);
                        std::copy (p1, p1 + kNP, p0);
                        if (base > 0) { p0[vRes] = p1[vRes] = m == vRes ? p1[vRes] : 1.0f; }
                        for (int i = 0; i < 16; ++i)
                        {
                            const float a = std::isfinite (buf[i]) ? std::abs (buf[i]) : 1e9f;
                            if (a > worstAll) { worstAll = a; worstP = m; }
                        }
                    }
                }
            char msg[160]; std::snprintf (msg, sizeof msg, "%-6s square-mod torture: peak %.2f (worst on %s)", typeName (type), worstAll, nm[worstP]);
            check (worstAll < 6.0f, msg);
        }
    }

    // 11. Karplus-Strong time: the string sounds at the pitch asked for (whatever the engine's damping), and rings
    for (int type : { 0, 1, 2 })
        for (float hz : { 55.0f, 220.0f, 587.33f, 1760.0f })
        {
            ChloeVoice v; v.prepare (sr, 16); v.reset();
            float p[kNP] = { 0.0f, 20000.0f, 0.15f, 0.0f, 0.0f, 0.33f, 0.97f, 1.0f, 0.0f };
            ChloeVoice::Flags f; f.type = type; f.tubeOn = false; f.ks = true; f.ksPeriod = 1.0f / hz;
            v.chains[v.active].start (0, type, false, f.ksPeriod, true); v.chains[v.active].inGain = 1.0f;   // begin already as a string
            std::vector<float> o; float buf[16]; juce::Random r (5); bool finite = true;
            for (int blk = 0; blk < (int) sr * 2 / 16; ++blk)
            {
                for (int i = 0; i < 16; ++i) buf[i] = blk < 12 ? r.nextFloat() - 0.5f : 0.0f;   // a pluck
                v.process (buf, 16, p, p, f);
                for (int i = 0; i < 16; ++i) finite &= std::isfinite (buf[i]);
                o.insert (o.end(), buf, buf + 16);
            }
            // the fundamental's frequency: the peak of a windowed transform scanned round the note
            const size_t a = (size_t) (sr * 0.05), n = (size_t) (sr * 0.5);
            auto mag = [&] (double cents)
            {
                const double w0 = 2.0 * M_PI * hz * std::exp2 (cents / 1200.0) / sr;
                double re = 0, im = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double wnd = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) n);
                    re += o[a + i] * wnd * std::cos (w0 * (double) i); im += o[a + i] * wnd * std::sin (w0 * (double) i);
                }
                return std::sqrt (re * re + im * im);
            };
            double bc = -15.0, bv = -1.0;
            for (double c = -15.0; c <= 15.0; c += 0.5) { const double m = mag (c); if (m > bv) { bv = m; bc = c; } }
            const double y0 = mag (bc - 0.5), y2 = mag (bc + 0.5);
            const double cents = bc + 0.25 * (y0 - y2) / (y0 - 2.0 * bv + y2);
            const double ring = rms (o, (size_t) (sr * 0.2), (size_t) (sr * 0.3));
            char msg[160]; std::snprintf (msg, sizeof msg, "karplus %s %.0f Hz: %+.2f cents, ringing %.4f", typeName (type), hz, cents, ring);
            check (finite && std::abs (cents) < 2.0 && ring > 0.002, msg);
        }

    // 12. tape hiss fades in when the tape is switched to, and out when it is left (no step either way)
    {
        ChloeVoice v; v.prepare (sr, 16); v.reset();
        float p[kNP] = { 0.0f, 20000.0f, 0.15f, 0.0f, 0.0f, 0.33f, 0.4f, 1.0f, 0.0f };
        ChloeVoice::Flags f; f.type = 1; f.tubeOn = false;
        std::vector<float> o; float buf[16];
        auto run = [&] (double sec) { for (int blk = 0; blk < (int) (sr * sec) / 16; ++blk) { for (auto& b : buf) b = 0.0f; v.process (buf, 16, p, p, f); o.insert (o.end(), buf, buf + 16); } };
        run (0.5);
        f.type = 0; const size_t t0 = o.size(); run (4.0);
        f.type = 1; const size_t t1 = o.size(); run (4.0);
        const size_t w = (size_t) (sr * 0.05);
        const double floorPt = rms (o, t0 - w * 4, t0);
        const double in0 = rms (o, t0, t0 + w), full = rms (o, t0 + (size_t) (sr * 3.0), t1);
        const double out0 = rms (o, t1, t1 + w), gone = rms (o, t1 + (size_t) (sr * 3.0), o.size());
        // the biggest jump between neighbouring 20 ms windows, against the full hiss level
        double jump = 0; const size_t h = (size_t) (sr * 0.02);
        for (size_t i = t0 - h; i + 2 * h < o.size(); i += h) jump = std::max (jump, std::abs (rms (o, i + h, i + 2 * h) - rms (o, i, i + h)));
        char msg[200]; std::snprintf (msg, sizeof msg, "tape hiss: PT floor %.6f, first 50 ms %.6f, full %.6f, first 50 ms after leaving %.6f, 3 s later %.6f, biggest 20 ms jump %.6f",
                                      floorPt, in0, full, out0, gone, jump);
        check (full > 4.0 * floorPt && in0 < 0.2 * full && out0 > 0.5 * full && gone < 0.3 * full && jump < 0.25 * full, msg);
    }

    // 13. Karplus-Strong with a steady signal going in: a string must not blow up. The loop's regeneration
    //     is near 1, so a sustained input would be amplified by 1/(1-g) (tens of dB); the string is held
    //     to its ceiling instead, cleanly (the output stays near the sum of the input and a ceiling string)
    for (int type : { 0, 1, 2 })
        for (float regen : { 0.4f, 0.8f, 1.0f, 1.15f })
        {
            ChloeVoice v; v.prepare (sr, 16); v.reset();
            float p[kNP] = { 0.0f, 20000.0f, 0.15f, 0.0f, 0.0f, 0.33f, regen, 1.0f, 0.0f };
            ChloeVoice::Flags f; f.type = type; f.tubeOn = false; f.ks = true; f.ksPeriod = 1.0f / 220.0f;
            v.chains[v.active].start (0, type, false, f.ksPeriod, true); v.chains[v.active].inGain = 1.0f;
            float buf[16]; juce::Random r (7); bool finite = true; float peak = 0; std::vector<float> o;
            for (int blk = 0; blk < (int) sr * 3 / 16; ++blk)
            {
                // a steady, full-band signal at 0.3 peak: noise plus a tone sitting on the string's 3rd harmonic
                for (int i = 0; i < 16; ++i) buf[i] = 0.15f * (r.nextFloat() * 2.0f - 1.0f) + 0.15f * std::sin (2.0f * pi * 660.0f * (float) (blk * 16 + i) / sr);
                v.process (buf, 16, p, p, f);
                for (int i = 0; i < 16; ++i) { finite &= std::isfinite (buf[i]); if (blk * 16 > (int) sr) peak = std::max (peak, std::abs (buf[i])); }
                o.insert (o.end(), buf, buf + 16);
            }
            // level over the last two seconds, and how much it wobbles between 100 ms windows (the governor must not pump)
            const size_t w = (size_t) (sr * 0.1); double lo = 1e9, hi = 0;
            for (size_t i = (size_t) sr; i + w <= o.size(); i += w) { const double m = rms (o, i, i + w); lo = std::min (lo, m); hi = std::max (hi, m); }
            char msg[160]; std::snprintf (msg, sizeof msg, "karplus %s steady input, regen %.2f: peak %.2f, rms %.3f, 100 ms windows %.3f..%.3f", typeName (type), regen, peak, rms (o, (size_t) sr, o.size()), lo, hi);
            check (finite && peak < 0.9f && hi < 1.3 * lo, msg);
        }


#if CHL_HEADLESS
    // 14. The whole plug-in, driven as a host and a player do: hot programme material through the
    // processor while the Karplus-Strong switch is flipped with the sound running, the note swept
    // across its whole range (fast, and parked at both ends), regen taken to the top and back, poly
    // and the flock up, freeze, slew and fade, every engine, both orders, the tube up, the filter
    // resonant and swept, the master feedback up, a long steady sine, at three sample rates and
    // block sizes from 32 to 2048. Nothing may ever go non-finite, the output stays under the
    // ceiling, and the sound is back after every change.
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        constexpr float ceiling = 1.0f;
        struct Host
        {
            ChloaelisProcessor p;
            double sr; int block, nOut;
            juce::AudioBuffer<float> buf; juce::MidiBuffer midi; juce::Random rng { 11 };
            long t = 0; bool finite = true; float peak = 0, rawPeak = 0; int kind = 0;
            juce::String firstBad, peakAt;
            Host (double s, int b) : sr (s), block (b)
            {
                nOut = juce::jmax (2, p.getTotalNumOutputChannels());
                p.setPlayConfigDetails (2, nOut, sr, block);
                p.prepareToPlay (sr, block);
                buf.setSize (nOut, block);
            }
            void set (const juce::String& id, float real) { auto* prm = p.apvts.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (real)); }
            void setN (const juce::String& id, float norm) { p.apvts.getParameter (id)->setValueNotifyingHost (norm); }
            void both (int prm, float real) { for (int s = 0; s < 2; ++s) set (chl::pid (s, prm), real); }
            void bothN (int prm, float norm) { for (int s = 0; s < 2; ++s) setN (chl::pid (s, prm), norm); }
            void bothId (juce::String (*id) (int), float real) { for (int s = 0; s < 2; ++s) set (id (s), real); }
            // runs seconds of audio; returns the output's rms over the last quarter of it
            double run (double seconds, const char* label)
            {
                const int blocks = juce::jmax (1, (int) (seconds * sr / block)), tailFrom = blocks * 3 / 4;
                double e = 0; long cnt = 0;
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < block; ++i, ++t)
                    {
                        const float ph = (float) t / (float) sr;
                        const float v = kind == 0 ? 0.55f * (rng.nextFloat() * 2.0f - 1.0f) + 0.2f * std::sin (2.0f * pi * 110.0f * ph) + 0.15f * std::sin (2.0f * pi * 1234.0f * ph)
                                      : kind == 1 ? 0.8f * std::sin (2.0f * pi * 220.0f * ph) : 0.0f;
                        buf.setSample (0, i, v); buf.setSample (1, i, v * 0.9f);
                    }
                    for (int c = 2; c < nOut; ++c) buf.clear (c, 0, block);
                    p.processBlock (buf, midi);
                    for (int s = 0; s < 2; ++s)
                        rawPeak = juce::jmax (rawPeak, p.voiceForTest (s).rawPeak);
                    for (int s = 0; s < 2; ++s)
                        if (p.voiceForTest (s).nanEvents > 0 && finite) { firstBad = juce::String (label) + ", side " + juce::String (s) + " went non-finite inside (block " + juce::String (b) + " of " + juce::String (blocks) + ")"; finite = false; }
                    for (int c = 0; c < nOut; ++c)
                    {
                        const float* d = buf.getReadPointer (c);
                        for (int i = 0; i < block; ++i)
                        {
                            if (! std::isfinite (d[i]))
                            {
                                if (finite) firstBad = juce::String (label) + ", ch " + juce::String (c) + ", block " + juce::String (b) + " of " + juce::String (blocks);
                                finite = false;
                                continue;
                            }
                            const float a = std::abs (d[i]);
                            if (a > peak) { peak = a; if (a > ceiling && peakAt.isEmpty()) peakAt = juce::String (label) + ", block " + juce::String (b) + " of " + juce::String (blocks); }
                            if (b >= tailFrom && c < 2) { e += (double) d[i] * d[i]; ++cnt; }
                        }
                    }
                }
                return std::sqrt (e / (double) juce::jmax (1L, cnt));
            }
        };
        const std::pair<double, int> setups[] = { { 48000.0, 32 }, { 44100.0, 64 }, { 48000.0, 512 }, { 96000.0, 2048 } };
        for (auto [srate, blk] : setups)
        {
            Host h (srate, blk);
            juce::String quietAt;
            auto step = [&] (const char* label, double secs)
            {
                const double r = h.run (secs, label);
                if (r < 1.0e-3 && quietAt.isEmpty()) quietAt = juce::String (label) + " (rms " + juce::String (r, 5) + ")";
            };
            step ("defaults", 1.0);
            h.bothId (chl::ksId, 1.0f);                       step ("karplus on with the sound running", 2.0);
            h.both (chl::wet, 1.0f); h.both (chl::ltDepth, 1.0f); h.both (chl::lmDepth, 1.0f); h.set (chl::widthId, 1.0f); h.set (chl::flockId, 1.0f); h.bothId (chl::hpfId, 1.0f); h.bothId (chl::delayTypeId, 2.0f); h.set (chl::lzDepthId, 0.5f);
            step ("all wet, the time and mode lfos across their whole range, poly, flock, hpf, bbd, lorenz", 4.0);
            h.both (chl::ltDepth, 0.0f); h.both (chl::lmDepth, 0.0f); h.both (chl::wet, 0.35f); h.set (chl::widthId, 0.0f); h.set (chl::flockId, 0.0f); h.bothId (chl::hpfId, 0.0f); h.bothId (chl::delayTypeId, 1.0f); h.set (chl::lzDepthId, 0.0f);
            step ("back to the defaults", 1.0);
            for (int k = 0; k <= 20; ++k) { h.bothN (chl::time, (float) k / 20.0f); step ("note sweep up", 0.08); }
            for (int k = 20; k >= 0; --k) { h.bothN (chl::time, (float) k / 20.0f); step ("note sweep down, fast", 0.03); }
            h.bothN (chl::time, 0.0f);                        step ("highest note", 0.6);
            h.bothN (chl::time, 1.0f);                        step ("lowest note", 0.6);
            h.bothN (chl::time, 0.5f);                        step ("middle note", 0.4);
            for (float r : { 0.0f, 0.3f, 0.6f, 0.9f, 1.0f, 1.15f }) { h.both (chl::regen, r); step ("regen up", 0.3); }
            step ("regen 1.15 held", 1.5);
            for (float r : { 0.9f, 0.4f, 0.0f, 0.4f }) { h.both (chl::regen, r); step ("regen back", 0.3); }
            h.set (chl::widthId, 1.0f); h.set (chl::flockId, 1.0f); step ("poly 8 taps + flock", 2.0);
            h.bothId (chl::freezeId, 1.0f);                   step ("freeze", 0.5);
            h.bothId (chl::freezeId, 0.0f);                   step ("unfreeze", 1.0);
            h.bothId (chl::timeModeId, 1.0f);                 step ("fade mode", 0.3);
            for (int k = 0; k <= 10; ++k) { h.bothN (chl::time, (float) k / 10.0f); step ("note sweep in fade mode", 0.1); }
            // (the bug of 1.2.0: a string held while a fade was crossfading between two reads of the same
            //  line summed them back into itself: a loop gain above one, off to infinity, then silence)
            h.bothId (chl::freezeId, 1.0f);
            for (int k = 10; k >= 0; --k) { h.bothN (chl::time, (float) k / 10.0f); step ("frozen while the note moves in fade mode", 0.12); }
            step ("frozen in fade mode, held", 2.0);
            h.bothId (chl::freezeId, 0.0f); h.both (chl::rnd, 1.0f); step ("random freeze with the note moving in fade mode", 2.0);
            h.both (chl::rnd, 0.0f);
            h.bothId (chl::timeModeId, 0.0f); h.bothN (chl::time, 0.4f); step ("slew mode", 0.5);
            for (int ty = 0; ty < 3; ++ty) { h.bothId (chl::delayTypeId, (float) ty); step ("engine change", 1.0); }
            h.set (chl::orderId, 1.0f);                       step ("delay > filter", 1.0);
            h.set (chl::orderId, 0.0f);                       step ("filter > delay", 0.6);
            h.bothId (chl::tubeOnId, 1.0f); h.both (chl::drive, 1.0f); step ("tube full", 1.0);
            h.both (chl::drive, 0.3f);
            h.both (chl::res, 1.0f);
            for (int k = 0; k <= 10; ++k) { h.both (chl::cutoff, 20.0f * std::pow (1000.0f, (float) k / 10.0f)); step ("resonant cutoff sweep", 0.1); }
            h.both (chl::res, 0.15f); h.both (chl::cutoff, 20000.0f);
            for (int k = 0; k < 6; ++k) { h.bothId (chl::ksId, k % 2 == 0 ? 0.0f : 1.0f); step ("karplus toggled", 0.15); }
            h.bothId (chl::ksId, 1.0f);
            h.set (chl::masterFbId, 0.9f);                    step ("master feedback 90%", 1.5);
            h.set (chl::masterFbId, 0.0f);
            h.kind = 1; h.both (chl::regen, 1.15f);           step ("long sine, regen 1.15, poly up", 4.0);
            h.kind = 0; h.both (chl::regen, 0.4f);
            h.bothId (chl::ksId, 0.0f);                       step ("karplus off again", 1.0);
            char msg[400];
            std::snprintf (msg, sizeof msg, "processor stress at %.0f Hz, blocks of %d: finite %s, peak %.2f (before the last line %.2f)%s%s%s", srate, blk, h.finite ? "yes" : "NO", h.peak, h.rawPeak,
                           h.firstBad.isNotEmpty() ? (" (first bad: " + h.firstBad + ")").toRawUTF8() : "",
                           h.peakAt.isNotEmpty() ? (" (over the ceiling: " + h.peakAt + ")").toRawUTF8() : "",
                           quietAt.isNotEmpty() ? (" (went quiet: " + quietAt + ")").toRawUTF8() : "");
            check (h.finite && h.peak <= ceiling && quietAt.isEmpty(), msg);
        }
        // and a host turning every knob at random (the same seed every time), hot input, with the
        // Karplus switch flipped often: whatever it lands on, nothing goes non-finite or past the ceiling
        for (auto [srate, blk, secs] : { std::tuple<double, int, double> { 48000.0, 256, 60.0 }, { 44100.0, 64, 30.0 } })
        {
            Host h (srate, blk);
            juce::Random r (1);
            juce::Array<juce::RangedAudioParameter*> prms;
            for (auto* prm : h.p.getParameters()) if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm)) prms.add (rp);
            double done = 0; int moves = 0;
            while (done < secs)
            {
                auto* rp = prms[r.nextInt (prms.size())];
                float v = r.nextFloat();
                if (r.nextFloat() < 0.3f) v = r.nextBool() ? 1.0f : 0.0f;
                if (r.nextFloat() < 0.15f) rp = h.p.apvts.getParameter (chl::ksId (r.nextInt (2)));
                rp->setValueNotifyingHost (v);
                ++moves;
                const double d = 0.01 + 0.08 * r.nextDouble();
                h.kind = r.nextFloat() < 0.1f ? 1 : 0;
                h.run (d, (rp->getParameterID() + " = " + juce::String (v, 3)).toRawUTF8());
                done += d;
            }
            char msg[400];
            std::snprintf (msg, sizeof msg, "random automation at %.0f Hz, blocks of %d: %.0f s, %d moves, finite %s, peak %.2f (before the last line %.2f)%s%s", srate, blk, done, moves, h.finite ? "yes" : "NO", h.peak, h.rawPeak,
                           h.firstBad.isNotEmpty() ? (" (first bad: " + h.firstBad + ")").toRawUTF8() : "",
                           h.peakAt.isNotEmpty() ? (" (over the ceiling: " + h.peakAt + ")").toRawUTF8() : "");
            check (h.finite && h.peak <= ceiling, msg);
        }
    }
#endif

    std::printf (fails ? "\nOFFLINE TESTS FAILED (%d)\n" : "\nOFFLINE TESTS OK\n", fails);
    return fails;
}
