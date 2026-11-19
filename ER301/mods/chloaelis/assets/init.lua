local Class = require "Base.Class"
local Library = require "Package.Library"

local Chloaelis = Class {}
Chloaelis:include(Library)

function Chloaelis:init(args)
  Library.init(self, args)
end

return Chloaelis
