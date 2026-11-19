local Class = require "Base.Class"
local DelayBase = require "chloaelis.DelayBase"

local PT2399 = Class {}
PT2399:include(DelayBase)

function PT2399:init(args)
  self:initDelay(args, 0, "PT2399 Delay", "PT")
end

return PT2399
