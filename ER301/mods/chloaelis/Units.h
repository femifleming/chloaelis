#pragma once
// Chloaelis for the ER-301: the plug-in's sections as separate units.
//
// Every class here is complete in this header (no Units.cpp). A package class that derives
// from a firmware class must keep all its virtual functions inline, or its vtable is laid out
// in a way the firmware cannot use on the hardware (see the habitat graphics authoring guide).
// The DSP itself is behind the plain functions of Engine.h, compiled separately at -O3.
//
// Each object is stereo: it works the left and right channels together, so a stereo unit costs
// what a mono one does. In a mono chain only the left ports are connected.
#include <od/objects/Object.h>
#include <od/config.h>
#include "Engine.h"

namespace chloaelis
{
  //============================================================================
  // A delay in a regeneration loop. kind: 0 = PT2399 chip, 1 = bucket brigade, 2 = tape loop.
  class Delay : public od::Object
  {
  public:
    Delay(int kind)
    {
      addInput(mLeftInput);
      addInput(mRightInput);
      addInput(mFreeze);
      addInput(mRefresh);
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mTime);
      addParameter(mRegen);
      addParameter(mWet);
      addParameter(mAge);
      addParameter(mRunAge);
      addOption(mTimeMode);
      addOption(mHpf);
      addOption(mAgeing);
      mRunAge.enableSerialization(); // the wear of the tape is kept with the unit's state,
      mRunAge.deserializeWithHardSet(); // and comes back at once, not as a ramp
      static unsigned serial = 0;    // every tape gets its own wow and flakes
      mKind = kind < 0 ? 0 : (kind > 2 ? 2 : kind);
      mpEngine = eng::delayNew(mKind, (float)globalConfig.sampleRate, 0x2399u + 977u * serial++);
      mpIO = eng::delayIO(mpEngine);
      mLastRunAge = 0.0f;
    }

    virtual ~Delay()
    {
      eng::delayFree(mpEngine);
    }

    int getKind() { return mKind; }

#ifndef SWIGLUA
    virtual void process()
    {
      if (mKind == 2)
      {
        // a state that was just loaded brings its tape with it
        const float saved = mRunAge.value();
        if (saved != mLastRunAge)
          eng::delaySetRunAge(mpEngine, saved);
      }
      eng::DelayIO &io = *mpIO;
      io.inL = mLeftInput.buffer();
      io.inR = mRightInput.isConnected() ? mRightInput.buffer() : 0;
      io.freeze = mFreeze.buffer();
      io.refresh = mRefresh.buffer();
      io.outL = mLeftOutput.buffer();
      io.outR = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.time = mTime.value();
      io.regen = mRegen.value();
      io.wet = mWet.value();
      io.age = mAge.value();
      io.ageing = mAgeing.value() == 1;
      io.hpf = mHpf.value() == 1;
      io.fade = mTimeMode.value() == 2;
      eng::delayRun(mpEngine);
      if (mKind == 2)
      {
        mLastRunAge = eng::delayRunAge(mpEngine);
        mRunAge.hardSet(mLastRunAge);
      }
    }
    void view(eng::DelayView *v) { eng::delayView(mpEngine, v); }

    od::Inlet mLeftInput{"Left In"};
    od::Inlet mRightInput{"Right In"};
    od::Inlet mFreeze{"Freeze"};
    od::Inlet mRefresh{"Refresh"};
    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mTime{"Time", 0.33f};
    od::Parameter mRegen{"Regen", 0.4f};
    od::Parameter mWet{"Wet", 0.35f};
    od::Parameter mAge{"Age", 0.0f};
    od::Parameter mRunAge{"Run Age", 0.0f};
    od::Option mTimeMode{"Time Mode", 1}; // 1 slew, 2 fade
    od::Option mHpf{"Feedback HPF", 2};   // 1 on, 2 off
    od::Option mAgeing{"Ageing", 1};      // 1 on, 2 off
#endif

  private:
    eng::Delay *mpEngine;
    eng::DelayIO *mpIO;
    int mKind;
    float mLastRunAge;
  };

  //============================================================================
  // Oversampled tube stage: 12AX7, EL84, 6L6 or starved.
  class Tube : public od::Object
  {
  public:
    Tube()
    {
      addInput(mLeftInput);
      addInput(mRightInput);
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mDrive);
      addParameter(mType);
      addOption(mQuality);
      mpEngine = eng::tubeNew((float)globalConfig.sampleRate);
      mpIO = eng::tubeIO(mpEngine);
    }

    virtual ~Tube()
    {
      eng::tubeFree(mpEngine);
    }

