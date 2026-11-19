-- Shared helpers for the Chloaelis units.
local app = app
local GainBias = require "Unit.ViewControl.GainBias"
local Gate = require "Unit.ViewControl.Gate"

local Kit = {}

function Kit.map(min, max, superCoarse, coarse, fine, superFine, rounding)
  local m = app.LinearDialMap(min, max)
  m:setSteps(superCoarse, coarse, fine, superFine)
  if rounding then m:setRounding(rounding) end
  return m
end

Kit.unitMap = Kit.map(0, 1, 0.1, 0.01, 0.001, 0.0001)

-- Every Chloaelis DSP object is stereo (it works both channels in one pass). In a mono chain
-- only its left side is connected.
function Kit.wire(unit, op, channelCount)
  connect(unit, "In1", op, "Left In")
  connect(op, "Left Out", unit, "Out1")
  if channelCount > 1 then
    connect(unit, "In2", op, "Right In")
    connect(op, "Right Out", unit, "Out2")
  end
end

-- A parameter with the ER-301's usual modulation: a sub-chain for CV, a gain for it and an
-- offset (bias), driving parameter `param` of the DSP object `op`.
function Kit.param(unit, name, op, param, default)
  local adapter = unit:addObject(name, app.ParameterAdapter())
  adapter:hardSet("Bias", default)
  tie(op, param, adapter, "Out")
  unit:addMonoBranch(name, adapter, "In", adapter, "Out")
  return adapter
end

function Kit.fader(unit, args)
  return GainBias {
    button = args.button,
    description = args.description,
    branch = unit.branches[args.name],
    gainbias = unit.objects[args.name],
    range = unit.objects[args.name],
    biasMap = args.map or Kit.unitMap,
    biasUnits = args.units or app.unitNone,
    biasPrecision = args.precision or 2,
    initialBias = args.default
  }
end

-- A gate or trigger input with its own sub-chain, feeding inlet `inlet` of the DSP object.
function Kit.gate(unit, name, op, inlet, trigger)
  local comparator = unit:addObject(name, app.Comparator())
  if trigger then
    comparator:setTriggerOnRiseMode()
  else
    comparator:setGateMode()
  end
  connect(comparator, "Out", op, inlet)
  unit:addMonoBranch(name, comparator, "In", comparator, "Out")
  return comparator
end

function Kit.gateControl(unit, name, button, description)
  return Gate {
    button = button,
    description = description,
    branch = unit.branches[name],
    comparator = unit.objects[name]
  }
end

return Kit
