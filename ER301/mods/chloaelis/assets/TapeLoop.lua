local Class = require "Base.Class"
-- Modules are required relative to this file, so the same files work in the Chloaelis package
-- ("chloaelis.X") and inside sürialeuis 2 ("surialeuis.chloaelis.X", with sürialeuis's library).
local here = (...):match("^(.*)%.")
local DelayBase = require(here .. ".DelayBase")

local TapeLoop = Class {}
TapeLoop:include(DelayBase)

function TapeLoop:init(args)
  self:initDelay(args, 2, "Tape Loop", "TL")
end

return TapeLoop