#ifndef SWIGLUA
    virtual void process()
    {
      eng::TubeIO &io = *mpIO;
      io.inL = mLeftInput.buffer();
      io.inR = mRightInput.isConnected() ? mRightInput.buffer() : 0;
      io.outL = mLeftOutput.buffer();
      io.outR = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.drive = mDrive.value();
      io.type = (int)(mType.value() + 0.5f) - 1;
      io.oversample = mQuality.value() == 2 ? 4 : 2;
      eng::tubeRun(mpEngine);
    }
    void view(eng::TubeView *v) { eng::tubeView(mpEngine, v); }

    od::Inlet mLeftInput{"Left In"};
    od::Inlet mRightInput{"Right In"};
    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mDrive{"Drive", 0.3f};
    od::Parameter mType{"Type", 1.0f};      // 1 12AX7, 2 EL84, 3 6L6, 4 starved (a parameter, so it takes CV)
    od::Option mQuality{"Oversampling", 1}; // 1 = 2x, 2 = 4x
#endif

  private:
    eng::Tube *mpEngine;
    eng::TubeIO *mpIO;
  };

  //============================================================================
  // Multimode filter, morphing LP > BP > HP > notch.
  class Filter : public od::Object
  {
  public:
    Filter()
    {
      addInput(mLeftInput);
      addInput(mRightInput);
      addInput(mVoltPerOctave);
      addInput(mFundamental);
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mResonance);
      addParameter(mMorph);
      mpEngine = eng::filterNew((float)globalConfig.sampleRate);
      mpIO = eng::filterIO(mpEngine);
    }

    virtual ~Filter()
    {
      eng::filterFree(mpEngine);
    }

#ifndef SWIGLUA
    virtual void process()
    {
      eng::FilterIO &io = *mpIO;
      io.inL = mLeftInput.buffer();
      io.inR = mRightInput.isConnected() ? mRightInput.buffer() : 0;
      io.voct = mVoltPerOctave.buffer();
      io.f0 = mFundamental.buffer();
      io.outL = mLeftOutput.buffer();
      io.outR = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.res = mResonance.value();
      io.morph = mMorph.value();
      eng::filterRun(mpEngine);
    }
    void view(eng::FilterView *v) { eng::filterView(mpEngine, v); }

    od::Inlet mLeftInput{"Left In"};
    od::Inlet mRightInput{"Right In"};
    od::Inlet mVoltPerOctave{"V/Oct"};
    od::Inlet mFundamental{"Fundamental"};
    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mResonance{"Resonance", 0.15f};
    od::Parameter mMorph{"Morph", 0.0f};
#endif

  private:
    eng::Filter *mpEngine;
    eng::FilterIO *mpIO;
  };

  //============================================================================
  // Lorenz attractor as a control source.
  class Lorenz : public od::Object
  {
  public:
    Lorenz()
    {
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mSpeed);
      addParameter(mChaos);
      addParameter(mShape);
      addParameter(mLevel);
      addOption(mAxis);
      mpEngine = eng::lorenzNew((float)globalConfig.sampleRate);
      mpIO = eng::lorenzIO(mpEngine);
    }

    virtual ~Lorenz()
    {
      eng::lorenzFree(mpEngine);
    }

#ifndef SWIGLUA
    virtual void process()
    {
      eng::LorenzIO &io = *mpIO;
      io.out1 = mLeftOutput.buffer();
      io.out2 = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.speed = mSpeed.value();
      io.chaos = mChaos.value();
      io.shape = mShape.value();
      io.level = mLevel.value();
      io.axis = mAxis.value() - 1;
      eng::lorenzRun(mpEngine);
    }
    void view(eng::LorenzView *v) { eng::lorenzView(mpEngine, v); }

    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mSpeed{"Speed", 0.35f};
    od::Parameter mChaos{"Chaos", 0.3f};
    od::Parameter mShape{"Shape", 2.6666667f};
    od::Parameter mLevel{"Level", 0.5f};
    od::Option mAxis{"Axis", 1}; // 1 x, 2 y, 3 z
