local Class = require "Base.Class"
local DelayBase = require "chloaelis.DelayBase"

local TapeLoop = Class {}
TapeLoop:include(DelayBase)

function TapeLoop:init(args)
  self:initDelay(args, 2, "Tape Loop", "TL")
end

return TapeLoop
