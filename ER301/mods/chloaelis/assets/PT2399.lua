local Class = require "Base.Class"
-- Modules are required relative to this file, so the same files work in the Chloaelis package
-- ("chloaelis.X") and inside sürialeuis 2 ("surialeuis.chloaelis.X", with sürialeuis's library).
local here = (...):match("^(.*)%.")
local DelayBase = require(here .. ".DelayBase")

local PT2399 = Class {}
PT2399:include(DelayBase)

function PT2399:init(args)
  self:initDelay(args, 0, "PT2399 Delay", "PT")
end

return PT2399
