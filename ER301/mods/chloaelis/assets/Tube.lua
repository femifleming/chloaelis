local app = app
-- Modules are required relative to this file, so the same files work in the Chloaelis package
-- ("chloaelis.X") and inside sürialeuis 2 ("surialeuis.chloaelis.X", with sürialeuis's library).
local here = (...):match("^(.*)%.")
local pkg = here:match("^[^.]+")
local lib = require(pkg .. ".lib" .. pkg)
lib = lib.chloaelis or lib.suri_chloaelis or lib -- the classes sit in their C++ namespace (SWIG nspace)
local Class = require "Base.Class"
local Unit = require "Unit"
local OptionControl = require "Unit.ViewControl.OptionControl"
local CellView = require(here .. ".CellView")
local Kit = require(here .. ".Kit")

local typeMap = Kit.map(1, 4, 1, 1, 1, 1, 1)

local Tube = Class {}
Tube:include(Unit)

function Tube:init(args)
  args.title = "Tube"
  args.mnemonic = "Tu"
  Unit.init(self, args)
end

function Tube:onLoadGraph(channelCount)
  local op = self:addObject("op1", lib.Tube())
  Kit.wire(self, op, channelCount)
  self.op = op
  Kit.param(self, "drive", op, "Drive", 0.3)
  Kit.param(self, "valve", op, "Type", 1)
end

function Tube:onLoadViews(objects, branches)
  local op = self.op
  local controls = {}

  controls.cells = CellView {
    make = function(w, h)
      local g = lib.TubeGraphic(0, 0, w, h)
      g:follow(op)
      return g
    end
  }
  controls.drive = Kit.fader(self, {
    name = "drive", button = "drive", description = "Drive", default = 0.3
  })
  controls.valve = Kit.fader(self, {
    name = "valve", button = "valve", description = "1 12AX7 2 EL84 3 6L6 4 Starved",
    map = typeMap, precision = 0, default = 1
  })
  controls.quality = OptionControl {
    button = "over",
    description = "Over sample",
    option = op:getOption("Oversampling"),
    choices = { "2x", "4x" }
  }

  return controls, { expanded = { "cells", "drive", "valve", "quality" }, collapsed = {} }
end

return Tube
