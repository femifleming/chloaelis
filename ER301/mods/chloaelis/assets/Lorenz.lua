local app = app
local lib = require "chloaelis.libchloaelis"
local Class = require "Base.Class"
local Unit = require "Unit"
local OptionControl = require "Unit.ViewControl.OptionControl"
local CellView = require "chloaelis.CellView"
local Kit = require "chloaelis.Kit"

local shapeMap = Kit.map(0.5, 4, 0.5, 0.1, 0.01, 0.001)

local Lorenz = Class {}
Lorenz:include(Unit)

function Lorenz:init(args)
  args.title = "Lorenz"
  args.mnemonic = "Lz"
  Unit.init(self, args)
end

function Lorenz:onLoadGraph(channelCount)
  -- one attractor: the left output takes the chosen axis, the right the next one round
  local op = self:addObject("op1", lib.Lorenz())
  connect(op, "Left Out", self, "Out1")
  if channelCount > 1 then
    connect(op, "Right Out", self, "Out2")
  end
  self.op = op
  Kit.param(self, "speed", op, "Speed", 0.35)
  Kit.param(self, "chaos", op, "Chaos", 0.3)
  Kit.param(self, "shape", op, "Shape", 8 / 3)
  Kit.param(self, "level", op, "Level", 0.5)
end

function Lorenz:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.LorenzGraphic(0, 0, w, h)
      g:follow(op)
      return g
    end
  }
  controls.speed = Kit.fader(self, {
    name = "speed", button = "speed", description = "Speed", default = 0.35
  })
  controls.chaos = Kit.fader(self, {
    name = "chaos", button = "chaos", description = "Chaos", default = 0.3
  })
  controls.shape = Kit.fader(self, {
    name = "shape", button = "shape", description = "Shape", map = shapeMap, default = 8 / 3
  })
  controls.level = Kit.fader(self, {
    name = "level", button = "level", description = "Level", default = 0.5
  })
  controls.axis = OptionControl {
    button = "axis",
    description = "Left Output",
    option = op:getOption("Axis"),
    choices = { "x", "y", "z" }
  }

  return controls, { expanded = { "cells", "speed", "chaos", "shape", "level", "axis" }, collapsed = {} }
end

return Lorenz