#endif

  private:
    eng::Lorenz *mpEngine;
    eng::LorenzIO *mpIO;
  };

  //============================================================================
  // One boid of the flock, as a control source. There is one flock for all the Flock units.
  class Flock : public od::Object
  {
  public:
    Flock()
    {
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mBoid);
      addParameter(mLevel);
      addOption(mAxis);
      mpEngine = eng::flockNew((float)globalConfig.sampleRate);
      mpIO = eng::flockIO(mpEngine);
    }

    virtual ~Flock()
    {
      eng::flockFree(mpEngine);
    }

#ifndef SWIGLUA
    virtual void process()
    {
      eng::FlockIO &io = *mpIO;
      io.out1 = mLeftOutput.buffer();
      io.out2 = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.boid = (int)(mBoid.value() + 0.5f) - 1;
      io.level = mLevel.value();
      io.axis = mAxis.value() - 1;
      eng::flockRun(mpEngine);
    }
    void view(eng::FlockView *v) { eng::flockView(mpEngine, v); }

    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mBoid{"Boid", 1.0f}; // 1..14 (a parameter, so it takes CV)
    od::Parameter mLevel{"Level", 0.5f};
    od::Option mAxis{"Axis", 1}; // 1 x, 2 y: what the left output follows (the right follows the other)
#endif

  private:
    eng::Flock *mpEngine;
    eng::FlockIO *mpIO;
  };

  //============================================================================
  // Karplus-Strong string, its pitch snapped to a scale; the poly strings play other
  // degrees of the same scale (different ones on the left and on the right).
  class String : public od::Object
  {
  public:
    String()
    {
      addInput(mLeftInput);
      addInput(mRightInput);
      addInput(mVoltPerOctave);
      addInput(mFreeze);
      addOutput(mLeftOutput);
      addOutput(mRightOutput);
      addParameter(mFundamental);
      addParameter(mSustain);
      addParameter(mPoly);
      addParameter(mWet);
      addOption(mDamp);
      addOption(mTimeMode);
      addOption(mHpf);
      mpEngine = eng::stringNew((float)globalConfig.sampleRate);
      mpIO = eng::stringIO(mpEngine);
    }

    virtual ~String()
    {
      eng::stringFree(mpEngine);
    }

    // The scale, in cents above 1/1, the way a Scala file lists it (the last and largest
    // pitch is the period). Called from the UI.
    void beginScale() { eng::stringBeginScale(mpEngine); }
    void addPitch(float cents) { eng::stringAddPitch(mpEngine, cents); }
    void endScale() { eng::stringEndScale(mpEngine); }

#ifndef SWIGLUA
    virtual void process()
    {
      eng::StringIO &io = *mpIO;
      io.inL = mLeftInput.buffer();
      io.inR = mRightInput.isConnected() ? mRightInput.buffer() : 0;
      io.voct = mVoltPerOctave.buffer();
      io.freeze = mFreeze.buffer();
      io.outL = mLeftOutput.buffer();
      io.outR = mRightOutput.isConnected() ? mRightOutput.buffer() : 0;
      io.n = FRAMELENGTH;
      io.f0 = mFundamental.value();
      io.sustain = mSustain.value();
      io.poly = mPoly.value();
      io.wet = mWet.value();
      io.damp = mDamp.value() - 1;
      io.hpf = mHpf.value() == 1;
      io.fade = mTimeMode.value() == 2;
      eng::stringRun(mpEngine);
    }
    void view(eng::StringView *v) { eng::stringView(mpEngine, v); }

    od::Inlet mLeftInput{"Left In"};
    od::Inlet mRightInput{"Right In"};
    od::Inlet mVoltPerOctave{"V/Oct"};
    od::Inlet mFreeze{"Freeze"};
    od::Outlet mLeftOutput{"Left Out"};
    od::Outlet mRightOutput{"Right Out"};
    od::Parameter mFundamental{"Fundamental", 110.0f};
    od::Parameter mSustain{"Sustain", 0.6f};
    od::Parameter mPoly{"Poly", 0.0f};
    od::Parameter mWet{"Wet", 1.0f};
    od::Option mDamp{"Damping", 1};   // 1 soft, 2 bright, 3 dark
    od::Option mTimeMode{"Time Mode", 1};
    od::Option mHpf{"Feedback HPF", 2};
#endif

  private:
    eng::String *mpEngine;
    eng::StringIO *mpIO;
  };

} // namespace chloaelis
