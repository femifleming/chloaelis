#!/usr/bin/env python3
"""Builds the PDF manual from the emulator screenshots (build/shots, made by
tools/emu-test.sh). Needs Pillow and Google Chrome.   python3 docs/make_manual.py"""
import os, subprocess, html
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMG = os.path.join(HERE, "img")
os.makedirs(IMG, exist_ok=True)

def oled(src, dst, scale=3):
    """emulator screenshot (dark on light) -> the look of the module's screens (light on dark)"""
    im = ImageOps.invert(Image.open(src).convert("L"))
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(dst)

UNITS = ["PT2399", "BBD", "TapeLoop", "Tube", "Filter", "Lorenz", "Flock", "String"]
for u in UNITS:
    oled(os.path.join(ROOT, "build/shots", u + ".png"), os.path.join(IMG, u + ".png"))
CSS = """
@page { size: A4; margin: 17mm 16mm 18mm 16mm; }
* { box-sizing: border-box; }
body { font-family: "Helvetica Neue", Helvetica, Arial, sans-serif; font-size: 10.4pt; line-height: 1.45; color: #151515; }
h1 { font-size: 30pt; letter-spacing: -0.5pt; margin: 0 0 2mm; }
h2 { font-size: 17pt; margin: 0 0 3mm; padding-top: 1mm; border-top: 1.2pt solid #151515; page-break-before: always; }
h2.same { page-break-before: auto; margin-top: 8mm; }
h3 { font-size: 11.5pt; margin: 5mm 0 1.5mm; }
p { margin: 0 0 2.4mm; }
.cover { height: 250mm; display: flex; flex-direction: column; justify-content: space-between; }
.cover .sub { font-size: 13pt; color: #444; max-width: 135mm; }
.cover img { width: 100%; image-rendering: pixelated; border-radius: 2mm; }
.small { font-size: 8.8pt; color: #555; }
img.shot { width: 118mm; image-rendering: pixelated; display: block; margin: 2mm 0 3mm; border-radius: 1.5mm; }
table { border-collapse: collapse; width: 100%; margin: 1mm 0 3mm; font-size: 9.6pt; }
th { text-align: left; font-weight: 600; border-bottom: 0.8pt solid #151515; padding: 1.2mm 2mm 1.2mm 0; }
td { border-bottom: 0.3pt solid #bbb; padding: 1.3mm 2mm 1.3mm 0; vertical-align: top; }
td:first-child { white-space: nowrap; font-weight: 600; width: 23mm; }
td.r { white-space: nowrap; width: 30mm; color: #333; }
.note { page-break-inside: avoid; border-left: 2.2pt solid #151515; padding: 1.5mm 0 1.5mm 4mm; margin: 3mm 0; background: #f3f3f3; }
code, .k { font-family: Menlo, Monaco, monospace; font-size: 9pt; background: #eee; padding: 0 1mm; border-radius: 0.8mm; }
pre { font-family: Menlo, Monaco, monospace; font-size: 8.6pt; background: #eee; padding: 2.5mm 3mm; border-radius: 1mm; white-space: pre-wrap; margin: 1mm 0 3mm; }
ul, ol { margin: 0 0 2.5mm; padding-left: 5.5mm; } li { margin-bottom: 1mm; }
.unit { page-break-inside: avoid; }
table.cpu td:first-child { white-space: normal; width: auto; font-weight: 400; } table.cpu td { white-space: nowrap; }
.toc td:first-child { width: auto; font-weight: 400; } .toc td { border: 0; padding: 0.6mm 0; }
"""

def table(rows, head=("Control", "Range", "What it does")):
    out = ["<table><tr>" + "".join(f"<th>{h}</th>" for h in head) + "</tr>"]
    for r in rows:
        out.append("<tr><td>%s</td><td class='r'>%s</td><td>%s</td></tr>" % r)
    return "\n".join(out) + "</table>"

