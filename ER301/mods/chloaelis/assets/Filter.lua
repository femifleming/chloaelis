local app = app
local lib = require "chloaelis.libchloaelis"
local Class = require "Base.Class"
local Unit = require "Unit"
local GainBias = require "Unit.ViewControl.GainBias"
local Pitch = require "Unit.ViewControl.Pitch"
local Encoder = require "Encoder"
local CellView = require "chloaelis.CellView"
local Kit = require "chloaelis.Kit"

local morphMap = Kit.map(0, 3, 1, 0.1, 0.01, 0.001)

local Filter = Class {}
Filter:include(Unit)

function Filter:init(args)
  args.title = "Cell Filter"
  args.mnemonic = "CF"
  Unit.init(self, args)
end

function Filter:onLoadGraph(channelCount)
  local op = self:addObject("op1", lib.Filter())
  Kit.wire(self, op, channelCount)
  self.op = op

  -- pitch and cutoff are audio-rate, wired the way the built-in filters are
  local tune = self:addObject("tune", app.ConstantOffset())
  local tuneRange = self:addObject("tuneRange", app.MinMax())
  local f0 = self:addObject("f0", app.GainBias())
  local f0Range = self:addObject("f0Range", app.MinMax())
  connect(tune, "Out", tuneRange, "In")
  connect(f0, "Out", f0Range, "In")
  connect(tune, "Out", op, "V/Oct")
  connect(f0, "Out", op, "Fundamental")
  self:addMonoBranch("tune", tune, "In", tune, "Out")
  self:addMonoBranch("f0", f0, "In", f0, "Out")

  Kit.param(self, "res", op, "Resonance", 0.15)
  Kit.param(self, "morph", op, "Morph", 0.0)
end

function Filter:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.FilterGraphic(0, 0, w, h)
      g:follow(op)
      return g
    end
  }
  controls.tune = Pitch {
    button = "V/oct",
    branch = branches.tune,
    description = "V/oct",
    offset = objects.tune,
    range = objects.tuneRange
  }
  controls.freq = GainBias {
    button = "f0",
    branch = branches.f0,
    description = "Cutoff",
    gainbias = objects.f0,
    range = objects.f0Range,
    biasMap = Encoder.getMap("filterFreq"),
    biasUnits = app.unitHertz,
    initialBias = 1760,
    gainMap = Encoder.getMap("freqGain"),
    scaling = app.octaveScaling
  }
  controls.res = Kit.fader(self, {
    name = "res", button = "res", description = "Resonance", default = 0.15
  })
  controls.morph = Kit.fader(self, {
    name = "morph", button = "morph", description = "0 LP 1 BP 2 HP 3 Notch",
    map = morphMap, default = 0.0
  })

  return controls, { expanded = { "cells", "tune", "freq", "res", "morph" }, collapsed = {} }
end

return Filter
