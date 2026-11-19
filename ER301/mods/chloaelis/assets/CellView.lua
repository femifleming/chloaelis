-- The living picture at the left of every Chloaelis unit: a network of cells driven by the
-- sound. While it has the cursor the same picture fills the small screen too.
local app = app
local Class = require "Base.Class"
local ViewControl = require "Unit.ViewControl"
local ply = app.SECTION_PLY

local CellView = Class {
  type = "CellView",
  canEdit = false,
  canMove = true
}
CellView:include(ViewControl)

function CellView:init(args)
  local name = args.name or "cells"
  local width = args.width or (2 * ply)
  -- args.make(width, height) returns a new graphic already following the DSP object
  local make = args.make or app.logError("%s.init: make is missing.", self)

  ViewControl.init(self, name)
  self:setClassName("chloaelis.CellView")

  local graphic = app.Graphic(0, 0, width, 64)
  self.pDisplay = make(width, 64)
  graphic:addChild(self.pDisplay)
  self:setMainCursorController(self.pDisplay)
  self:setControlGraphic(graphic)

  for i = 1, (width // ply) do
    self:addSpotDescriptor {
      center = (i - 0.5) * ply
    }
  end

  self.subGraphic = app.Graphic(0, 0, 128, 64)
  self.pSubDisplay = make(128, 64)
  self.subGraphic:addChild(self.pSubDisplay)
end

return CellView