STATUS = """<div class="note"><b>Where this stands.</b> The package is built for the module with the firmware's own
compiler, against stolmine firmware 0.7.0-stolmine.9.7.0, and passes checks for the three things that crash a module
without warning (stack use on the audio thread, alignment, imported functions). Everything in this manual has also
been run in the ER-301 emulator, and the sound engines pass their own offline tests. It has <b>not yet been run on a
real ER-301</b>, so the first install is the first hardware test, and the processor load given in section 14 is an
estimate from a model of the processor, not a reading from a module. Section 15 is the short list of things to try
first.</div>"""

# ---------------------------------------------------------------- processor load (from the benchmark)
import sys
sys.path.insert(0, os.path.join(ROOT, "bench", "tools"))
import summary
CPU_SECTION = f"""<p>These are <b>estimates</b>, not readings from a module: no ER-301 was available while the package was
made. Each unit was compiled with the module's own compiler and flags, run instruction by instruction in an emulated
Cortex-A8 with programme material at its inputs (plucked tones and noise bursts, an LFO and gates in the sub-chains),
and every instruction was charged what ARM's manual for the Cortex-A8 says it costs. The model was then checked
against eighteen published readings from real modules (stock units, single objects and four reverbs from the habitat
project) and scaled to match them on average; after that, sixteen of the eighteen are within about -18% and +21%,
which is the range given after each figure. The percentages are of one 128-sample frame at 48 kHz on the 1 GHz
processor, the same thing the module's CPU meter shows.</p>
<p>The 1.0.0 column is the first version of this package measured the same way. Version 1.1.0 was rewritten for the
processor (both channels in one vector register, the oversamplers and the bit and bucket loops vectorised, slow
coefficients worked out a few times a millisecond, tables in place of transcendental functions, no subnormal numbers,
and a Tube, Cell Filter and Scale String that do almost nothing once their input and their tails are silent).</p>
{summary.html()}
<p>The firmware reports dropouts from about 80% at 128-sample frames. A four-channel patch of fourteen units, six of them
Chloaelis, comes to about a quarter of the processor, leaving plenty for other units. The dearest settings
are the Tube at 4x with the 6L6, the PT2399 at very short times (the chip is clocked fastest there) and the Scale
String with all seven strings; all three at once, stereo, take under a third. At 96 kHz everything costs about twice as
much per second.</p>"""

