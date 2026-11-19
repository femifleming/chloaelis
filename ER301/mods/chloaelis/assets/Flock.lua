local app = app
local lib = require "chloaelis.libchloaelis"
local Class = require "Base.Class"
local Unit = require "Unit"
local OptionControl = require "Unit.ViewControl.OptionControl"
local CellView = require "chloaelis.CellView"
local Kit = require "chloaelis.Kit"

local boidMap = Kit.map(1, 14, 1, 1, 1, 1, 1)
local levelMap = Kit.map(-1, 1, 0.1, 0.01, 0.001, 0.0001)

local Flock = Class {}
Flock:include(Unit)

function Flock:init(args)
  args.title = "Flock"
  args.mnemonic = "Fk"
  Unit.init(self, args)
end

function Flock:onLoadGraph(channelCount)
  -- there is one flock for the whole module: this unit follows one boid of it. The left
  -- output is where the boid is across the square, the right how far up it
  local op = self:addObject("op1", lib.Flock())
  connect(op, "Left Out", self, "Out1")
  if channelCount > 1 then
    connect(op, "Right Out", self, "Out2")
  end
  self.op = op
  Kit.param(self, "boid", op, "Boid", 1)
  Kit.param(self, "level", op, "Level", 0.5)
end

function Flock:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.FlockGraphic(0, 0, w, h)
      g:follow(op)
      return g
    end
  }
  controls.boid = Kit.fader(self, {
    name = "boid", button = "boid", description = "Boid (1 to 14)",
    map = boidMap, precision = 0, default = 1
  })
  controls.level = Kit.fader(self, {
    name = "level", button = "level", description = "Level", map = levelMap, default = 0.5
  })
  controls.axis = OptionControl {
    button = "axis",
    description = "Left Output",
    option = op:getOption("Axis"),
    choices = { "x", "y" }
  }

  return controls, { expanded = { "cells", "boid", "level", "axis" }, collapsed = {} }
end

return Flock
