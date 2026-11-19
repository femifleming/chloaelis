local Class = require "Base.Class"
local DelayBase = require "chloaelis.DelayBase"

local BBD = Class {}
BBD:include(DelayBase)

function BBD:init(args)
  self:initDelay(args, 1, "BBD Delay", "BB")
end

return BBD