# ---------------------------------------------------------------- Chloaelis manual
CH = f"""<!doctype html><html><head><meta charset="utf-8"><title>Chloaelis for the ER-301</title><style>{CSS}</style></head><body>
<div class="cover">
  <div><h1>Chloaelis<br>for the ER-301</h1>
  <p class="sub">Eight units for the Orthogonal Devices ER-301 sound computer, taken from the Chloaelis plug-in: three delays, a tube, a filter, two control sources (a chaotic one and a flock) and a string tuned to your own scales.</p></div>
  <div><img src="img/TapeLoop.png"><p class="small" style="margin-top:3mm">Tape Loop in the ER-301 emulator. Manual for package version 1.1.2, stolmine firmware. sadnoise, November 2026.</p></div>
</div>

<h2>Contents</h2>
<table class="toc">
<tr><td>1. What it is</td></tr><tr><td>2. Installing</td></tr><tr><td>3. How every unit works: cells, faders, CV</td></tr>
<tr><td>4. PT2399 Delay</td></tr><tr><td>5. BBD Delay</td></tr><tr><td>6. Tape Loop</td></tr><tr><td>7. Tube</td></tr>
<tr><td>8. Cell Filter</td></tr><tr><td>9. Lorenz</td></tr><tr><td>10. Flock</td></tr><tr><td>11. Scale String, and your own scales</td></tr>
<tr><td>12. Patches: putting Chloaelis back together</td></tr><tr><td>13. Differences from the plug-in</td></tr>
<tr><td>14. Processor load</td></tr><tr><td>15. The first time on a module</td></tr><tr><td>16. If something is wrong</td></tr>
<tr><td>17. Building from source, credits</td></tr>
</table>

<h2 class="same">1. What it is</h2>
<p>The Chloaelis plug-in is two signal paths, each <b>filter &gt; tube &gt; delay</b>, tied together by a Lorenz attractor and a flock of boids.
On the ER-301 the chain is the instrument, so here the plug-in is taken apart into its sections. Each is a unit
of its own that you place, order and modulate like any other:</p>
{table([
 ("PT2399 Delay", "effect", "The PT2399 echo chip: a one-bit delay that gets gritty, dark and noisy as the time gets longer."),
 ("BBD Delay", "effect", "A bucket brigade delay with its compander: dark, crunchy, breathing."),
 ("Tape Loop", "effect", "A tape echo that wears out as it runs: hiss, wow and flutter, dropouts, and a held loop that slowly disintegrates."),
 ("Tube", "effect", "An oversampled valve stage with four valves to choose from."),
 ("Cell Filter", "effect", "A resonant filter that moves smoothly from low pass through band pass and high pass to notch."),
 ("Lorenz", "source", "The Lorenz attractor as a slow, never-repeating control signal."),
 ("Flock", "source", "One boid of a flock, as a control signal. All the Flock units follow the same flock, so what they modulate moves together."),
 ("Scale String", "effect / voice", "A Karplus-Strong string whose pitch snaps to a scale, with up to seven more strings making a chord from the same scale. It reads Scala files from your card."),
], ("Unit", "Kind", "What it is"))}
{STATUS}

<h2>2. Installing</h2>
<ol>
<li>Copy <span class="k">chloaelis-1.1.2.pkg</span> to the <span class="k">ER-301/packages</span> folder of the front SD card.</li>
<li>On the module open the admin screen, go to the Package Manager, select <b>chloaelis</b> and install it.</li>
<li>The units appear in the unit chooser under the category <b>Chloaelis</b>.</li>
</ol>
<p>The package must be built for the firmware it runs on. This one is built against the stolmine firmware
(er-301-stolmine), release 0.7.0-stolmine.9.7.0. Rebuild it if you move to a different firmware; see section 17.
It changes nothing in the firmware and shares nothing with other packages: uninstalling it in the Package Manager
removes it completely.</p>
<p>The Scale String needs the <b>core</b> package, which every ER-301 already has installed.</p>
<p><b>1.1.2:</b> Tape Loop keeps its tape's wear when a patch is loaded while it is running.</p>
<p><b>1.1.1:</b> Chloaelis' class names no longer clash with the ER-301's own Delay unit; the stock Delay works
whatever order units are created in. (With 1.1.0, a PT2399, BBD or Tape Loop created before any of the firmware's
own units could leave the stock Delay unable to load.) Patches and presets saved with 1.1.0 load unchanged.</p>

<h2 class="same">3. How every unit works</h2>
<h3>The cells</h3>
<p>The first control of every unit is a picture: a small network of glass cells, in the style of the plug-in,
driven by the sound. A cell's nucleus glows with the level at that point. Put the cursor on the picture and it is
drawn on the small screen as well. The picture has nothing to set; it is there to be read.</p>
<h3>Faders, CV and offset</h3>
<p>Every continuous parameter is an ordinary ER-301 fader, built the way the factory units are:</p>
<ul>
<li><b>Offset</b> (bias): turn the knob with the fader selected.</li>
<li><b>CV</b>: press the fader's button again to open its sub-chain. Whatever you put there (an input jack, an
LFO, another unit) is added to the offset.</li>
<li><b>Gain</b>: on the small screen, the amount of that CV that reaches the parameter, positive or negative.</li>
</ul>
<p>Freeze and New Tape are ordinary gate controls: a sub-chain, a threshold, and a button to fire them by hand.
Pitch controls (<span class="k">V/oct</span>) are the usual ER-301 pitch control: 1 volt per octave.</p>
<h3>Switches</h3>
<p>Two- and three-way choices (time change, regen highpass, damping) are option controls: select one, then pick
with the buttons under the small screen. They are saved with the preset and do not take CV. The Tube's valve
type is a fader, so that one does.</p>
<h3>Mono and stereo</h3>
<p>Every unit works in a mono chain and in a stereo one. Inside, each unit always works on a left and a right
channel together, in the two halves of the processor's vector registers, so <b>a stereo unit costs the same
processor time as a mono one</b>. The two channels of a stereo delay are two chips on one clock, or two tracks of one
tape: the same time and the same controls, but each with its own noise, its own flutter and its own wear.</p>
<h3>Levels</h3>
<p>The units expect ordinary modular levels (about ±5 V). The saturating parts were voiced in the plug-in for a
full-scale signal, so each unit lifts the signal on the way in and lowers it again on the way out. Quieter
signals come through cleaner, hotter ones dirtier.</p>

<h2>4. PT2399 Delay</h2>
<img class="shot" src="img/PT2399.png">
<p>A model of the PT2399 echo chip: the sound is turned into a stream of single bits, written into a fixed memory
of 44,000 bits, and read out again. The delay time is set by how fast the chip is clocked. A long delay means a
slow clock, so fewer bits per second: more grit, less treble, more noise. Turning the time while it runs
re-clocks the memory, so the echoes bend in pitch.</p>
<p>The picture shows the memory as a square wave of sixteen square cells, the newest sound at the left.</p>
{table([
 ("time", "30 ms to 1 s", "Delay time. Short is clean and bright, long is dark and gritty."),
 ("regen", "0 to 1.15", "How much of the echo is fed back. Above about 1 it keeps itself going."),
 ("wet", "0 to 1", "Dry at 0, echo only at 1, equal power in between."),
 ("freeze", "gate", "While high, the chip stops writing: the memory goes round for ever, bit for bit."),
 ("time (switch)", "slew / fade", "Slew: time changes glide and bend the pitch. Fade: the new time is set up on a second chip and crossfaded in once that chip has filled, so nothing changes pitch."),
 ("hpf", "on / off", "A 320 Hz highpass in the feedback path, so long regenerations thin out instead of booming."),
])}
<p class="small">Shorter times cost more processor: the chip model does one step per bit, and a 30 ms delay is about
1.5 million bits a second. See section 14.</p>

<div class="unit"><h2 class="same">5. BBD Delay</h2>
<img class="shot" src="img/BBD.png">
<p>A bucket brigade: a compressor, a fixed anti-alias filter, 4,096 charge buckets passed along by a clock, a
sample-and-hold output, a fixed reconstruction filter and an expander. Because the filters stay put while the
clock moves, long delays alias and go dark and crunchy. The buckets clip softly and leak a little, and the
compander makes the noise floor breathe with the signal.</p>
<p>The controls are the same as the PT2399 Delay's. Frozen, the buckets go round with a very slow leak, so a held
loop fades over minutes. The picture is a sagging chain of round buckets.</p></div>

<h2>6. Tape Loop</h2>
<img class="shot" src="img/TapeLoop.png">
<p>A tape echo: record head with pre-emphasis and soft magnetic saturation, a loop of tape, a playback head with
gap loss and head bump, wow and flutter, and hiss. The tape <b>ages while it runs</b>. Over about ten minutes
flakes of oxide come off (dropouts that come round again every pass), the treble goes, the warble widens, the
hiss rises and a crackle sets in. Frozen, the held loop disintegrates a little on every pass.</p>
<p>The picture is the loop itself with the heads in the middle. Worn stretches of tape are drawn as empty husks.</p>
{table([
 ("time", "30 ms to 1 s", "Delay time."),
 ("regen", "0 to 1.15", "Feedback. The hiss is recorded with everything else, so it builds too."),
 ("wet", "0 to 1", "Dry / echo."),
 ("freeze", "gate", "Holds the loop; it decays pass by pass, fastest where the oxide is gone."),
 ("age", "0 to 1", "Wear added on top of the tape's own. With CV here the tape can be made old and young again at will."),
 ("new", "trigger", "Puts on new tape: clears the flakes and sets the tape's own age back to nothing."),
 ("wear", "on / off", "Whether the tape ages by itself as it runs. Off, only the age fader wears it."),
 ("time (switch)", "slew / fade", "Slew: the tape speed glides, so pitch bends. Fade: a second playback head is crossfaded in."),
 ("hpf", "on / off", "Highpass in the feedback path."),
])}
<p class="small">The tape's own age is kept with the unit's state: a preset or a quicksave brings back the tape as
worn as it was (the flakes themselves fall afresh).</p>

<h2>7. Tube</h2>
<img class="shot" src="img/Tube.png">
<p>A valve stage, run at two or four times the sample rate so the distortion does not fold back as aliasing. The
picture is one glass cell: its cilia stand up with the input, its nucleus glows with the output, and the lit
cell below shows which valve is in.</p>
{table([
 ("drive", "0 to 1", "0 to 42 dB of gain into the first stage. The output is level-matched, so drive changes the colour more than the loudness."),
 ("valve", "1 to 4", "<b>1</b> 12AX7: two triode stages, warm, even harmonics, and blocking distortion when hit hard. <b>2</b> EL84: a triode into push-pull pentodes, symmetric and crunchy. <b>3</b> 6L6: a push-pull pair with power-supply sag, tight at first, then it blooms and compresses. <b>4</b> Starved: a 12AX7 on a low supply, gated and sputtery; quiet signals fall into the dead zone. Changing valve crossfades over 40 ms."),
 ("over", "2x / 4x", "Oversampling. 4x is cleaner at high drive and costs about twice as much."),
])}
<p class="small">With nothing coming in, the valve goes to rest after a moment and costs almost nothing until sound
returns. The Cell Filter and the Scale String do the same once they have rung out.</p>

<div class="unit"><h2 class="same">8. Cell Filter</h2>
<img class="shot" src="img/Filter.png">
<p>A state variable filter with a saturating integrator, so it stays musical when it rings. The picture draws the
response as a membrane; a cell rides it at the cutoff, growing with resonance.</p>
{table([
 ("V/oct", "pitch", "Tracks the cutoff at 1 V per octave, at audio rate."),
 ("f0", "Hz", "Cutoff. Its sub-chain is also audio rate, for filter FM."),
 ("res", "0 to 1", "Resonance. At the top it is on the edge of oscillating."),
 ("morph", "0 to 3", "0 low pass, 1 band pass, 2 high pass, 3 notch, and everything between."),
])}</div>

<h2>9. Lorenz</h2>
<img class="shot" src="img/Lorenz.png">
<p>The Lorenz attractor as a control source: a point that circles one lobe of a butterfly for a while, then
crosses to the other, never repeating. The unit makes a signal rather than processing one, so put it in a
fader's sub-chain (a filter's f0, a delay's time) or at the start of a chain. In a stereo chain the left output
is the chosen axis and the right output is the next one, so the two sides move together but not alike.</p>
{table([
 ("speed", "0 to 1", "How fast the point travels, from one lobe change in minutes to several a second."),
 ("chaos", "0 to 1", "How wild. Low, it barely holds a lobe; higher, it throws itself across more often."),
 ("shape", "0.5 to 4", "The attractor's third constant. 2.67 is the classic butterfly."),
 ("level", "0 to 1", "Output size. 0.5 is about ±5 V."),
 ("axis", "x / y / z", "Which axis the left output carries. x and y swing both ways; z bounces."),
])}

<h2>10. Flock</h2>
<img class="shot" src="img/Flock.png">
<p>In the plug-in a flock of fourteen boids steers the times of the extra delay taps, so that they drift, gather,
scatter and regroup together instead of each wobbling on its own. Here the flock is a control source. There is
<b>one flock for the whole module</b>, and every Flock unit follows one boid of it: its left output is where that
boid is across the square, its right output how far up. The picture shows the whole flock, the followed boid
bright.</p>
{table([
 ("boid", "1 to 14", "Which boid this unit follows. It takes CV, so a sequencer can hop from boid to boid."),
 ("level", "-1 to 1", "Output size and direction. 0.5 is about ±5 V."),
 ("axis", "x / y", "What the left output follows: across, or up. The right output follows the other."),
])}
<p>To have several things move as a flock, give each its own Flock unit with a different boid: in the
<b>time</b> sub-chain of three delays in a mixer, for instance, with a little gain. The flock runs at its own
unhurried pace (a boid crosses the square in a few seconds) and is the same for every unit, whatever chain it is
in.</p>

<h2>11. Scale String</h2>
<img class="shot" src="img/String.png">
<p>A Karplus-Strong string: a very short, exactly tuned delay in a feedback loop. Feed it anything (a click, a
burst of noise, a voice, a drum loop) and it rings at its note. The note is the <b>fundamental</b> moved by
<b>V/oct</b> and then snapped to the nearest note of the chosen <b>scale</b>, so any CV plays in tune.</p>
{table([
 ("V/oct", "pitch", "Chooses the note. It is read once per block and snapped to the scale."),
 ("scale", "list", "The scale. Turn the knob to choose; the circle shows its notes."),
 ("f0", "Hz", "The pitch of the scale's 1/1. 110 Hz to start with."),
 ("sust", "0 to 1.15", "How long the string rings. Most of the travel is long rings; at the very top it sings on its own."),
 ("poly", "0 to 1", "Fades in up to seven more strings, one after another, each on another note of the scale stacked up from the one played. All the way up it is a full chord."),
 ("wet", "0 to 1", "Dry / string. It starts at 1."),
 ("freeze", "gate", "Holds the string exactly as it is, undamped."),
 ("damp", "soft / bright / dark", "How fast the high partials die."),
 ("note", "glide / fade", "Glide slides between notes; fade crossfades to the new note with no slide."),
 ("hpf", "on / off", "Highpass in the feedback path: thinner, more plucked."),
])}
<p>In a stereo chain the two channels share the chord between them: the left plays the notes 2, 4, 6 ... steps up
the scale, the right 1, 3, 5 ...</p>
<p class="small">The string keeps its own level in hand: however long it is driven and however high the sustain,
it rings up to a ceiling and no further, instead of overloading. Held (frozen) strings go round exactly as they
are.</p>
<h3>Your own scales</h3>
<p>The scale list is the ER-301's own, the same one Scale Quantize uses. It holds the built-in scales and
<b>every Scala file (.scl) in the <span class="k">ER-301/scales</span> folder of the front SD card</b>. Copy
your .scl files there; they appear in the list by file name once the card has been mounted again (or after a restart). Scales that do
not repeat at the octave work too: the last pitch of the file is taken as the period, so a Bohlen-Pierce scale
repeats at the tritave.</p>
<p class="small">The pitch circle is drawn by the firmware and only shows pitches up to an octave. A scale with a
longer period still plays correctly; its circle is just incomplete.</p>

<h2>12. Patches</h2>
<h3>Chloaelis, put back together</h3>
<p>In one stereo chain: <b>Cell Filter &gt; Tube &gt; PT2399 Delay</b> (or BBD, or Tape). Open the filter's f0
sub-chain, insert <b>Lorenz</b>, and turn the gain up. In a stereo chain the two sides then drift apart the way
the plug-in's do. For the plug-in's other order, put the delay first and the filter last.</p>
<h3>Taps that flock</h3>
<p>The plug-in's poly taps: a mixer with three or four parallel chains, a delay in each with a different time, and
in each delay's time sub-chain a <b>Flock</b> unit set to a different boid, gain around 0.1. The echoes drift
against each other and regroup, as the plug-in's do with flock turned up.</p>
<h3>Random freeze</h3>
<p>In a delay's freeze sub-chain put anything that is sometimes high: a slow clock through a random gate, or
Lorenz with the axis set to z. The buffer is caught and let go at odd moments.</p>
<h3>A tape that dies and comes back</h3>
<p>Tape Loop with wear off. A very slow rising LFO into age, and its reset pulse into new. The echo falls apart
over a few minutes, then is suddenly new.</p>
<h3>A chord from one trigger</h3>
<p>A short noise burst into Scale String, poly at 0.7, sustain 0.9, a sequencer into V/oct and a .scl file of your
own. Each step is a chord in your tuning. Send Lorenz to poly so the chord opens and closes.</p>
<h3>Strings inside a delay</h3>
<p>Scale String, then BBD Delay with regen near 1 and the highpass on. The string re-excites itself through the
delay, the buckets darken each pass.</p>

<h2 class="same">13. Differences from the plug-in</h2>
<ul>
<li>The plug-in's <b>width / poly taps on the delays</b> (seven more delays per side, smearing toward a reverb)
are not inside the delay units: on the ER-301 that is several delay units side by side in a mixer, each as cheap
as the units here are, with Flock units on their times (section 12). The Scale String keeps its seven poly
strings.</li>
<li>The built-in <b>field recordings</b>, the input and output trims and the master feedback are not included. The
ER-301's own sample players, mixer and feedback do those jobs.</li>
<li>The string was a mode of the delay; here it is its own unit, and its scale list is the ER-301's, not the
plug-in's built-in set.</li>
<li>The sound engines are the plug-in's, re-laid for the module's processor: left and right side by side, stages of
a chain side by side, slow things worked out a few times a millisecond instead of every sample. The triode's curve
is read from a table and the oversampler is a different (equally steep) design. The offline tests compare each
unit with a plain transcription of the plug-in's code.</li>
<li>The LFOs and random-freeze clocks are gone on purpose: any ER-301 signal can modulate any fader.</li>
</ul>

<h2>14. Processor load</h2>
{CPU_SECTION}

<h2>15. The first time on a module</h2>
<p>This package has not yet been run on an ER-301. When you first install it, this order finds a problem
soonest and says the most about it:</p>
<ol>
<li>Install the package, then insert <b>Lorenz</b> in an empty chain. It is the simplest unit: if it loads, the
package loads.</li>
<li>Insert <b>Cell Filter</b> after an oscillator, then <b>Tube</b>, then each delay, then <b>Scale String</b>, one
at a time, listening to each.</li>
<li>Open the admin screen's CPU load view with one unit in the chain and note the load with and without it. Compare
with the table in section 14. If a unit reads more than twice its figure there, that is worth knowing.</li>
<li>On each delay: hold <b>freeze</b>, turn <b>time</b> in both slew and fade, switch <b>hpf</b>. On the Tube: turn
<b>valve</b> through all four, switch to 4x. On the String: turn <b>poly</b> up, hold freeze, change the scale.</li>
<li>Link two channels and repeat with one unit in the stereo chain.</li>
<li>Save a quicksave with a few units in it, restart, and load it.</li>
</ol>
<p>If the module freezes or restarts when a unit is inserted or while it runs, remove the package (hold the
module's safe-boot procedure if you have to) and note which unit and which step: that is exactly the report that
is needed.</p>

<h2>16. If something is wrong</h2>
{table([
 ("No echo", "", "Is wet above 0? A frozen delay passes no new sound into the line: check the freeze gate."),
 ("Echo is very noisy", "", "That is the PT2399 at long times, or an old tape. Shorten the time, or fire new."),
 ("String is silent", "", "It only rings when something excites it. Feed it a click or noise, and raise sust."),
 ("My .scl is not listed", "", "It must be in ER-301/scales on the front card, with the extension .scl. If it will not parse, the firmware writes a .log file beside it saying why."),
 ("Unit will not load", "", "The package was built for a different firmware. Rebuild it against the firmware on your module."),
 ("Stock Delay will not load", "", "With Chloaelis 1.1.0, after a Chloaelis delay was created first. Install 1.1.1."),
], ("Symptom", "", "What to check"))}

<h2 class="same">17. Building from source, credits</h2>
<p>The source is in the <span class="k">ER301</span> folder of the Chloaelis repository. <span class="k">make test</span>
runs the offline DSP tests; <span class="k">tools/emu-test.sh</span> loads every unit in the headless emulator;
<span class="k">make check</span> looks over what was built for the module; <span class="k">make -C bench</span>
produces the processor estimates. The README there has the build steps for the emulator and for the module.</p>
<p class="small">Chloaelis and this port: sadnoise, built with Claude Code. The ER-301 and its firmware: Orthogonal
Devices (Brian Clarkson); the firmware fork and the habitat project whose build notes this port follows: stolmine.
Unit skeleton conventions after tmfset and yrn1. Half-band filter coefficients after the well-known polyphase
all-pass designs published on musicdsp.org.</p>
</body></html>"""


for name, doc in (("Chloaelis-ER301-Manual", CH),):
    hp = os.path.join(HERE, name + ".html")
    open(hp, "w").write(doc)
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    "--print-to-pdf=" + os.path.join(HERE, name + ".pdf"), "file://" + hp],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote", name + ".pdf")
