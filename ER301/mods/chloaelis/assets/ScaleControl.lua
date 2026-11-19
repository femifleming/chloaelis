-- Scale chooser for the Scale String. It is the ER-301's own pitch circle (the one in Scale
-- Quantize), so it lists the built-in scales and every Scala (.scl) file in the front card's
-- "scales" folder, and sends whichever is chosen to the strings as well.
local Class = require "Base.Class"
local PitchCircle = require "core.Quantizer.PitchCircle"
local Scales = require "core.Quantizer.Scales"

local ScaleControl = Class {}
ScaleControl:include(PitchCircle)

function ScaleControl:init(args)
  self.strings = args.strings or {}
  PitchCircle.init(self, args)
  self:setClassName("chloaelis.ScaleControl")
end

function ScaleControl:loadBuiltinScale(name)
  PitchCircle.loadBuiltinScale(self, name)
  if not name then return end
  local scale = Scales.getScale(name)
  if scale and #scale > 0 then
    for _, s in ipairs(self.strings) do
      s:beginScale()
      for _, cents in ipairs(scale) do
        s:addPitch(cents)
      end
      s:endScale()
    end
  end
end

return ScaleControl
