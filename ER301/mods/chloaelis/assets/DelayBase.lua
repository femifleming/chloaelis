-- The three delays share everything but the engine and the tape's extra controls.
local app = app
local lib = require "chloaelis.libchloaelis"
local Class = require "Base.Class"
local Unit = require "Unit"
local OptionControl = require "Unit.ViewControl.OptionControl"
local CellView = require "chloaelis.CellView"
local Kit = require "chloaelis.Kit"

local timeMap = Kit.map(0.03, 1.0, 0.1, 0.01, 0.001, 0.0001)
local regenMap = Kit.map(0, 1.15, 0.1, 0.01, 0.001, 0.0001)

local DelayBase = Class {}
DelayBase:include(Unit)

-- kind: 0 PT2399, 1 BBD, 2 tape
function DelayBase:initDelay(args, kind, title, mnemonic)
  self.kind = kind
  args.title = title
  args.mnemonic = mnemonic
  Unit.init(self, args)
end

function DelayBase:onLoadGraph(channelCount)
  local op = self:addObject("op1", lib.Delay(self.kind))
  Kit.wire(self, op, channelCount)
  self.op = op

  Kit.param(self, "time", op, "Time", 0.33)
  Kit.param(self, "regen", op, "Regen", 0.4)
  Kit.param(self, "wet", op, "Wet", 0.35)
  Kit.gate(self, "freeze", op, "Freeze", false)

  if self.kind == 2 then
    Kit.param(self, "age", op, "Age", 0.0)
    Kit.gate(self, "refresh", op, "Refresh", true)
  end
end

function DelayBase:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.DelayGraphic(0, 0, w, h)
      g:follow(op)
      return g
    end
  }

  controls.time = Kit.fader(self, {
    name = "time", button = "time", description = "Delay Time",
    map = timeMap, units = app.unitSecs, precision = 3, default = 0.33
  })
  controls.regen = Kit.fader(self, {
    name = "regen", button = "regen", description = "Regeneration",
    map = regenMap, default = 0.4
  })
  controls.wet = Kit.fader(self, {
    name = "wet", button = "wet", description = "Wet/Dry", default = 0.35
  })
  controls.freeze = Kit.gateControl(self, "freeze", "freeze", "Freeze")

  controls.mode = OptionControl {
    button = "time",
    description = "Time Change",
    option = op:getOption("Time Mode"),
    choices = { "slew", "fade" }
  }
  controls.hpf = OptionControl {
    button = "hpf",
    description = "Regen Highpass",
    option = op:getOption("Feedback HPF"),
    choices = { "on", "off" }
  }

  local expanded = { "cells", "time", "regen", "wet", "freeze" }
  if self.kind == 2 then
    controls.age = Kit.fader(self, {
      name = "age", button = "age", description = "Tape Age", default = 0.0
    })
    controls.refresh = Kit.gateControl(self, "refresh", "new", "New Tape")
    controls.ageing = OptionControl {
      button = "wear",
      description = "Tape wear",
      option = op:getOption("Ageing"),
      choices = { "on", "off" }
    }
    expanded[#expanded + 1] = "age"
    expanded[#expanded + 1] = "refresh"
    expanded[#expanded + 1] = "ageing"
  end
  expanded[#expanded + 1] = "mode"
  expanded[#expanded + 1] = "hpf"

  return controls, { expanded = expanded, collapsed = {} }
end

return DelayBase
