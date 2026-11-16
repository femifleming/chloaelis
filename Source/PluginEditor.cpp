#include "PluginEditor.h"
#if JUCE_MODULE_AVAILABLE_juce_audio_plugin_client
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include <complex>
#include <BinaryData.h>

using namespace juce;

// profiling counters (read by the host test's benchmark)
int chloaelisDbgRenders = 0;
int chloaelisDbgFrames = 0;

// Look: faltung mustard. One flat colour field (the sleeve's mustard), and on it every control
// as a floating cell: a folded paper strip (a skewed parallelogram with a lighter fold across
// the top and a white hairline on the fold) carrying its Greek letter, with its level as a stack
// of chips lit up to the value, or a skewed fill (a vial), or a band riding a rule (the cutoff's
// collar). Buttons and switches are outlined chips, filled when chosen. A dashed wireframe links
// the cells along the signal path; the Lorenz attractor runs in the middle as a thin line in
// front of a wireframe polyhedron; the flock flies round the poly knob. The cells breathe: a slow
// drift, each in its own phase. Names and symbols are set in Inter, values in IBM Plex Mono, all
// lowercase. Two modes, light (mustard) and dark (mustard-black), nothing else: no bokeh, glow,
// blur, grain, vignette or bevel.
// The palette still follows the audio: the field warms with the input level, each side's cells
// warm with its output (toward the highlight, then red at the top), the folds brighten, and the
// links and the attractor brighten, with a meter's ballistics. In silence it is the plain look.
namespace
{
    constexpr float W = 1200.0f, H = 820.0f;
    // How close together the cells are. The layout below is written for a 1200 x 820 field; the
    // cells are drawn this much larger on it and the window opens this much smaller, so each cell is
    // the size it always was and only the space between them has gone.
    constexpr float closeness = 1.12f;
    float density = closeness;
    constexpr float SK = -0.42f;         // the fold's skew (about 24 degrees)

    struct Field { Colour bg, ink, hi, well, cell[3], face[3]; };
    const Field lightField { Colour (0xffc9b53b), Colour (0xff23200a), Colour (0xffffffff), Colour (0xffd8c64e),
                             { Colour (0xff5e8f6a), Colour (0xff8ec3e6), Colour (0xffee8e6e) }, { Colour (0xff8dbb7a), Colour (0xffb6dcf2), Colour (0xfff6c590) } };
    const Field darkField  { Colour (0xff23200a), Colour (0xffe8dfb0), Colour (0xfffff2b0), Colour (0xff2f2a0c),
                             { Colour (0xff4a7555), Colour (0xff6fa3c6), Colour (0xffd4765a) }, { Colour (0xff5e8f6a), Colour (0xff8ec3e6), Colour (0xffee8e6e) } };
    bool dark = false;
    Field F = lightField;        // the field as drawn this frame: the base, warmed by the level
    float lineAlpha = 0.45f, actMax = 0.0f, actSide[3] {};

    // level -> dB (as the meters read it)
    float levelDb (float meanAbs) { return Decibels::gainToDecibels (meanAbs * 1.57f, -90.0f); }

    // The palette follows the audio. warm: each side's input level (0..1, with ballistics);
    // outDb: each side's output level. The middle cells take the louder side.
    void updateField (const float warm[2], const float outDb[2])
    {
        const Field& B = dark ? darkField : lightField;
        float act[3], hot[3], red[3];
        for (int s = 0; s < 2; ++s)
        {
            act[s] = jlimit (0.0f, 1.0f, (outDb[s] + 54.0f) / 48.0f);
            hot[s] = jlimit (0.0f, 1.0f, (outDb[s] + 14.0f) / 10.0f);
            red[s] = jlimit (0.0f, 1.0f, (outDb[s] + 3.5f) / 3.5f);
        }
        act[2] = jmax (act[0], act[1]); hot[2] = jmax (hot[0], hot[1]); red[2] = jmax (red[0], red[1]);
        for (int i = 0; i < 3; ++i) actSide[i] = act[i];
        actMax = act[2];
        const float kf = jmax (warm[0], warm[1]);
        const Colour warmBg   = dark ? Colour (0xff4a3612) : Colour (0xffe0a63a);
        const Colour warmWell = dark ? Colour (0xff5a4418) : Colour (0xffebb857);
        const Colour redC (0xffff4a3a);
        F.bg   = B.bg.interpolatedWith (warmBg, 0.5f * kf);
        F.well = B.well.interpolatedWith (warmWell, 0.5f * kf);
        F.ink  = B.ink;
        F.hi   = B.hi;
        for (int i = 0; i < 3; ++i)
        {
            F.cell[i] = B.cell[i].interpolatedWith (B.hi, 0.3f * hot[i]).interpolatedWith (redC, 0.85f * red[i]);
            F.face[i] = B.face[i].interpolatedWith (B.hi, 0.25f * act[i] + 0.4f * hot[i]);
        }
        lineAlpha = 0.45f + 0.35f * act[2];
    }
    Colour ink (float a = 1.0f) { return F.ink.withAlpha (a); }
    Colour lineCol()            { return F.ink.withAlpha (lineAlpha); }
    Colour dimCol()             { return F.ink.withAlpha (0.55f); }
    Colour cellOf (int side)    { return F.cell[side < 0 ? 2 : side]; }
    Colour faceOf (int side)    { return F.face[side < 0 ? 2 : side]; }
    float  actOf (int side)     { return actSide[side < 0 ? 2 : side]; }

    // Inter for names and symbols, IBM Plex Mono for values (both SIL Open Font License), embedded
    // so the UI looks the same everywhere. px is the em size, as on a web page.
    Typeface::Ptr loadFace (const char* data, int size) { return Typeface::createSystemTypefaceFor (data, (size_t) size); }
    Font sans (float px, bool medium = false)
    {
        static Typeface::Ptr r = loadFace (BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);
        static Typeface::Ptr m = loadFace (BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize);
        return Font (FontOptions (medium ? m : r).withPointHeight (px));
    }
    Font mono (float px, bool medium = false)
    {
        static Typeface::Ptr r = loadFace (BinaryData::IBMPlexMonoRegular_ttf, BinaryData::IBMPlexMonoRegular_ttfSize);
        static Typeface::Ptr m = loadFace (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);
        return Font (FontOptions (medium ? m : r).withPointHeight (px));
    }

    double now() { return Time::getMillisecondCounterHiRes() * 0.001; }

    void txt (Graphics& g, const String& s, Rectangle<float> r, const Font& f, Colour c, Justification j = Justification::centred)
    {
        g.setFont (f); g.setColour (c); g.drawText (s, r, j, false);
    }
    void txtAt (Graphics& g, const String& s, Point<float> at, const Font& f, Colour c, Justification j = Justification::centred)
    {
        txt (g, s, Rectangle<float> (400.0f, 40.0f).withCentre (at), f, c, j);
    }
    void txtLeft (Graphics& g, const String& s, Point<float> at, const Font& f, Colour c)
    {
        txt (g, s, Rectangle<float> (at.x, at.y - 20.0f, 600.0f, 40.0f), f, c, Justification::centredLeft);
    }
    void txtRight (Graphics& g, const String& s, Point<float> at, const Font& f, Colour c)
    {
        txt (g, s, Rectangle<float> (at.x - 600.0f, at.y - 20.0f, 600.0f, 40.0f), f, c, Justification::centredRight);
    }

    // the faltung chip: a skewed parallelogram
    Path para (Point<float> c, float w, float h, float skew = SK)
    {
        const float k = std::tan (skew) * h * 0.5f;
        Path p;
        p.startNewSubPath (c.x - w * 0.5f + k, c.y - h * 0.5f);
        p.lineTo (c.x + w * 0.5f + k, c.y - h * 0.5f);
        p.lineTo (c.x + w * 0.5f - k, c.y + h * 0.5f);
        p.lineTo (c.x - w * 0.5f - k, c.y + h * 0.5f);
        p.closeSubPath();
        return p;
    }
    void dashed (Graphics& g, Point<float> a, Point<float> b, Colour c, float w = 1.0f, float on = 3.0f, float off = 5.0f)
    {
        const float dl[] = { on, off };
        Path p; p.startNewSubPath (a); p.lineTo (b);
        Path d; PathStrokeType (w).createDashedStroke (d, p, dl, 2);
        g.setColour (c); g.fillPath (d);
    }
    [[maybe_unused]] void dashedPath (Graphics& g, const Path& p, Colour c, float w = 1.0f, float on = 2.0f, float off = 5.0f)
    {
        const float dl[] = { on, off };
        Path d; PathStrokeType (w).createDashedStroke (d, p, dl, 2);
        g.setColour (c); g.fillPath (d);
    }

    // A floating cell: a folded paper strip. The body is flat colour; a narrower, lighter face
    // across the top is the fold, as if the strip turned over, with a hairline where it bends.
    void strip (Graphics& g, Point<float> c, float w, float h, Colour col, Colour face, bool hover = false)
    {
        g.setColour (col); g.fillPath (para (c, w, h));
        const Point<float> fc (c.x, c.y - h * 0.5f + 9.0f);
        g.setColour (face); g.fillPath (para (fc, w - 2.0f, 18.0f, -SK));
        g.setColour (F.hi);
        g.fillRect (fc.x - w * 0.5f + 6.0f, fc.y + 8.25f, w - 16.0f, 1.5f);
        if (hover) { g.setColour (F.hi.withAlpha (0.9f)); g.strokePath (para (c, w, h), PathStrokeType (1.2f)); }
    }
    // the level as a stack of chips (the kt1 grid cell), lit up to the value; mod >= 0 marks
    // where modulation has it (an ink hairline across the stack)
    void chips (Graphics& g, Point<float> c, float v, float mod, bool mini)
    {
        const int count = 7;
        const float cw = mini ? 22.0f : 30.0f, ch = mini ? 4.0f : 5.0f, step = mini ? 5.5f : 7.0f;
        const int lit = roundToInt (jlimit (0.0f, 1.0f, v) * count);
        for (int i = 0; i < count; ++i)
        {
            const Point<float> p (c.x, c.y + step * 3.0f - (float) i * step);
            const auto ch1 = para (p, cw, ch);
            if (i < lit) { g.setColour (F.hi); g.fillPath (ch1); }
            g.setColour (i < lit ? F.hi : lineCol());
            g.strokePath (ch1, PathStrokeType (i == lit - 1 ? 1.5f : 1.0f));
        }
        if (mod >= 0.0f && std::abs (mod - v) > 0.004f)
        {
            const float y = c.y + step * 3.0f + ch * 0.5f - jlimit (0.0f, 1.0f, mod) * (step * 6.0f + ch);
            g.setColour (F.ink); g.fillRect (c.x - cw * 0.5f - 5.0f, y - 0.5f, cw + 10.0f, 1.0f);
        }
    }
    // the level as the faltung slider: a thin rule, a skewed fill up to the value with a bright
    // edge; pulse (0..1) brightens the fill (an LFO breathing in it)
    void vial (Graphics& g, Point<float> c, float w, float h, float v, float mod, float pulse = 0.0f)
    {
        g.setColour (ink (0.35f)); g.fillRect (c.x - 0.5f, c.y - h * 0.5f, 1.0f, h);
        const float fh = jlimit (0.0f, 1.0f, v) * h, k = std::tan (SK) * w * 0.5f;
        if (fh > 0.5f)
        {
            Path f;
            f.startNewSubPath (c.x - w * 0.5f, c.y + h * 0.5f + k);
            f.lineTo (c.x + w * 0.5f, c.y + h * 0.5f - k);
            f.lineTo (c.x + w * 0.5f, c.y + h * 0.5f - fh - k);
            f.lineTo (c.x - w * 0.5f, c.y + h * 0.5f - fh + k);
            f.closeSubPath();
            g.setColour (F.hi.withAlpha (0.82f + 0.18f * pulse)); g.fillPath (f);
        }
        Path e;
        e.startNewSubPath (c.x - w * 0.5f, c.y + h * 0.5f - fh + k);
        e.lineTo (c.x + w * 0.5f, c.y + h * 0.5f - fh - k);
        g.setColour (F.hi); g.strokePath (e, PathStrokeType (2.0f));
        if (mod >= 0.0f && std::abs (mod - v) > 0.004f)
        {
            const float mh = jlimit (0.0f, 1.0f, mod) * h;
            Path m;
            m.startNewSubPath (c.x - w * 0.5f - 3.0f, c.y + h * 0.5f - mh + k);
            m.lineTo (c.x + w * 0.5f + 3.0f, c.y + h * 0.5f - mh - k);
            g.setColour (F.ink); g.strokePath (m, PathStrokeType (1.0f));
        }
    }
    // the collar: the band alone riding the rule (the cutoff); mod >= 0 draws a hollow band
    void collar (Graphics& g, Point<float> c, float w, float h, float v, float mod)
    {
        g.setColour (ink (0.35f)); g.fillRect (c.x - 0.5f, c.y - h * 0.5f, 1.0f, h);
        if (mod >= 0.0f && std::abs (mod - v) > 0.004f)
        {
            g.setColour (F.ink);
            g.strokePath (para ({ c.x, c.y + h * 0.5f - jlimit (0.0f, 1.0f, mod) * h }, w + 4.0f, 7.0f), PathStrokeType (1.0f));
        }
        g.setColour (F.hi);
        g.fillPath (para ({ c.x, c.y + h * 0.5f - jlimit (0.0f, 1.0f, v) * h }, w + 4.0f, 7.0f));
    }
    // a button face: an outlined chip; filled in the side's colour with a bright border when on
    void chip (Graphics& g, Point<float> c, float w, float h, const String& label, bool on, bool over, Colour col, float px, bool monoFont)
    {
        const auto p = para (c, w, h);
        if (on) { g.setColour (col); g.fillPath (p); g.setColour (F.hi); g.strokePath (p, PathStrokeType (1.5f)); }
        else    { g.setColour (over ? ink (0.8f) : lineCol()); g.strokePath (p, PathStrokeType (over ? 1.4f : 1.0f)); }
        txtAt (g, label, { c.x, c.y + 0.5f }, monoFont ? mono (px, on) : sans (px, on), on ? F.hi : F.ink);
    }

