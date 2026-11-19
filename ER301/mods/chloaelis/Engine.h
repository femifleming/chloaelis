#pragma once
// The boundary between the ER-301 classes (header-only, compiled inside the SWIG wrapper at
// -Os) and the DSP (Engine.cpp, compiled at -O3). Plain functions and plain structs only:
// nothing here has a virtual function, so nothing here can upset the firmware's vtables.
//
// Every engine is stereo: left and right are worked together (see ChloeSimd.h), so the right
// channel is free. For a mono unit pass null for the right input and output.

namespace chloaelis
{
  namespace eng
  {
    static const int kCells = 16;
    static const int kBoids = 14;

    // what a display needs to know, copied out on the UI thread
    struct DelayView
    {
      float cells[kCells]; // loudness along the delay, newest first
      float wear[kCells];  // tape only: oxide lost along the loop, 0..1
      float level;         // output of the line now
      float age;           // tape only
      float timeSec;
      int frozen;
    };
    struct TubeView
    {
      float in, out;
      int type;
    };
    struct FilterView
    {
      float hz, res, morph, level;
    };
    struct LorenzView
    {
      float x, y, z;         // -1..1
      float trail[2][48];    // recent x and z, newest first
      int n;
    };
    struct FlockView
    {
      float x[kBoids], y[kBoids]; // -1..1
      int boid;                   // the one this unit follows
    };
    struct StringView
    {
      float level[8]; // the main string, then the seven poly strings
      float ratio[8]; // each string's pitch against the main one
      float hz;
      int frozen;
    };

    // How a unit object talks to its engine: the engine owns a small block of "what to do this
    // frame" (buffers, frame length, the controls' values). The object fills it in and calls
    // run. (Passing fifteen arguments instead costs stack on the audio thread, which has little.)
    struct DelayIO
    {
      const float *inL, *inR, *freeze, *refresh; // inR may be null (mono)
      float *outL, *outR;                        // outR may be null
      int n;
      float time, regen, wet, age;
      bool ageing, hpf, fade;
    };
    struct Delay;
    Delay *delayNew(int kind, float sampleRate, unsigned seed); // kind: 0 PT2399, 1 BBD, 2 tape
    void delayFree(Delay *);
    DelayIO *delayIO(Delay *);
    void delayRun(Delay *);
    void delayView(Delay *, DelayView *);
    float delayRunAge(Delay *);          // tape: how worn the tape has become by running, 0..1
    void delaySetRunAge(Delay *, float); // (kept with the unit's state)

    struct TubeIO
    {
      const float *inL, *inR;
      float *outL, *outR;
      int n;
      float drive;
      int type, oversample;
    };
    struct Tube;
    Tube *tubeNew(float sampleRate);
    void tubeFree(Tube *);
    TubeIO *tubeIO(Tube *);
    void tubeRun(Tube *);
    void tubeView(Tube *, TubeView *);

    struct FilterIO
    {
      const float *inL, *inR, *voct, *f0;
      float *outL, *outR;
      int n;
      float res, morph;
    };
    struct Filter;
    Filter *filterNew(float sampleRate);
    void filterFree(Filter *);
    FilterIO *filterIO(Filter *);
    void filterRun(Filter *);
    void filterView(Filter *, FilterView *);

    struct LorenzIO
    {
      float *out1, *out2;
      int n;
      float speed, chaos, shape, level;
      int axis;
    };
    struct Lorenz;
    Lorenz *lorenzNew(float sampleRate);
    void lorenzFree(Lorenz *);
    LorenzIO *lorenzIO(Lorenz *);
    void lorenzRun(Lorenz *);
    void lorenzView(Lorenz *, LorenzView *);

    struct FlockIO
    {
      float *out1, *out2;
      int n;
      int boid;
      float level;
      int axis;
    };
    struct Flock;
    Flock *flockNew(float sampleRate);
    void flockFree(Flock *);
    FlockIO *flockIO(Flock *);
    void flockRun(Flock *);
    void flockView(Flock *, FlockView *);

    struct StringIO
    {
      const float *inL, *inR, *voct, *freeze;
      float *outL, *outR;
      int n;
      float f0, sustain, poly, wet;
      int damp;
      bool hpf, fade;
    };
    struct String;
    String *stringNew(float sampleRate);
    void stringFree(String *);
    void stringBeginScale(String *);
    void stringAddPitch(String *, float cents);
    void stringEndScale(String *);
    StringIO *stringIO(String *);
    void stringRun(String *);
    void stringView(String *, StringView *);
  }
}
