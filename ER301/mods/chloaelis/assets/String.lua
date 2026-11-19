local app = app
local lib = require "chloaelis.libchloaelis"
local libcore = require "core.libcore"
local Class = require "Base.Class"
local Unit = require "Unit"
local Pitch = require "Unit.ViewControl.Pitch"
local OptionControl = require "Unit.ViewControl.OptionControl"
local Encoder = require "Encoder"
local CellView = require "chloaelis.CellView"
local ScaleControl = require "chloaelis.ScaleControl"
local Kit = require "chloaelis.Kit"
local ply = app.SECTION_PLY

local sustainMap = Kit.map(0, 1.15, 0.1, 0.01, 0.001, 0.0001)

local String = Class {}
String:include(Unit)

function String:init(args)
  args.title = "Scale String"
  args.mnemonic = "SS"
  Unit.init(self, args)
end

function String:onLoadGraph(channelCount)
  -- (in stereo the right channel plays the other half of the chord)
  local op = self:addObject("op1", lib.String())
  Kit.wire(self, op, channelCount)
  self.op = op

  local tune = self:addObject("tune", app.ConstantOffset())
  local tuneRange = self:addObject("tuneRange", app.MinMax())
  connect(tune, "Out", tuneRange, "In")
  connect(tune, "Out", op, "V/Oct")
  self:addMonoBranch("tune", tune, "In", tune, "Out")

  Kit.param(self, "f0", op, "Fundamental", 110)
  Kit.param(self, "sustain", op, "Sustain", 0.6)
  Kit.param(self, "poly", op, "Poly", 0.0)
  Kit.param(self, "wet", op, "Wet", 1.0)
  Kit.gate(self, "freeze", op, "Freeze", false)

  -- only here to draw the pitch circle. It is not added to the unit: an object a unit owns is
  -- processed every frame whether anything listens to it or not
  self.circle = libcore.ScaleQuantizer()
end

function String:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.StringGraphic(0, 0, w, h)
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
  controls.scale = ScaleControl {
    name = "scale",
    width = 2 * ply,
    quantizer = self.circle,
    strings = { self.op }
  }
  controls.f0 = Kit.fader(self, {
    name = "f0", button = "f0", description = "Fundamental (1/1)",
    map = Encoder.getMap("oscFreq"), units = app.unitHertz, precision = 1, default = 110
  })
  controls.sustain = Kit.fader(self, {
    name = "sustain", button = "sust", description = "Sustain", map = sustainMap, default = 0.6
  })
  controls.poly = Kit.fader(self, {
    name = "poly", button = "poly", description = "Poly Strings", default = 0.0
  })
  controls.wet = Kit.fader(self, {
    name = "wet", button = "wet", description = "Wet/Dry", default = 1.0
  })
  controls.freeze = Kit.gateControl(self, "freeze", "freeze", "Freeze")
  controls.damp = OptionControl {
    button = "damp",
    description = "Damping",
    option = op:getOption("Damping"),
    choices = { "soft", "bright", "dark" }
  }
  controls.glide = OptionControl {
    button = "note",
    description = "Note Change",
    option = op:getOption("Time Mode"),
    choices = { "glide", "fade" }
  }
  controls.hpf = OptionControl {
    button = "hpf",
    description = "Regen Highpass",
    option = op:getOption("Feedback HPF"),
    choices = { "on", "off" }
  }

  return controls, {
    expanded = { "cells", "tune", "scale", "f0", "sustain", "poly", "wet", "freeze", "damp", "glide", "hpf" },
    collapsed = {}
  }
end

return String