    // Each parameter is named by a Greek letter (its full name is in the tooltip)
    String symbolFor (const String& name)
    {
        static const std::pair<const char*, const char*> map[] = {
            { "Input", "\xce\xb3" },   // gamma: gain
            { "Cutoff", "\xcf\x89" },  // omega: cutoff frequency
            { "Reso", "\xce\xb6" },    // zeta: damping / resonance
            { "Mode", "\xce\xbc" },    // mu: morph between filter modes
            { "Drive", "\xce\xba" },   // kappa: the curve of the tube
            { "Time", "\xcf\x84" },    // tau: delay time
            { "Regen", "\xcf\x86" },   // phi: feedback
            { "Mix", "\xce\xb7" },     // eta: dry / wet ratio
            { "Rand", "\xce\xbe" },    // xi: chance of a random freeze
            { "Out", "\xce\xa9" },     // Omega: the end of the chain
            { "Speed", "\xce\xbd" },   // nu: how fast the attractor runs
            { "Chaos", "\xcf\x81" },   // rho: the Lorenz rho
            { "Shape", "\xce\xb2" },   // beta: the Lorenz beta
            { "Depth", "\xce\x94" },   // Delta: how far it moves the cutoffs
            { "Width", "\xcf\x88" },   // psi: poly spread
            { "Source", "\xcf\x83" },  // sigma: the built-in recording's level
            { "Feedback", "\xce\xa6" },// Phi: master feedback, out back into in
            { "Flock", "\xcf\x87" },   // chi: chaos for the poly taps (a boids flock moves their times)
            { "Distance", "\xce\xbb" },// lambda: how far out the echoes fly
        };
        for (auto& [n, sym] : map) if (name == n) return String::fromUTF8 (sym);
        return name;
    }

    // short value text: no units, no prefixes (the full text shows while the control is hovered)
    String compactValue (const String& full)
    {
        auto t = full.trim();
        for (auto* cut : { " Hz", "Hz", " ms", "%" }) t = t.replace (cut, "");
        t = t.replace (String::fromUTF8 ("\xcf\x81 "), "").replace (String::fromUTF8 ("\xce\xb2 "), "");
        if (t.containsChar ('>')) t = t.upToFirstOccurrenceOf (" ", false, false).replace (">", String::fromUTF8 ("\xe2\x80\xba"));
        if (t == "0.0" || t == "-0.0") t = "0";
        else if (t.startsWith ("0.")) t = t.substring (1);
        return t.toLowerCase();
    }

    //==========================================================================
    // Each cell renders itself (and its controls) once into a cached image, at the screen's pixel
    // density, and again only when something inside it changes. Drifting (a transform change)
    // keeps the picture.
    struct CellCache : public CachedComponentImage
    {
        Component& owner;
        Image img;
        bool dirty = true, holdForMove = false;
        float renderedScale = 0.0f;
        explicit CellCache (Component& c) : owner (c) {}
        void paint (Graphics& g) override
        {
            const float phys = g.getInternalContext().getPhysicalPixelScaleFactor();
            const float scale = std::ceil (phys * density * 8.0f - 0.05f) / 8.0f;
            const auto cb = owner.getLocalBounds();
            const int iw = jmax (1, roundToInt ((float) cb.getWidth() * scale)), ih = jmax (1, roundToInt ((float) cb.getHeight() * scale));
            if (img.isNull() || img.getWidth() != iw || img.getHeight() != ih)
            {
                img = Image (Image::ARGB, iw, ih, true, SoftwareImageType());
                img.setBackupEnabled (false);
                dirty = true;
            }
            if (dirty)
            {
                img.clear (img.getBounds());
                Graphics ig (img);
                ig.addTransform (AffineTransform::scale ((float) iw / (float) cb.getWidth(), (float) ih / (float) cb.getHeight()));
                owner.paintEntireComponent (ig, true);
                ++chloaelisDbgRenders;
                dirty = false;
            }
            g.setColour (Colours::black.withAlpha (owner.getAlpha()));
            g.setImageResamplingQuality (Graphics::mediumResamplingQuality);
            g.drawImageTransformed (img, AffineTransform::scale ((float) cb.getWidth() / (float) iw, (float) cb.getHeight() / (float) ih), false);
        }
        bool invalidateAll() override                    { if (! holdForMove) dirty = true; return true; }
        bool invalidate (const Rectangle<int>&) override { dirty = true; return true; }
        void releaseResources() override                 { img = {}; }
    };

    // a line of text in the field's ink (so it follows the mode and the level)
    struct Txt : public Component
    {
        String text;
        float px = 10.0f;
        bool monoFont = false, medium = false, faint = false;
        Txt() { setInterceptsMouseClicks (false, false); }
        void set (const String& s) { if (s != text) { text = s; repaint(); } }
        void paint (Graphics& g) override
        {
            g.setFont (monoFont ? mono (px, medium) : sans (px, medium));
            g.setColour (faint ? dimCol() : F.ink);
            g.drawText (text, getLocalBounds().toFloat(), Justification::centred, false);
        }
    };

    //==========================================================================
    // a knob: a stack of chips lit to the value (dragged up and down); or the whole cell as a vial
    // (filled to the value) or a collar (a band riding a rule)
    struct Knob : public Slider
    {
        int side = 0;
        bool mini = false;
        const std::atomic<float>* mod = nullptr;   // modulated position (normalised) -> a marker
        const std::atomic<float>* lfo = nullptr;   // an LFO that breathes in the fill while the knob is up
        float lastMod = -1.0f, lastLfo = -9.0f;
        bool lastHover = false;
        int style = 0;                  // 0 chips, 1 vial, 2 collar
        Point<float> at;                // where the level is drawn, in this knob's coords
        float lw = 22.0f, lh = 50.0f;   // the vial's / collar's size
        int animTick = 0;
        Knob() : Slider (RotaryHorizontalVerticalDrag, NoTextBox)
        {
            setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            setMouseDragSensitivity (240);
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void poll()
        {
            bool dirty = isMouseOverOrDragging() != lastHover;
            lastHover = isMouseOverOrDragging();
            if (mod != nullptr)
            {
                const float m = mod->load (std::memory_order_relaxed);
                if (std::abs (m - lastMod) > 0.004f) { lastMod = m; dirty = true; }
            }
            if (lfo != nullptr && getValue() > 0.0)
            {
                const float v = lfo->load (std::memory_order_relaxed);
                if (std::abs (v - lastLfo) > 0.05f && ++animTick % 3 == 0) { lastLfo = v; dirty = true; }
            }
            if (dirty) repaint();
        }
    };

    struct FlatLnF : public LookAndFeel_V4
    {
        FlatLnF()
        {
            setColour (PopupMenu::backgroundColourId, Colour (0xff23200a));
            setColour (PopupMenu::textColourId, Colour (0xffe8dfb0));
            setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xffee8e6e));
            setColour (PopupMenu::highlightedTextColourId, Colour (0xff23200a));
        }
        Font getPopupMenuFont() override { return mono (13.0f); }
        void drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider& s) override
        {
            auto* k = dynamic_cast<Knob*> (&s);
            const float m = (k != nullptr && k->mod != nullptr) ? k->mod->load (std::memory_order_relaxed) : -1.0f;
            if (k == nullptr) { chips (g, Rectangle<float> ((float) x, (float) y, (float) w, (float) h).getCentre(), pos, m, false); return; }
            if (k->style == 1)
            {
                const float pulse = (k->lfo != nullptr && s.getValue() > 0.0) ? jlimit (0.0f, 1.0f, k->lfo->load (std::memory_order_relaxed)) : 0.0f;
                vial (g, k->at, k->lw, k->lh, pos, m, pulse);
            }
            else if (k->style == 2) collar (g, k->at, k->lw, k->lh, pos, m);
            else chips (g, k->at, pos, m, k->mini);
        }
        Font getSliderPopupFont (Slider&) override { return mono (11.0f); }
        int getSliderPopupPlacement (Slider&) override { return BubbleComponent::above; }
        void drawBubble (Graphics& g, BubbleComponent&, const Point<float>&, const Rectangle<float>& body) override
        {
            g.setColour (F.bg); g.fillRect (body);
            g.setColour (F.ink); g.drawRect (body, 1.0f);
        }
        void drawTooltip (Graphics& g, const String& t, int w, int h) override
        {
            g.fillAll (F.bg);
            g.setColour (F.ink); g.drawRect (0, 0, w, h);
            g.setFont (mono (11.0f));
            g.drawFittedText (t.toLowerCase(), Rectangle<int> (w, h).reduced (9, 4), Justification::centredLeft, 3);
        }
    };

    //==========================================================================
    // a button: an outlined chip, filled when on; it may carry a second line under it
    struct Cell : public Button
    {
        String a, offText, subOn, subOff;
        int side;
        float px = 10.0f;
        bool monoFont = true;
        bool held = false;          // show it on anyway (the freeze cell fills whenever the buffer is held)
        Cell (String text, int s) : Button ({}), a (std::move (text)), side (s)
        {
            setClickingTogglesState (true);
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void paintButton (Graphics& g, bool over, bool down) override
        {
            const bool on = getToggleState() || (down && ! getClickingTogglesState());
            const bool sub = subOn.isNotEmpty();
            const auto c = getLocalBounds().toFloat().getCentre().translated (0.0f, sub ? -7.0f : 0.0f);
            chip (g, c, (float) getWidth() - 6.0f, 20.0f, (! on && ! held && offText.isNotEmpty()) ? offText : a, on || held, over, cellOf (side), px, monoFont);
            if (sub) txtAt (g, (on || held) ? subOn : subOff, c.translated (0.0f, 22.0f), mono (9.0f), dimCol());
        }
    };

    // a row of chips for a choice parameter (one filled)
    struct Seg : public Component, public SettableTooltipClient
    {
        StringArray opts;
        int side, value = 0, hover = -1;
        float px = 10.0f;
        bool monoFont = true;
        std::unique_ptr<ParameterAttachment> att;
        Seg (RangedAudioParameter& p, StringArray o, int s) : opts (std::move (o)), side (s)
        {
            att = std::make_unique<ParameterAttachment> (p, [this] (float v) { value = (int) std::lround (v); repaint(); });
            att->sendInitialUpdate();
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        int indexAt (float x) const { return jlimit (0, opts.size() - 1, (int) (x / ((float) getWidth() / (float) opts.size()))); }
        void mouseDown (const MouseEvent& e) override { att->setValueAsCompleteGesture ((float) indexAt (e.position.x)); }
        void mouseMove (const MouseEvent& e) override { const int h = indexAt (e.position.x); if (h != hover) { hover = h; repaint(); } }
        void mouseExit (const MouseEvent&) override { hover = -1; repaint(); }
        void paint (Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat().reduced (3.0f, 0.0f);
            const int k = opts.size();
            const float gap = 4.0f, cw = (r.getWidth() - gap * (float) (k - 1)) / (float) k;
            for (int i = 0; i < k; ++i)
                chip (g, { r.getX() + cw * 0.5f + (float) i * (cw + gap), r.getCentreY() }, cw - 4.0f, 20.0f, opts[i], i == value, i == hover, cellOf (side), px, monoFont);
        }
    };

    // the live (modulated) filter response: a thin ink line on a well
    struct FilterPlot : public Component
    {
        ChloaelisProcessor& proc;
        int side;
        float lastSig = -1.0f;
        FilterPlot (ChloaelisProcessor& p, int s) : proc (p), side (s) { setInterceptsMouseClicks (false, false); }
        float val (int p) const { return chl::rangeFor (p).convertFrom0to1 (proc.paramNorm[(size_t) side][(size_t) p].load()); }
        void update()
        {
            const float sig = val (chl::cutoff) * 0.001f + val (chl::res) * 7.0f + val (chl::morph) * 13.0f;
            if (std::abs (sig - lastSig) > 1e-4f) { lastSig = sig; repaint(); }
        }
        void paint (Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat().reduced (3.0f);
            const auto well = para (r.getCentre(), r.getWidth(), r.getHeight());
            g.setColour (F.well); g.fillPath (well);
            g.setColour (lineCol()); g.strokePath (well, PathStrokeType (1.0f));
            const auto in = r.reduced (14.0f, 8.0f);
            auto fx = [&] (float f) { return in.getX() + in.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
            auto dy = [&] (float db) { return in.getY() + in.getHeight() * (1.0f - (db + 36.0f) / 54.0f); };
            const float fc = val (chl::cutoff), k = 2.0f - 1.96f * val (chl::res), m = jlimit (0.0f, 3.0f, val (chl::morph));
            const int seg = jmin (2, (int) m);
            const float t = m - (float) seg;
            Path p;
            for (int i = 0; i <= 120; ++i)
            {
                const float f = 20.0f * std::pow (1000.0f, (float) i / 120.0f);
                const std::complex<float> s (0.0f, f / fc);
                const auto D = s * s + k * s + 1.0f;
                const std::complex<float> resp[4] = { 1.0f / D, k * s / D, s * s / D, (s * s + 1.0f) / D };
                const auto hcx = resp[seg] + (resp[seg + 1] - resp[seg]) * t;
                const float db = jlimit (-36.0f, 18.0f, 20.0f * std::log10 (std::abs (hcx) + 1e-6f));
                const Point<float> q (fx (f), dy (db));
                if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
            }
            g.setColour (F.ink); g.strokePath (p, PathStrokeType (1.2f));
            g.setColour (F.hi); g.fillRect (in.getX(), in.getBottom() - 0.5f, in.getWidth(), 1.0f);
        }
    };

    // tape condition: a row of chips, filled while there is tape left, thinning as it wears
    struct TapeStrip : public Component
    {
        ChloaelisProcessor& proc;
        int side;
        float last = -1.0f;
        bool lastActive = false;
        TapeStrip (ChloaelisProcessor& p, int s) : proc (p), side (s) { setInterceptsMouseClicks (false, false); }
        bool active() const { return proc.apvts.getRawParameterValue (chl::delayTypeId (side))->load() < 0.5f; }
        void update()
        {
            const float a = proc.tapeAge[(size_t) side].load();
            if (std::abs (a - last) > 0.002f || active() != lastActive) { last = a; lastActive = active(); repaint(); }
        }
        void paint (Graphics& g) override
        {
            const auto c = getLocalBounds().toFloat().getCentre();
            const float left = 1.0f - jlimit (0.0f, 1.0f, last);
            const int filled = roundToInt (11.0f * left);
            for (int i = 0; i < 11; ++i)
            {
                const auto p = para ({ c.x - 45.0f + (float) i * 9.0f, c.y - 6.0f }, 7.0f, 7.0f);
                if (i < filled) { g.setColour (lastActive ? cellOf (side) : dimCol()); g.fillPath (p); }
                g.setColour (lineCol()); g.strokePath (p, PathStrokeType (1.0f));
            }
            txtAt (g, "tape " + String (roundToInt (left * 100.0f)) + "%", c.translated (0.0f, 9.0f), mono (9.0f), dimCol());
        }
    };

    //==========================================================================
    // A cell in the network: a folded strip at its own tilt (the whole cell turns, contents and
    // all), drifting slowly about its home. The first cell of a section carries its callout.
    struct Node : public Component
    {
        float cw, ch, angle, phase;
        bool drawCell;
        int side = -1;
        Point<float> home;
        String title;
        float tdx = 0.0f, tdy = 0.0f;    // the callout's offset (0: the usual place)
        float hold = 0.0f;               // 1 while the cursor rests on it: the drift eases off
        bool hov = false;
        static constexpr float pad = 34.0f;

        Node (Point<float> centre, float w, float h, float ang, bool cellBg, float ph)
            : cw (w), ch (h), angle (ang), phase (ph), drawCell (cellBg), home (centre)
        {
            setBounds (Rectangle<float> (w + pad * 2, h + pad * 2).withCentre (centre).toNearestInt());
            setCachedComponentImage (cache = new CellCache (*this));
        }
        CellCache* cache = nullptr;     // owned by the component
        // drift: move without throwing the cached render away
        void moveTo (const AffineTransform& t)
        {
            cache->holdForMove = true;
            setTransform (t);
            cache->holdForMove = false;
        }
        Point<float> c() const { return getLocalBounds().toFloat().getCentre(); }
        bool hitTest (int x, int y) override
        {
            // inside the skewed strip, with a little margin
            const auto p = Point<float> ((float) x, (float) y) - c();
            const float k = std::tan (SK) * ch * 0.5f, shift = -k * p.y / (ch * 0.5f);
            return std::abs (p.x - shift) <= cw * 0.5f + 6.0f && std::abs (p.y) <= ch * 0.5f + 6.0f;
        }
        template <typename T> T* place (T* comp, Point<float> offset, float w, float h)
        {
            addAndMakeVisible (comp);
            comp->setBounds (Rectangle<float> (w, h).withCentre (c() + offset).toNearestInt());
            return comp;
        }
        void paint (Graphics& g) override
        {
            if (drawCell) strip (g, c(), cw, ch, cellOf (side), faceOf (side), hov);
        }
    };

    // The vessel (standalone only): drop an audio file into it. Its waveform runs up the inside
    // as a stack of strips (the start of the file at the bottom), the strips inside the loop in
    // the sides' colours, two bands mark the loop, a hairline rides the playhead, and the rule on
    // the right is its level. Drag a band to move that loop point, drag anywhere else to set the
    // level, double-click to choose a file.
    struct VesselView : public Component, public FileDragAndDropTarget
    {
        ChloaelisProcessor& proc;
        Rectangle<float> body;
        bool dragOver = false;
        int dragging = 0;            // 1 = start band, 2 = end band, 3 = level
        float levelAtDown = 0.0f, lastPos = -1.0f;
        std::unique_ptr<FileChooser> chooser;

        explicit VesselView (ChloaelisProcessor& p) : proc (p) { setMouseCursor (MouseCursor::PointingHandCursor); }

        float yFor (float t) const { return body.getBottom() - 8.0f - jlimit (0.0f, 1.0f, t) * (body.getHeight() - 16.0f); }
        float tFor (float y) const { return jlimit (0.0f, 1.0f, (body.getBottom() - 8.0f - y) / (body.getHeight() - 16.0f)); }

        void poll (int frame)
        {
            const float p = proc.vesselPos.load();
            const bool moving = proc.vesselForUi() != nullptr && std::abs (p - lastPos) > 0.0005f;
            if ((moving && frame % 2 == 0) || frame % 6 == 0) { lastPos = p; repaint(); }
        }

        void paint (Graphics& g) override
        {
            const auto clip = proc.vesselForUi();
            const bool hover = isMouseOverOrDragging() || dragOver;
            const auto well = para (body.getCentre(), body.getWidth(), body.getHeight());
            g.setColour (F.well); g.fillPath (well);
            g.setColour (hover ? F.ink : lineCol()); g.strokePath (well, PathStrokeType (hover ? 1.4f : 1.0f));
            // the level: a rule on the right with a band at the volume
            const float lx = body.getRight() + 8.0f, top = body.getY() + 10.0f, bot = body.getBottom() - 10.0f;
            g.setColour (ink (0.35f)); g.fillRect (lx - 0.5f, top, 1.0f, bot - top);
            g.setColour (F.hi); g.fillPath (para ({ lx, bot - proc.vesselLevel.load() * (bot - top) }, 10.0f, 5.0f));
            if (clip == nullptr)
            {
                txtAt (g, String::fromUTF8 ("\xe2\x86\x93"), body.getCentre(), sans (24.0f), dragOver ? F.ink : dimCol());
                return;
            }
            const float s = proc.vesselStart.load(), e = proc.vesselEnd.load();
            const float lo = jmin (s, e), hi = jmax (s, e);
            // the waveform as strips, bottom (start) to top (end); the loop in the sides' colours
            const int n = (int) clip->peaks.size();
            const float halfW = body.getWidth() * 0.4f;
            for (float y = -0.5f * (body.getHeight() - 20.0f); y <= 0.5f * (body.getHeight() - 20.0f); y += 8.0f)
            {
                const float t = tFor (body.getCentreY() + y);
                const int k = jlimit (0, n - 1, (int) (t * (float) n));
                const float w = 4.0f + jmin (1.0f, clip->peaks[(size_t) k] * 1.6f) * (halfW - 4.0f);
                const bool live = t >= lo && t <= hi;
                const auto p = para ({ body.getCentreX(), body.getCentreY() + y }, w * 2.0f, 5.0f);
                g.setColour (live ? cellOf (t < (lo + hi) * 0.5f ? 0 : 1) : dimCol()); g.fillPath (p);
                if (live) { g.setColour (F.hi); g.strokePath (p, PathStrokeType (1.0f)); }
            }
            // the playhead
            const float py = yFor (proc.vesselPos.load());
            g.setColour (F.hi); g.fillRect (body.getX() + 6.0f, py - 0.5f, body.getWidth() - 12.0f, 1.0f);
        }
        void paintOverChildren (Graphics& g) override
        {
            if (proc.vesselForUi() == nullptr) return;
            const float s = proc.vesselStart.load(), e = proc.vesselEnd.load();
            for (auto [t, drag] : { std::pair { s, 1 }, std::pair { e, 2 } })
            {
                const auto p = para ({ body.getCentreX(), yFor (t) }, body.getWidth() + 6.0f, 7.0f);
                g.setColour (F.hi); g.fillPath (p);
                g.setColour (dragging == drag ? F.ink : lineCol()); g.strokePath (p, PathStrokeType (1.0f));
            }
        }

        bool hitTest (int x, int y) override { return body.expanded (14.0f, 8.0f).contains ((float) x, (float) y); }
        void mouseDown (const MouseEvent& ev) override
        {
            const float y = ev.position.y;
            const float ys = yFor (proc.vesselStart.load()), ye = yFor (proc.vesselEnd.load());
            const bool has = proc.vesselForUi() != nullptr;
            if (has && std::abs (y - ys) < 10.0f && std::abs (y - ys) <= std::abs (y - ye)) dragging = 1;
            else if (has && std::abs (y - ye) < 10.0f) dragging = 2;
            else { dragging = 3; levelAtDown = proc.vesselLevel.load(); }
            repaint();
        }
        void mouseDrag (const MouseEvent& ev) override
        {
            if (dragging == 1) proc.vesselStart.store (jmin (tFor (ev.position.y), proc.vesselEnd.load() - 0.005f));
            else if (dragging == 2) proc.vesselEnd.store (jmax (tFor (ev.position.y), proc.vesselStart.load() + 0.005f));
            else if (dragging == 3) proc.vesselLevel.store (jlimit (0.0f, 1.0f, levelAtDown - (float) ev.getDistanceFromDragStartY() / 180.0f));
            repaint();
        }
        void mouseUp (const MouseEvent&) override { dragging = 0; repaint(); }
        void mouseDoubleClick (const MouseEvent&) override
        {
            chooser = std::make_unique<FileChooser> ("Pour audio into the vessel", File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
            {
                if (fc.getResult().existsAsFile()) { proc.loadVesselFile (fc.getResult()); repaint(); }
            });
        }
        static bool isAudio (const String& path)
        {
            for (auto* ext : { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".m4a", ".ogg", ".caf" })
                if (path.endsWithIgnoreCase (ext)) return true;
            return false;
        }
        bool isInterestedInFileDrag (const StringArray& files) override { for (auto& f : files) if (isAudio (f)) return true; return false; }
        void fileDragEnter (const StringArray&, int, int) override { dragOver = true; repaint(); }
        void fileDragExit (const StringArray&) override { dragOver = false; repaint(); }
        void filesDropped (const StringArray& files, int, int) override
        {
            dragOver = false;
            for (auto& f : files)
                if (isAudio (f) && proc.loadVesselFile (File (f))) break;
            repaint();
        }
    };

    // The help screen: the key to every symbol and how to play it. Opened with the ? cell,
    // closed by clicking anywhere (or Esc in the desktop app).
    struct HelpOverlay : public Component
    {
        bool desktop = false;
        HelpOverlay() { setBufferedToImage (true); }
        void mouseDown (const MouseEvent&) override { setVisible (false); }

        void paint (Graphics& g) override
        {
            g.fillAll (F.bg);
            const auto area = getLocalBounds().toFloat().reduced (70.0f, 40.0f);
            txt (g, String::fromUTF8 ("chloaelis  \xc2\xb7  help"), area.withHeight (30), sans (24.0f, true), F.ink, Justification::centredLeft);
            txt (g, "click anywhere to close", area.withHeight (30).translated (0, 26), mono (10.0f), dimCol(), Justification::centredLeft);

            struct Row { const char* sym; const char* text; };
            const Row strip[] = {
                { "\xce\xb3", "input level" },
                { "\xcf\x89", "filter cutoff: drag the band up and down (a hollow band shows modulation)" },
                { "\xce\xb6", "resonance" },
                { "\xce\xbc", "filter mode: low pass > band pass > high pass > notch" },
                { "\xce\xba", "tube drive" },
                { "tube on", "the tube in or out of the path" },
                { "i-iv", "tube type: one wire 12ax7, two el84, three 6l6 with sag, four starved" },
                { "\xcf\x84", "delay time" },
                { "slew fade", "time changes: glide in pitch (slew), or cross over (fade)" },
                { "hpf", "high pass in the delay's feedback" },
                { "refresh", "put on fresh tape (the row of chips beside it thins as the tape wears)" },
                { "freeze", "hold the delay buffer (the chip fills while it is held)" },
                { "\xcf\x99", "karplus: time becomes a string's pitch, tuned to the scale in the middle (regen is its sustain)" },
                { "\xcf\x86", "regen: the delay's feedback (past ~90% it sings on its own)" },
                { "\xce\xb7", "delay mix, dry to wet" },
                { "\xce\xbe", "random freeze: the chance the buffer gets held at random" },
                { "\xce\xa9", "output level" },
                { "~", "lfo: the chips are its rate, the fill its depth (the letter says what it moves)" } };
            const Row shared[] = {
                { "\xce\xbd", "lorenz speed" },
                { "\xcf\x81", "lorenz chaos" },
                { "\xce\xb2", "lorenz shape" },
                { "\xce\x94", "how far the lorenz attractor moves both cutoffs" },
                { "\xcf\x89\xe2\x80\xba\xcf\x84", "order: filter then delay, or delay then filter" },
                { "\xcf\x88", "poly: up to 8 delay taps per side, a reverb-like cloud at the top" },
                { "\xcf\x87", "flock: a boids swarm moves the poly taps' times" },
                { "\xcf\x83", "level of the built-in recording (fade it in and out)" },
                { "\xe2\x88\x9e \xe2\x89\x88", "which recording: the loop sign is the base recording, the wave sign the hydrophone" },
                { "\xce\xa6", "master feedback, crossed: left out > right in, right out > left in" },
                { "\xe2\x86\xba", "reset everything to the defaults" },
                { "\xe2\x98\xbe \xe2\x98\x80", "light or dark (it follows your system until you choose)" } };

            auto column = [&] (Rectangle<float> r, const char* head, const Row* rows, int n)
            {
                txt (g, head, r.removeFromTop (24), mono (10.0f, true), F.ink, Justification::centredLeft);
                for (int i = 0; i < n; ++i)
                {
                    auto ln = r.removeFromTop (27);
                    const String sym = String::fromUTF8 (rows[i].sym);
                    txt (g, sym, ln.removeFromLeft (64), sym.length() > 2 ? mono (10.0f, true) : sans (18.0f, true), F.ink, Justification::centredLeft);
                    g.setFont (mono (10.5f)); g.setColour (ink (0.85f));
                    g.drawFittedText (String::fromUTF8 (rows[i].text), ln.toNearestInt(), Justification::centredLeft, 2, 0.8f);
                }
            };
            auto body = area.withTrimmedTop (66);
            auto left = body.removeFromLeft (body.getWidth() * 0.5f).reduced (10, 0);
            auto right = body.reduced (10, 0);
            column (left, "each side  (left in > left out, right in > right out)", strip, (int) std::size (strip));
            column (right.removeFromTop (24 + 27 * (int) std::size (shared)), "shared (middle)", shared, (int) std::size (shared));

            right.removeFromTop (14);
            txt (g, "how to play", right.removeFromTop (24), mono (10.0f, true), F.ink, Justification::centredLeft);
            String how = String::fromUTF8 ("drag any stack of chips or fill up and down; double-click a knob to reset it.  "
                                           "switches (tube on, tape / pt2399 / bbd, slew / fade, filter \xe2\x80\xba dly) crossfade.  "
                                           "freeze holds the delay, refresh puts on fresh tape, hpf thins the feedback.  "
                                           "the colours follow the sound: the field warms with the input, each side's cells with its output.  "
                                           "it remembers how you left it.");
            if (desktop)
                how += String::fromUTF8 ("  vessel: drop an audio file in (or double-click it), drag the bands to set the loop, "
                                         "drag inside for its level.  \xe2\xa4\xa2 or cmd+f: fullscreen, esc to leave.");
            g.setFont (mono (10.5f)); g.setColour (ink (0.85f));
            g.drawFittedText (how, right.toNearestInt(), Justification::topLeft, 12, 0.9f);
        }
    };

    // section callouts: a lowercase word on a thin leader, drawn above all the cells
    struct Callouts : public Component
    {
        std::vector<Node*> nodes;
        Callouts() { setInterceptsMouseClicks (false, false); }
        void paint (Graphics& g) override
        {
            for (auto* n : nodes)
            {
                if (n->title.isEmpty()) continue;
                const auto c = n->getBounds().toFloat().getCentre().transformedBy (n->getTransform());
                const float m = c.x > W * 0.5f ? -1.0f : 1.0f;
                const float hh = n->ch * density * 0.5f;
                const Point<float> t (c.x + m * (n->tdx != 0.0f ? n->tdx : 58.0f), c.y - hh - 24.0f + n->tdy);
                const float a = n->getAlpha();
                const bool flip = n->tdx < 0.0f;     // the word sits on the far side (reads toward the cell)
                if ((m > 0) != flip) txtLeft (g, n->title, t, mono (10.0f, true), F.ink.withAlpha (a));
                else                 txtRight (g, n->title, t, mono (10.0f, true), F.ink.withAlpha (a));
                Path p;
                p.startNewSubPath (t.x, t.y + 8.0f);
                p.lineTo (t.x + m * 4.0f, t.y + 8.0f);
                p.lineTo (c.x + m * (n->tdx > 0.0f ? 24.0f : flip ? -24.0f : 12.0f), c.y - hh + 6.0f);
                g.setColour (lineCol().withMultipliedAlpha (a)); g.strokePath (p, PathStrokeType (1.0f));
            }
        }
    };

    // the dashed wireframe between cells (it follows the cells as they drift), brighter where
    // that side's output is louder
    struct Links : public Component
    {
        struct L { Component* a; Component* b; int side; };
        std::vector<L> links;
        float db[2] { -90.0f, -90.0f };
        Links() { setInterceptsMouseClicks (false, false); }
        static Point<float> centreOf (Component* c) { return c->getBounds().toFloat().getCentre().transformedBy (c->getTransform()); }
        void paint (Graphics& g) override
        {
            for (auto& l : links)
            {
                const auto a = centreOf (l.a), b = centreOf (l.b);
                const auto d = b - a;
                const float len = d.getDistanceFromOrigin();
                if (len < 1.0f) continue;
                const auto u = d / len;
                // start and end at the cell edges
                const auto s0 = a + u * (34.0f * density), s1 = b - u * (34.0f * density);
                if ((s1 - s0).getDotProduct (u) <= 0) continue;
                const float act = actOf (l.side);
                dashed (g, s0, s1, F.ink.interpolatedWith (F.hi, 0.6f * act).withAlpha (lineAlpha), 1.0f);
            }
        }
    };

    // The Lorenz attractor in the middle, as a thin line: a faint steady orbit (integrated here at
    // a steady pace, so the shape always shows), the real path (the processor's attractor, followed
    // closely) in ink with its newest stretch in the highlight, and a small chip for the head, which
    // sends dashed lines to both cutoffs (marching faster with the Depth knob). Behind it a thin
    // wireframe polyhedron with one translucent face: the fell sleeve.
    struct Attractor : public Component
    {
        ChloaelisProcessor& proc;
        struct P3 { float x, y, z; };
        std::vector<P3> ghost, trail;
        size_t gHead = 0, tHead = 0, gCount = 0, tCount = 0;
        double gx = 1.0, gy = 1.0, gz = 20.0, qx = 0.1, qy = 0.0, qz = 20.0;
        Component* cut[2] {};
        float spin = 0.0f, depth = 0.0f, send[2] {}, march = 0.0f, t = 0.0f;
        double rho = 28.0, beta = 8.0 / 3.0;
        Point<float> headPos;
        static constexpr float sc = 84.0f;
        Path ghostPath, trailOld, trailNew;

        explicit Attractor (ChloaelisProcessor& p) : proc (p)
        {
            setInterceptsMouseClicks (false, false);
            ghost.resize (2200); trail.resize (700);
            for (int i = 0; i < 3000; ++i) stepGhost (0.004);        // settle onto the attractor
        }
        static void lorenzStep (double& x, double& y, double& z, double h, double r, double b)
        {
            // midpoint (RK2) is plenty for a picture
            const double dx = 10.0 * (y - x), dy = x * (r - z) - y, dz = x * y - b * z;
            const double mx = x + dx * h * 0.5, my = y + dy * h * 0.5, mz = z + dz * h * 0.5;
            x += h * 10.0 * (my - mx); y += h * (mx * (r - mz) - my); z += h * (mx * my - b * mz);
            if (! std::isfinite (x) || std::abs (x) > 1e4) { x = 0.1; y = 0.0; z = 20.0; }
        }
        void stepGhost (double h)
        {
            lorenzStep (gx, gy, gz, h, rho, beta);
            ghost[gHead] = { (float) gx, (float) gy, (float) gz };
            gHead = (gHead + 1) % ghost.size(); gCount = jmin (gCount + 1, ghost.size());
        }
        void advance (float dt)
        {
            const double chaos = proc.apvts.getRawParameterValue (chl::lzChaosId)->load();
            rho  = 24.5 + chaos * chaos * 155.5;
            beta = proc.apvts.getRawParameterValue (chl::lzShapeId)->load();
            const double speed = 0.03 * std::exp2 (proc.apvts.getRawParameterValue (chl::lzSpeedId)->load() * 9.0);
            depth = proc.apvts.getRawParameterValue (chl::lzDepthId)->load();

            for (int i = 0; i < 6; ++i) stepGhost (0.0035);          // the shape: steady, whatever the speed

            // the real path: integrate along, then lean onto the processor's actual state
            const double simT = speed * dt;
            const int n = jlimit (1, 40, (int) std::ceil (simT / 0.004));
            for (int i = 0; i < n; ++i)
            {
                lorenzStep (qx, qy, qz, simT / n, rho, beta);
                trail[tHead] = { (float) qx, (float) qy, (float) qz };
                tHead = (tHead + 1) % trail.size(); tCount = jmin (tCount + 1, trail.size());
            }
            qx += (proc.lzX.load() - qx) * 0.3; qy += (proc.lzY.load() - qy) * 0.3; qz += (proc.lzZ.load() - qz) * 0.3;
            for (int s = 0; s < 2; ++s) send[s] += (proc.lzOut[(size_t) s].load() - send[s]) * 0.3f;

            spin += dt * 0.07f;
            t += dt;
            march += dt * (10.0f + 40.0f * depth);
            build();
        }
        // attractor coordinates -> design px
        Point<float> project (const P3& p) const
        {
            const float s = (float) std::sqrt (jmax (1.0, beta * (rho - 1.0)));
            const float X = p.x / s, Y = p.y / s, Z = (p.z - (float) (rho - 1.0)) / s;
            const float ca = std::cos (spin), sa = std::sin (spin);
            const float x1 = X * ca - Y * sa, y1 = X * sa + Y * ca;
            const float ct = std::cos (0.28f), st = std::sin (0.28f);
            const float z2 = Z * ct - y1 * st, d = Z * st + y1 * ct;
            const float f = 1.0f / (1.0f - d * 0.12f);
            return { W * 0.5f + x1 * sc * f, 405.0f - z2 * sc * f };
        }
        void build()
        {
            ghostPath.clear();
            for (size_t k = 0; k < gCount; k += 2)
            {
                const auto p = project (ghost[(gHead + ghost.size() - gCount + k) % ghost.size()]);
                if (k == 0) ghostPath.startNewSubPath (p); else ghostPath.lineTo (p);
            }
            trailOld.clear(); trailNew.clear();
            const size_t split = tCount > 120 ? tCount - 120 : 0;
            for (size_t k = 0; k < tCount; ++k)
            {
                const auto p = project (trail[(tHead + trail.size() - tCount + k) % trail.size()]);
                if (k <= split) { if (k == 0) trailOld.startNewSubPath (p); else trailOld.lineTo (p); }
                if (k >= split) { if (k == split) trailNew.startNewSubPath (p); else trailNew.lineTo (p); }
            }
            if (tCount > 0) headPos = project (trail[(tHead + trail.size() - 1) % trail.size()]);
        }
        void paint (Graphics& g) override
        {
            g.setOpacity (1.0f);
            // the polyhedron: a thin wireframe, slowly rocking, with one translucent face
            {
                const Point<float> pts[] = { { -150, -40 }, { -40, -150 }, { 120, -120 }, { 170, 30 }, { 60, 160 }, { -110, 130 }, { 0, 0 } };
                const float r = std::sin (t * 0.05f) * 0.02f, cr = std::cos (r), sr = std::sin (r);
                Point<float> P[7];
                for (int i = 0; i < 7; ++i) P[i] = { W * 0.5f + pts[i].x * cr - pts[i].y * sr, 450.0f + pts[i].x * sr + pts[i].y * cr };
                const int edges[][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 0 }, { 6, 0 }, { 6, 1 }, { 6, 2 }, { 6, 3 }, { 6, 4 }, { 6, 5 }, { 1, 3 }, { 0, 4 } };
                Path w;
                for (auto& e : edges) { w.startNewSubPath (P[e[0]]); w.lineTo (P[e[1]]); }
                g.setColour (lineCol()); g.strokePath (w, PathStrokeType (1.0f));
                Path f; f.startNewSubPath (P[6]); f.lineTo (P[2]); f.lineTo (P[3]); f.closeSubPath();
                g.setColour (F.hi.withAlpha (dark ? 0.16f : 0.55f)); g.fillPath (f);
            }
            // the orbit, the path, the head
            const Colour inkC = F.ink.interpolatedWith (F.hi, 0.5f * actMax);
            g.setColour (F.ink.withAlpha (0.3f)); g.strokePath (ghostPath, PathStrokeType (0.8f));
            g.setColour (inkC.withAlpha (0.85f)); g.strokePath (trailOld, PathStrokeType (1.0f));
            g.setColour (F.hi); g.strokePath (trailNew, PathStrokeType (1.6f));
            if (tCount == 0) return;
            const auto hp = headPos;
            // the sends: head -> each cutoff, marching toward it; a chip rides it where it is pushing
            for (int s = 0; s < 2; ++s)
            {
                if (cut[s] == nullptr) continue;
                const auto b = Links::centreOf (cut[s]);
                const auto d = b - hp;
                const float len = d.getDistanceFromOrigin();
                if (len < 60.0f) continue;
                const auto u = d / len;
                const auto s0 = hp + u * 12.0f, s1 = b - u * 38.0f;
                const float segLen = (s1 - s0).getDistanceFromOrigin();
                const float amt = std::abs (send[s]);
                const float a = (0.25f + 0.6f * depth) * (0.5f + 0.5f * amt);
                Path dp;
                for (float q = std::fmod (march, 9.0f); q < segLen; q += 9.0f)
                    dp.addLineSegment ({ s0 + u * q, s0 + u * jmin (segLen, q + 4.0f) }, 1.0f);
                g.setColour (cellOf (s).withAlpha (a)); g.fillPath (dp);
                const auto bead = s0 + u * (segLen * (0.5f + 0.45f * send[s]));
                const auto bp = para (bead, 8.0f, 5.0f);
                g.setColour (cellOf (s).withAlpha (0.4f + 0.6f * depth)); g.fillPath (bp);
                g.setColour (F.hi.withAlpha (0.4f + 0.6f * depth)); g.strokePath (bp, PathStrokeType (1.0f));
            }
            const auto head = para (hp, 10.0f, 6.0f);
            g.setColour (F.hi); g.fillPath (head);
            g.setColour (F.ink); g.strokePath (head, PathStrokeType (1.0f));
        }
    };

    // The poly flock round the poly knob: each boid a small chip in its side's colour with a thin
    // tail, neighbours flying close joined by a faint web, and a dashed send marching from the
    // flock to the poly knob. It all brightens with the flock (chi) amount.
    struct FlockView : public Component
    {
        ChloaelisProcessor& proc;
        static constexpr int n = chl::dsp::Flock::n, hist = 16;
        Point<float> trail[n][hist];
        int head = 0, count = 0;
        float amt = 0.0f, march = 0.0f;
        Component* poly = nullptr;
        static constexpr float rx = 300.0f, ry = 545.0f, rw = 600.0f, rh = 270.0f;

        explicit FlockView (ChloaelisProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
        static Point<float> toScreen (float x, float y) { return { rx + rw * 0.5f + x * rw * 0.45f, ry + rh * 0.5f + y * rh * 0.42f }; }
        Point<float> headOf (int i) const { return trail[i][(head + hist - 1) % hist]; }

        void advance (float dt)
        {
            amt += (proc.apvts.getRawParameterValue (chl::flockId)->load() - amt) * 0.1f;
            for (int i = 0; i < n; ++i)
                trail[i][head] = toScreen (proc.flockX[(size_t) i].load (std::memory_order_relaxed), proc.flockY[(size_t) i].load (std::memory_order_relaxed));
            head = (head + 1) % hist;
            count = jmin (count + 1, hist);
            march += dt * (8.0f + 30.0f * amt);
        }
        void paint (Graphics& g) override
        {
            if (count == 0) return;
            g.setOpacity (1.0f);
            const float bright = 0.4f + 0.6f * amt;
            // the web: boids flying close together
            Path web;
            Point<float> centroid;
            for (int i = 0; i < n; ++i)
            {
                const auto a = headOf (i);
                centroid += a / (float) n;
                for (int j = i + 1; j < n; ++j)
                {
                    const auto b = headOf (j);
                    if (a.getDistanceFrom (b) < 80.0f) web.addLineSegment ({ a, b }, 0.7f);
                }
            }
            g.setColour (F.ink.withAlpha (0.25f * bright)); g.fillPath (web);
            // tails and heads
            for (int i = 0; i < n; ++i)
            {
                Path tail;
                for (int k = 0; k < count; ++k)
                {
                    const auto p = trail[i][(head + hist - count + k) % hist];
                    if (k == 0) tail.startNewSubPath (p); else tail.lineTo (p);
                }
                g.setColour (F.ink.withAlpha (0.4f * bright)); g.strokePath (tail, PathStrokeType (1.0f));
                const auto p = headOf (i);
                const auto c = para (p, 9.0f, 5.0f);
                g.setColour (cellOf (i < n / 2 ? 0 : 1).withAlpha (0.6f + 0.4f * amt)); g.fillPath (c);
                g.setColour (F.hi.withAlpha (0.5f + 0.5f * amt)); g.strokePath (c, PathStrokeType (1.0f));
            }
            // the send: flock -> poly, marching, brighter with the amount
            if (poly != nullptr)
            {
                const auto b = Links::centreOf (poly);
                const auto d = b - centroid;
                const float len = d.getDistanceFromOrigin();
                if (len > 50.0f)
                {
                    const auto u = d / len;
                    const auto s0 = centroid + u * 10.0f, s1 = b - u * 38.0f;
                    const float segLen = (s1 - s0).getDistanceFromOrigin();
                    Path dp;
                    for (float q = std::fmod (march, 9.0f); q < segLen; q += 9.0f)
                        dp.addLineSegment ({ s0 + u * q, s0 + u * jmin (segLen, q + 4.0f) }, 1.0f);
                    g.setColour (F.ink.withAlpha (0.2f + 0.5f * amt)); g.fillPath (dp);
                }
            }
        }
    };

    // the sleeve's type, top left: the name, a line about it, and a mono line with the version
    // and the output meters
    struct TitleBlock : public Component
    {
        String name, sub, code;
        float namePx = 26.0f;
        float db[2] { -90.0f, -90.0f };
        String lastMeter;
        TitleBlock() { setInterceptsMouseClicks (false, false); }
        int lit (int c) const { return roundToInt (jlimit (0.0f, 1.0f, (db[c] + 54.0f) / 48.0f) * 7.0f); }
        String meterLine() const { return String (jmax (-60.0f, jmax (db[0], db[1])), 1) + " db " + String (lit (0)) + "/" + String (lit (1)); }
        void poll() { const auto m = meterLine(); if (m != lastMeter) { lastMeter = m; repaint(); } }
        void paint (Graphics& g) override
        {
            txtLeft (g, name, { 40.0f, 44.0f }, sans (namePx, true), F.ink);
            txtLeft (g, sub, { 40.0f, 70.0f }, sans (12.0f), dimCol());
            txtLeft (g, code, { 40.0f, 88.0f }, mono (10.0f), dimCol());
            // the meters: the peak in dB, then seven chips a side, lit up to the level
            txtLeft (g, String (jmax (-60.0f, jmax (db[0], db[1])), 1) + " db", { 40.0f, 102.0f }, mono (10.0f), dimCol());
            for (int c = 0; c < 2; ++c)
            {
                const float x0 = c == 0 ? 104.0f : 166.0f;
                txtLeft (g, c == 0 ? "l" : "r", { x0, 102.0f }, mono (10.0f), dimCol());
                for (int i = 0; i < 7; ++i)
                {
                    const auto p = para ({ x0 + 14.0f + (float) i * 6.0f, 102.0f }, 4.0f, 7.0f);
                    if (i < lit (c)) { g.setColour (cellOf (c)); g.fillPath (p); }
                    g.setColour (i < lit (c) ? F.hi : lineCol()); g.strokePath (p, PathStrokeType (1.0f));
                }
            }
        }
    };

    // Which scale the Karplus-Strong notes are tuned to (the same list as Cloudfin). At rest it is
    // a small chip: a ladder (scala) and how many notes the scale has to the octave. With the mouse
    // on it, it widens to show the scale's name; click for the list, or scroll through it. Filled
    // while either side's Karplus switch is on.
    struct ScaleCell : public Component, public SettableTooltipClient
    {
        static constexpr float wSmall = 62.0f, wFull = 214.0f;
        StringArray names;
        int value = 0;
        bool lit = false, menuOpen = false;
        float wheel = 0.0f, cur = wSmall;
        float* nodeW = nullptr;         // the cell's node follows its width (hover zone, links)
        std::unique_ptr<ParameterAttachment> att;
        explicit ScaleCell (RangedAudioParameter& p)
        {
            for (auto& sc : fp::getScales()) names.add (sc.name);
            att = std::make_unique<ParameterAttachment> (p, [this] (float v) { value = jlimit (0, names.size() - 1, (int) std::lround (v)); repaint(); });
            att->sendInitialUpdate();
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void choose (int i) { att->setValueAsCompleteGesture ((float) jlimit (0, names.size() - 1, i)); }
        bool hitTest (int x, int) override { return std::abs ((float) x - (float) getWidth() * 0.5f) <= cur * 0.5f; }
        // once a frame: ease open under the mouse (and while its list is up), closed otherwise
        void animate()
        {
            const float target = (isMouseOver() || menuOpen) ? wFull : wSmall;
            if (std::abs (target - cur) < 0.01f) return;
            cur += (target - cur) * 0.24f;
            if (std::abs (target - cur) < 0.6f) cur = target;
            if (nodeW != nullptr) *nodeW = cur;
            repaint();
        }
        void mouseDown (const MouseEvent&) override
        {
            PopupMenu m;
            m.setLookAndFeel (&getLookAndFeel());
            for (int i = 0; i < names.size(); ++i) m.addItem (i + 1, names[i].toLowerCase(), true, i == value);
            menuOpen = true;
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMaximumNumColumns (1),
                             [safe = SafePointer<ScaleCell> (this)] (int r) { if (safe != nullptr) { safe->menuOpen = false; if (r > 0) safe->choose (r - 1); } });
        }
        void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
        {
            wheel += w.deltaY;
            if (std::abs (wheel) < 0.12f) return;
            choose (value + (wheel < 0.0f ? 1 : -1));
            wheel = 0.0f;
        }
        void paint (Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            const auto c = b.getCentre();
            const float open = (cur - wSmall) / (wFull - wSmall);
            const auto p = para (c, std::round (cur) - 10.0f, 20.0f);
            if (lit) { g.setColour (cellOf (-1)); g.fillPath (p); g.setColour (F.hi); g.strokePath (p, PathStrokeType (1.5f)); }
            else     { g.setColour (isMouseOver() ? ink (0.8f) : lineCol()); g.strokePath (p, PathStrokeType (1.0f)); }
            const auto inkC = lit ? F.hi : F.ink;
            if (open > 0.5f)
            {
                Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (Rectangle<float> (cur - 20.0f, 20.0f).withCentre (c).toNearestInt());
                txtAt (g, names[value].upToFirstOccurrenceOf (" (", false, false).toLowerCase(), c.translated (0.0f, 0.5f), mono (9.5f), inkC.withMultipliedAlpha ((open - 0.5f) * 2.0f));
                return;
            }
            // the ladder and the number of notes
            const float a = 1.0f - open * 2.0f;
            const float cx = std::round (c.x), cy = std::round (c.y);
            g.setColour (inkC.withMultipliedAlpha (a));
            const float lx = cx - 17.0f;
            g.fillRect (lx, cy - 6.0f, 1.0f, 12.0f); g.fillRect (lx + 8.0f, cy - 6.0f, 1.0f, 12.0f);
            for (float y : { -4.0f, 0.0f, 4.0f }) g.fillRect (lx, cy + y, 9.0f, 1.0f);
            txt (g, String ((int) chl::ksScale (value).degrees.size()), Rectangle<float> (26.0f, 14.0f).withCentre ({ cx + 9.0f, cy + 0.5f }), mono (10.0f), inkC.withMultipliedAlpha (a));
        }
    };

    //==========================================================================
    struct Content : public Component
    {
        ChloaelisProcessor& proc;
        Attractor holo { proc };
        FlockView flockView { proc };
        Links links;
        Callouts callouts;
        TitleBlock title;
        OwnedArray<Node> nodes;
        OwnedArray<Knob> knobs;
        OwnedArray<AudioProcessorValueTreeState::SliderAttachment> sAtt;
        OwnedArray<Cell> cells;
        OwnedArray<Seg> segs;
        OwnedArray<AudioProcessorValueTreeState::ButtonAttachment> bAtt;
        OwnedArray<FilterPlot> fplots;
        OwnedArray<TapeStrip> strips;
        OwnedArray<Txt> labels;
        std::vector<std::tuple<Txt*, RangedAudioParameter*, Knob*>> readouts;
        Cell* frzCell[2] {};
        RangedAudioParameter* timePrm[2] {};
        std::atomic<float>* ksOn[2] {};
        std::atomic<float>* ksScaleRaw = nullptr;
        std::unique_ptr<ScaleCell> scaleCell;
        Cell* modeCell = nullptr;
        int shownMode = -1;
        Node* tapeNode[2] {};
        Point<float> mouse { W * 0.5f, H * 0.5f }, forcedMouse { -1.0f, -1.0f };
        float warmth[2] {};
        int frame = 0;
        Random rng { 0xce115 };
        // (no tooltip window: the user doesn't want pop-up boxes)

        Knob* knob (Component& parent, Point<float> centre, float d, const String& id, const String& tip, int side)
        {
            auto* k = knobs.add (new Knob());
            k->side = side;
            parent.addAndMakeVisible (k);
            sAtt.add (new AudioProcessorValueTreeState::SliderAttachment (proc.apvts, id, *k));
            auto* prm = proc.apvts.getParameter (id);
            k->setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
            k->setTooltip (tip);
            k->setBounds (Rectangle<float> (d + 14, d + 14).withCentre (centre).toNearestInt());
            k->at = k->getLocalBounds().toFloat().getCentre();
            return k;
        }
        Txt* label (Component& parent, Rectangle<float> r, float px, bool monoFont, bool medium = false)
        {
            auto* l = labels.add (new Txt());
            l->px = px; l->monoFont = monoFont; l->medium = medium;
            parent.addAndMakeVisible (l);
            l->setBounds (r.toNearestInt());
            return l;
        }
        Node* node (Point<float> at, float w, float h, float angle, bool cellBg, int side)
        {
            auto* n = nodes.add (new Node (at, w, h, angle, cellBg, (float) nodes.size() * 1.37f));
            n->side = side;
            addAndMakeVisible (n);
            return n;
        }
        // a knob cell: the letter on the fold, the level, the value
        Node* knobCell (const String& id, int side, Point<float> at, float angle, const char* name, const String& tip, Knob** out = nullptr)
        {
            auto* n = node (at, 62, 104, angle, true, side);
            auto* k = knob (*n, n->c().translated (0, 7), 48.0f, id, tip, side);
            if (out != nullptr) *out = k;
            k->setTooltip (String (name) + ": " + tip);
            const String nm0 (name);
            const int style = nm0 == "Cutoff" ? 2 : (nm0 == "Depth" || nm0 == "Source" || nm0 == "Feedback" || nm0 == "Flock") ? 1 : 0;
            if (style != 0)
            {
                // the whole cell is the control: a vial or a collar
                k->setBounds (n->getLocalBounds());
                k->style = style;
                k->at = n->c().translated (0, 9);
                k->lh = 46.0f;
            }
            else k->at = k->getLocalBounds().toFloat().getCentre().translated (0, 1);
            auto* nm = label (*n, Rectangle<float> (40, 24).withCentre (n->c().translated (0, -31)), 17.0f, false, true);
            nm->set (symbolFor (name));
            auto* l = label (*n, Rectangle<float> (76, 13).withCentre (n->c().translated (0, 43)), 9.0f, true);
            readouts.push_back ({ l, proc.apvts.getParameter (id), k });
            return n;
        }
        Node* knobCell (int side, int param, Point<float> at, float angle, const char* name, const String& tip, Knob** out = nullptr)
        {
            return knobCell (chl::pid (side, param), side, at, angle, name, tip, out);
        }
        // an LFO cell: rate as a small stack of chips, depth as a small fill
        Node* lfoCell (int side, int dest, int rateP, int depthP, Point<float> at, float angle, const char* name)
        {
            auto* n = node (at, 112, 76, angle, true, side);
            auto* nm = label (*n, Rectangle<float> (110, 16).withCentre (n->c().translated (0, -29)), 12.0f, false, true);
            nm->set ("~ " + symbolFor (name));   // ~ = LFO, then its target
            auto* kr = knob (*n, n->c().translated (-24, 2), 30.0f, chl::pid (side, rateP), String ("triangle LFO rate (to ") + name + ")", side);
            auto* kd = knob (*n, n->c().translated (24, 2), 30.0f, chl::pid (side, depthP), String ("LFO depth: sweeps ") + name + " upwards from where its knob is", side);
            kr->mini = kd->mini = true;
            kr->setTooltip (String ("LFO rate (") + name + "): " + kr->getTooltip());
            kd->setTooltip (String ("LFO depth (") + name + "): " + kd->getTooltip());
            kd->style = 1; kd->lw = 16.0f; kd->lh = 36.0f;   // depth as a small fill
            kd->lfo = &proc.lfoValue[dest][(size_t) side];
            for (auto [dx, p] : { std::pair<float, int> { -24.0f, rateP }, { 24.0f, depthP } })
            {
                auto* l = label (*n, Rectangle<float> (52, 11).withCentre (n->c().translated (dx, 30)), 9.0f, true);
                readouts.push_back ({ l, proc.apvts.getParameter (chl::pid (side, p)), p == rateP ? kr : kd });
            }
            return n;
        }
        Node* buttonCell (Cell* cl, Point<float> at, float w, const String& id, const String& tip, int side)
        {
            const bool sub = cl->subOn.isNotEmpty();
            auto* n = node (at, w, 32, 0.0f, false, side);
            cells.add (cl);
            n->place (cl, { 0.0f, sub ? 7.0f : 0.0f }, w, sub ? 46.0f : 32.0f);
            bAtt.add (new AudioProcessorValueTreeState::ButtonAttachment (proc.apvts, id, *cl));
            cl->setTooltip (tip);
            return n;
        }
        Node* segCell (const String& id, StringArray opts, Point<float> at, float w, const String& tip, int side)
        {
            auto* n = node (at, w, 32, 0.0f, false, side);
            auto* sg = segs.add (new Seg (*proc.apvts.getParameter (id), std::move (opts), side));
            n->place (sg, {}, w, 32);
            sg->setTooltip (tip);
            lastSeg = sg;
            return n;
        }
        Seg* lastSeg = nullptr;
        std::function<void()> onFullscreen;
        HelpOverlay help;
        VesselView* vessel = nullptr;
        std::unique_ptr<VesselView> vesselOwner;
        void link (Component* a, Component* b, int side) { links.links.push_back ({ a, b, side }); }

        explicit Content (ChloaelisProcessor& p) : proc (p)
        {
            const float silent[2] { -90.0f, -90.0f }, cold[2] {};
            updateField (cold, silent);
            setSize ((int) W, (int) H);
            addAndMakeVisible (holo); holo.setBounds (getLocalBounds());
            addAndMakeVisible (flockView); flockView.setBounds (getLocalBounds());
            addAndMakeVisible (links); links.setBounds (getLocalBounds());
            addAndMakeVisible (title); title.setBounds (0, 0, 460, 120);
            title.name = "chloaelis";
            title.sub = String::fromUTF8 ("dual filter \xc2\xb7 tube \xc2\xb7 delay, with a weather of its own");
            title.code = String::fromUTF8 ("chl " CHL_VERSION " \xc2\xb7 freakiepie");

            Node* cutNode[2] {};
            Node* inNode[2] {};
            Node* timeNode[2] {};
            Node* outNode[2] {};

            for (int s = 0; s < 2; ++s)
            {
                auto P = [s] (float x, float y) { return Point<float> (s == 0 ? x : W - x, y); };
                const float fl = s == 0 ? 1.0f : -1.0f;
                Knob* k = nullptr;

                // input
                auto* nIn   = knobCell (s, chl::inAtt, P (118, 168), -0.30f * fl, "Input", "input level");
                nIn->title = "input";
                if (s == 0) { nIn->tdx = -74.0f; nIn->tdy = 30.0f; }    // (to its left: clear of the title block)
                inNode[s] = nIn;

                // filter
                auto* nCut  = knobCell (s, chl::cutoff, P (232, 262), -0.2f * fl, "Cutoff", "filter cutoff (the Lorenz attractor moves it when its depth is up)", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::cutoff];
                nCut->title = "filter";
                auto* nPlot = node (P (372, 214), 150, 44, 0.0f, false, s);
                fplots.add (nPlot->place (new FilterPlot (proc, s), {}, 150, 44));
                auto* nRes  = knobCell (s, chl::res, P (306, 336), 0.3f * fl, "Reso", "filter resonance");
                auto* nMorph= knobCell (s, chl::morph, P (404, 322), -0.25f * fl, "Mode", "filter mode: low pass > band pass > high pass > notch", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::morph];
                auto* nLM   = lfoCell (s, 0, chl::lmRate, chl::lmDepth, P (414, 432), 0.12f * fl, "Mode");

                // tube
                auto* nDrv  = knobCell (s, chl::drive, P (92, 312), 0.2f * fl, "Drive", "tube drive", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::drive];
                nDrv->title = "tube";
                auto* tubeOn = new Cell ("tube on", s);
                tubeOn->offText = "tube off";
                auto* nTOn  = buttonCell (tubeOn, P (150, 400), 96, chl::tubeOnId (s), "tube distortion on / off", s);
                auto* nLD   = lfoCell (s, 2, chl::ldRate, chl::ldDepth, P (96, 492), -0.1f * fl, "Drive");
                // tube type: i 12AX7 (default), ii EL84, iii 6L6 with sag, iv starved
                auto* nTT   = segCell (chl::tubeTypeId (s), { "i", "ii", "iii", "iv" }, P (218, 446), 128,
                                       "tube type: one 12AX7, two EL84, three 6L6 with sag, four starved", s);
                link (nDrv, nTT, s);

                // delay
                auto* nTime = knobCell (s, chl::time, P (82, 634), -0.25f * fl, "Time", "delay time", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::time];
                nTime->title = "delay";
                auto* nType = segCell (chl::delayTypeId (s), { "tape", "pt2399", "bbd" }, P (252, 526), 196,
                                       "delay engine.  tape: an ageing tape loop (hiss, warble, wear; refresh for new tape).  pt2399: the chip, gritty at long times.  bbd: bucket brigade, crunchy and dark at long times", s);
                auto* nReg  = knobCell (s, chl::regen, P (188, 654), 0.2f * fl, "Regen", "delay regeneration (past ~90% it self-oscillates)");
                auto* nMix  = knobCell (s, chl::wet, P (300, 642), -0.3f * fl, "Mix", "delay dry / wet");
                auto* nSlew = segCell (chl::timeModeId (s), { "slew", "fade" }, P (416, 552), 118,
                                       "time changes.  slew: the clock / tape speed glides, so echoes bend in pitch.  fade: the new time crossfades in, no pitch change", s);
                // Karplus-Strong time: the delay becomes a string, and the time knob picks its note
                auto* ksCell = new Cell (String::fromUTF8 ("\xcf\x99"), s);     // koppa, the old Greek K: Karplus
                ksCell->px = 13.0f; ksCell->monoFont = false;
                auto* nKs   = buttonCell (ksCell, P (416, 508), 58, chl::ksId (s),
                                          "Karplus-Strong time: the delay becomes a tuned string. the time knob picks its note from the scale in the middle; regen is its sustain", s);
                link (nKs, nSlew, s);
                timePrm[s] = proc.apvts.getParameter (chl::pid (s, chl::time));
                ksOn[s] = proc.apvts.getRawParameterValue (chl::ksId (s));
                auto* nRnd  = knobCell (s, chl::rnd, P (398, 646), 0.22f * fl, "Rand", "random freeze: the chance that the buffer gets held for a while, at random moments");
                auto* nLT   = lfoCell (s, 1, chl::ltRate, chl::ltDepth, P (112, 752), 0.08f * fl, "Time");
                auto* nHpf  = buttonCell (new Cell ("hpf", s), P (238, 748), 70, chl::hpfId (s), "high pass filter in the delay feedback path", s);
                // freeze: filled while the buffer is held (by hand or by the random freeze)
                frzCell[s] = new Cell ("freeze", s);
                frzCell[s]->subOn = "buffer held"; frzCell[s]->subOff = "buffer live";
                auto* nFrz  = buttonCell (frzCell[s], P (330, 760), 84, chl::freezeId (s), "freeze the delay buffer (on tape, a held loop slowly disintegrates as the tape wears)", s);
                auto* nTape = node (P (430, 744), 104, 54, 0.0f, false, s);
                tapeNode[s] = nTape;
                auto* rf = new Cell ("refresh", s);
                rf->setClickingTogglesState (false);
                rf->onClick = [this, s] { proc.refreshTape[(size_t) s] = true; };
                rf->setTooltip ("put on a fresh tape: the wear (dropouts, dullness, hiss, warble) goes back to new");
                cells.add (rf);
                nTape->place (rf, { 0, -12 }, 104, 28);
                strips.add (nTape->place (new TapeStrip (proc, s), { 0, 14 }, 104, 30));

                // out
                auto* nOut  = knobCell (s, chl::out, P (520, 742), 0.25f * fl, "Out", "output level (the cells warm with it: the side's colour, then the highlight when hot, red at the top)");

                link (nIn, nCut, s); link (nCut, nPlot, s); link (nCut, nRes, s); link (nRes, nMorph, s); link (nMorph, nLM, s);
                link (nCut, nDrv, s); link (nDrv, nTOn, s); link (nDrv, nLD, s);
                link (nTOn, nType, s); link (nType, nTime, s); link (nTime, nReg, s); link (nTime, nLT, s);
                link (nReg, nMix, s); link (nReg, nHpf, s); link (nHpf, nFrz, s); link (nMix, nSlew, s); link (nMix, nRnd, s);
                link (nRnd, nFrz, s); link (nFrz, nTape, s); link (nMix, nOut, s);
                cutNode[s] = nCut; timeNode[s] = nTime; outNode[s] = nOut;
            }

            // the shared middle: Lorenz attractor -> both cutoffs, poly width -> both delays, order
            auto* nSp = knobCell (chl::lzSpeedId, -1, { W * 0.5f - 84, 178 }, -0.18f, "Speed", "how fast the attractor travels: slow drifts to fast flutter");
            auto* nCh = knobCell (chl::lzChaosId, -1, { W * 0.5f, 214 }, 0.1f, "Chaos", "rho: low = it barely holds a lobe and wanders gently; high = wild, with odd periodic windows");
            auto* nSh = knobCell (chl::lzShapeId, -1, { W * 0.5f + 84, 178 }, 0.2f, "Shape", "beta: reshapes the orbit (thin and twitchy vs. fat and lazy)");
            auto* nDp = knobCell (chl::lzDepthId, -1, { W * 0.5f, 352 }, -0.12f, "Depth", "how far the attractor moves the cutoffs (up to +/- 3.5 octaves)");
            auto* nOrd = segCell (chl::orderId, { String::fromUTF8 ("filter \xe2\x80\xba dly"), String::fromUTF8 ("dly \xe2\x80\xba filter") }, { W * 0.5f, 548 }, 190,
                                  "order: filter > tube > delay, or delay > tube > filter", -1);
            auto* nW  = knobCell (chl::widthId, -1, { W * 0.5f, 636 }, 0.15f, "Width", "poly: fades in up to 7 more delay taps per side, one after another, each at its own offset time. all the way up = 8 taps, a reverb-like cloud. all the way down = off");
            nW->title = "poly"; nW->tdx = 112.0f; nW->tdy = -32.0f;
            auto* nFl = knobCell (chl::flockId, -1, { W * 0.5f, 752 }, -0.1f, "Flock",
                                  "flock: a boids simulation moves each poly tap's delay time (up to ~0.9 octave). the taps drift, cluster and scatter together");
            link (nW, nFl, -1);
            flockView.poly = nW;

            nSp->title = "lorenz";

            // the built-in recording (which version, and its level) and the master feedback
            auto* nSrcSel = segCell (chl::srcSelId, { String::fromUTF8 ("\xe2\x88\x9e"), String::fromUTF8 ("\xe2\x89\x88") }, { 300, 96 }, 92,
                                     String::fromUTF8 ("built-in recording.  \xe2\x88\x9e: baserecording1, a long loop from its middle (3:36 to 7:25).  \xe2\x89\x88: the hydrophone recording (june 2026, 3), looping"), -1);
            lastSeg->px = 14.0f; lastSeg->monoFont = false;    // the symbols need to be legible

            // the column of small chips, top right: reset, (fullscreen), help, light / dark
            float ty = 36.0f;
            auto smallChip = [this] (Point<float> at, const String& text, float px, const String& tip, std::function<void()> click)
            {
                auto* n = node (at, 58, 30, 0.0f, false, -1);
                auto* c = new Cell (text, -1);
                c->px = px; c->monoFont = false;
                c->setClickingTogglesState (false);
                c->setTooltip (tip);
                c->onClick = std::move (click);
                cells.add (c);
                n->place (c, {}, 58, 28);
                return c;
            };
            // reset: every control back to its default (the tape's wear is left alone)
            smallChip ({ W - 58, ty }, String::fromUTF8 ("\xe2\x86\xba"), 13.0f, "reset everything to the defaults", [this]
            {
                for (auto* prm : proc.getParameters())
                    if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
                    {
                        rp->beginChangeGesture();
                        rp->setValueNotifyingHost (rp->getDefaultValue());
                        rp->endChangeGesture();
                    }
            });

            // desktop (standalone) only: the vessel you pour audio into, and a fullscreen toggle
            if (proc.isStandalone())
            {
                auto* nV = node ({ W - 300, 104 }, 70, 130, 0.0f, false, -1);
                vessel = new VesselView (proc);
                vesselOwner.reset (vessel);
                nV->addAndMakeVisible (vessel);
                vessel->setBounds (nV->getLocalBounds());
                vessel->body = Rectangle<float> (70, 130).withCentre (nV->c());
                nV->title = "vessel";
                link (nV, inNode[0], 0); link (nV, inNode[1], 1);

                smallChip ({ W - 58, ty += 36.0f }, String::fromUTF8 ("\xe2\xa4\xa2"), 13.0f, "fullscreen (cmd+f; esc to leave)", [this] { if (onFullscreen) onFullscreen(); });
            }

            // the scale the Karplus-Strong notes are tuned to
            {
                auto* nScale = node ({ W * 0.5f, 506 }, 214, 32, 0.0f, false, -1);
                scaleCell = std::make_unique<ScaleCell> (*proc.apvts.getParameter (chl::ksScaleId));
                nScale->place (scaleCell.get(), {}, 214, 32);
                nScale->cw = ScaleCell::wSmall;
                scaleCell->nodeW = &nScale->cw;
                scaleCell->setTooltip ("the scale the Karplus-Strong notes are tuned to: click for the list, or scroll");
                ksScaleRaw = proc.apvts.getRawParameterValue (chl::ksScaleId);
                link (nScale, nOrd, -1);
            }
            auto* nSrc = knobCell (chl::srcLevelId, -1, { 410, 116 }, -0.12f, "Source", "the built-in recording's level: fade it in and out (left into the left strip, right into the right)");
            nSrc->title = "source";
            auto* nFb = knobCell (chl::masterFbId, -1, { W - 410, 116 }, 0.12f, "Feedback", "master feedback: each side's output back into its own input. past ~90% it can sing on its own");
            nFb->title = "feedback";
            link (nSrcSel, nSrc, -1);
            link (nSrc, inNode[0], 0); link (nSrc, inNode[1], 1);
            link (nFb, inNode[0], 0); link (nFb, inNode[1], 1);
            link (nDp, cutNode[0], 0); link (nDp, cutNode[1], 1);
            holo.cut[0] = cutNode[0]; holo.cut[1] = cutNode[1];
            link (nSp, nCh, -1); link (nCh, nSh, -1); link (nCh, nDp, -1);
            link (nW, timeNode[0], 0); link (nW, timeNode[1], 1); link (nOrd, nW, -1);
            link (nW, outNode[0], 0); link (nW, outNode[1], 1);

            for (auto* n : nodes) callouts.nodes.push_back (n);
            addAndMakeVisible (callouts); callouts.setBounds (getLocalBounds());

            // the help screen (hidden until the ? cell is clicked)
            help.desktop = proc.isStandalone();
            addChildComponent (help); help.setBounds (getLocalBounds());
            smallChip ({ W - 58, ty += 36.0f }, "?", 13.0f, "help: the key to every symbol", [this] { help.setVisible (! help.isVisible()); help.toFront (false); });
            // light or dark: the mode is kept with the state; until it is chosen the editor follows the system
            modeCell = smallChip ({ W - 58, ty += 36.0f }, String::fromUTF8 ("\xe2\x98\xbe"), 13.0f, "light or dark", [this] { proc.uiDark.store (dark ? 0 : 1); });
            tick();
        }
        void paint (Graphics&) override { ++chloaelisDbgFrames; }   // called once per rendered frame

        bool wantDark() const
        {
            const int d = proc.uiDark.load();
            return d < 0 ? Desktop::getInstance().isDarkModeActive() : d == 1;
        }
        // everything that caches a picture is drawn again when the mode changes
        void modeChanged()
        {
            for (auto* n : nodes) n->repaint();
            for (auto* c : cells) c->repaint();
            for (auto* s : segs) s->repaint();
            modeCell->a = String::fromUTF8 (dark ? "\xe2\x98\x80" : "\xe2\x98\xbe");
            help.repaint();
            if (auto* top = getParentComponent()) top->repaint();
        }

        void tick()
        {
            const double t = now();
            ++frame;
            {
                const bool d = wantDark();
                if ((d ? 1 : 0) != shownMode) { dark = d; shownMode = d ? 1 : 0; modeChanged(); }
                const bool anyKs = ksOn[0]->load() > 0.5f || ksOn[1]->load() > 0.5f;
                if (anyKs != scaleCell->lit) { scaleCell->lit = anyKs; scaleCell->repaint(); }
                scaleCell->animate();
            }
            // input level -> warmth: quick to warm, slow to cool, like a meter's ballistics
            for (int s = 0; s < 2; ++s)
            {
                const float target = jlimit (0.0f, 1.0f, (levelDb (proc.inLevel[(size_t) s].load()) + 42.0f) / 36.0f);
                warmth[s] += (target - warmth[s]) * (target > warmth[s] ? 0.18f : 0.035f);
            }
            // output levels -> colour (fast up, slower down)
            for (int s = 0; s < 2; ++s)
            {
                const float d = levelDb (proc.outLevel[(size_t) s].load());
                links.db[s] = d > links.db[s] ? d : links.db[s] + (d - links.db[s]) * 0.08f;
                title.db[s] = links.db[s];
            }
            updateField (warmth, links.db);

            const bool forced = forcedMouse.x >= 0.0f;
            const bool over = forced || isMouseOver (true);
            if (forced)       mouse = forcedMouse;
            else if (over)    mouse = getMouseXYRelative().toFloat();

            // the cells float: a slow breath on each, in its own phase; the one under the cursor
            // (or in hand) eases to rest, so it doesn't slide away from the pointer
            for (auto* n : nodes)
            {
                const bool held = ! forced && (n->isMouseOverOrDragging (true) || n->isMouseButtonDown (true));
                if (held != n->hov) { n->hov = held; n->repaint(); }
                n->hold += ((held ? 1.0f : 0.0f) - n->hold) * 0.1f;
                const float k = 1.0f - n->hold;
                const float dx = k * 3.0f * (float) std::sin (t * 0.31 + n->phase);
                const float dy = k * 4.0f * (float) std::sin (t * 0.23 + n->phase * 1.7);
                const auto c = n->getBounds().toFloat().getCentre();
                n->moveTo (AffineTransform::rotation (n->angle, c.x, c.y).scaled (density, density, c.x, c.y).translated (dx, dy));
            }

            holo.advance (1.0f / 60.0f);
            flockView.advance (1.0f / 60.0f);
            flockView.repaint();

            for (auto* k : knobs) k->poll();
            if (vessel != nullptr) vessel->poll (frame);
            for (auto* f : fplots) f->update();
            for (auto* st : strips) st->update();
            title.poll();
            for (auto& [l, prm, kn] : readouts)
            {
                String full = prm->getCurrentValueAsText() + prm->getLabel(), brief = prm->getCurrentValueAsText();
                // Karplus-Strong time: the time knob reads as the note it is on
                for (int sd = 0; sd < 2; ++sd)
                    if (prm == timePrm[sd] && ksOn[sd]->load() > 0.5f)
                    {
                        const double hz = chl::ksNote (proc.paramNorm[(size_t) sd][chl::time].load(), (int) std::lround (ksScaleRaw->load())).hz;
                        brief = hz < 100.0 ? String (hz, 1) : String (roundToInt (hz));
                        full = brief + " hz";
                    }
                l->set (kn != nullptr && kn->isMouseOverOrDragging() ? full.toLowerCase() : compactValue (brief));
            }
            for (int s = 0; s < 2; ++s)
            {
                const bool fr = proc.frozen[(size_t) s].load() > 0.5f;
                if (frzCell[s]->held != fr) { frzCell[s]->held = fr; frzCell[s]->repaint(); }
                const int ty = jlimit (0, 2, (int) std::lround (proc.apvts.getRawParameterValue (chl::delayTypeId (s))->load()));
                const float ta = ty == 0 ? 1.0f : 0.45f;
                if (std::abs (tapeNode[s]->getAlpha() - ta) > 0.01f) tapeNode[s]->setAlpha (ta);
            }

            // the palette follows the level: the cells re-render a few at a time, so no frame
            // redraws everything; the field, links, attractor and flock follow it every frame
            if (! nodes.isEmpty())
                for (int i = 0; i < 5; ++i)
                    nodes[(frame * 5 + i) % nodes.size()]->repaint();
            holo.repaint();
            links.repaint();
            callouts.repaint();
            if (auto* top = getParentComponent()) top->repaint();
        }
    };
}

//==============================================================================
ChloaelisEditor::ChloaelisEditor (ChloaelisProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    lnf = std::make_unique<FlatLnF>();
    setLookAndFeel (lnf.get());
    content = std::make_unique<Content> (p);
    auto* c = static_cast<Content*> (content.get());
    addAndMakeVisible (*content);
    c->onFullscreen = [this] { toggleFullScreen(); };

    setResizable (true, true);
    if (p.isStandalone())
        setResizeLimits (designW / 2, designH / 2, 16384, 16384);     // free shape: fullscreen works, the UI letterboxes
    else
    {
        setResizeLimits (designW / 2, designH / 2, designW * 2, designH * 2);
        if (auto* cons = getConstrainer())
            cons->setFixedAspectRatio ((double) designW / (double) designH);
    }
    setSize (roundToInt ((float) designW / closeness), roundToInt ((float) designH / closeness));
    gl.setComponentPaintingEnabled (true);
    gl.setContinuousRepainting (false);
    gl.attachTo (*this);
    startTimerHz (60);
}

ChloaelisEditor::~ChloaelisEditor()
{
    stopTimer();
    if (kiosk) toggleFullScreen();
    if (auto* w = hostWindow.getComponent()) w->removeKeyListener (this);
    gl.detach();
    content.reset();
    setLookAndFeel (nullptr);
}

void ChloaelisEditor::paint (Graphics& g) { g.fillAll (F.bg); }   // the field fills the whole window, whatever its shape

void ChloaelisEditor::resized()
{
    const float s = jmin ((float) getWidth() / (float) designW, (float) getHeight() / (float) designH);
    content->setTransform (AffineTransform::scale (s).translated (((float) getWidth() - designW * s) * 0.5f, ((float) getHeight() - designH * s) * 0.5f));
}

void ChloaelisEditor::toggleFullScreen()
{
    auto* win = hostWindow.getComponent();
    if (win == nullptr) win = findParentComponentOfClass<DocumentWindow>();
    if (win == nullptr) return;
    auto& desk = Desktop::getInstance();
    auto setOptionsVisible = [win] (bool v)
    {
        for (auto* ch : win->getChildren())
            if (auto* b = dynamic_cast<Button*> (ch); b != nullptr && b->getButtonText() == "Options")
                b->setVisible (v);
    };
    if (! kiosk)
    {
        savedBounds = win->getBounds();
        savedTitleH = win->getTitleBarHeight();
        win->setTitleBarHeight (0);
        win->setResizable (false, false);
        setOptionsVisible (false);
        desk.setKioskModeComponent (win, false);     // hides the menu bar and the dock, fills the display
        kiosk = true;
    }
    else
    {
        desk.setKioskModeComponent (nullptr);
        win->setTitleBarHeight (savedTitleH);
        win->setResizable (true, false);
        setOptionsVisible (true);
        win->setBounds (savedBounds);
        kiosk = false;
    }
}

bool ChloaelisEditor::keyPressed (const KeyPress& key, Component*)
{
    if (key == KeyPress ('f', ModifierKeys::commandModifier, 0)) { toggleFullScreen(); return true; }
    if (key == KeyPress::escapeKey)
    {
        auto& h = static_cast<Content*> (content.get())->help;
        if (h.isVisible()) { h.setVisible (false); return true; }
        if (kiosk) { toggleFullScreen(); return true; }
    }
    return false;
}

void ChloaelisEditor::advanceFrames (int frames)
{
    auto* c = static_cast<Content*> (content.get());
    for (int i = 0; i < frames; ++i)
        c->tick();
}

void ChloaelisEditor::holdMouseForTest (float x, float y)
{
    static_cast<Content*> (content.get())->forcedMouse = { x, y };
}

void ChloaelisEditor::showHelpForTest (bool show)
{
    auto& h = static_cast<Content*> (content.get())->help;
    h.setVisible (show);
    if (show) h.toFront (false);
}

void ChloaelisEditor::setDensityForTest (float d) { density = jlimit (0.5f, 2.0f, d); }

// The desktop app: JUCE's standalone wrapper mutes the audio input by default (a guard against
// feedback through the speakers) and remembers whichever devices it opened on the first run, so
// a fresh install sat there silent with no input device and the input muted. The first time the
// window exists we unmute the input and, if no input device was chosen, pick the system's default
// one; after that the audio settings are the user's.
void ChloaelisEditor::prepareStandaloneInput (DocumentWindow* w)
{
   #if JUCE_MODULE_AVAILABLE_juce_audio_plugin_client
    auto* sw = dynamic_cast<StandaloneFilterWindow*> (w);
    auto* h = sw != nullptr ? sw->getPluginHolder() : nullptr;
    if (h == nullptr || h->settings == nullptr || h->settings->getBoolValue ("chlInputReady", false)) return;
    h->getMuteInputValue() = false;
    auto setup = h->deviceManager.getAudioDeviceSetup();
    if (setup.inputDeviceName.isEmpty())
        if (auto* type = h->deviceManager.getCurrentDeviceTypeObject())
        {
            const auto names = type->getDeviceNames (true);
            if (! names.isEmpty())
            {
                setup.inputDeviceName = names[jlimit (0, names.size() - 1, type->getDefaultDeviceIndex (true))];
                setup.useDefaultInputChannels = true;
                h->deviceManager.setAudioDeviceSetup (setup, true);
            }
        }
    h->settings->setValue ("chlInputReady", true);
   #else
    ignoreUnused (w);
   #endif
}

void ChloaelisEditor::timerCallback()
{
    // standalone: once the window exists, listen for cmd+F / esc on it
    if (proc.isStandalone() && hostWindow == nullptr)
        if (auto* w = findParentComponentOfClass<DocumentWindow>())
        {
            hostWindow = w;
            w->addKeyListener (this);
            prepareStandaloneInput (w);
        }
    static_cast<Content*> (content.get())->tick();
}
