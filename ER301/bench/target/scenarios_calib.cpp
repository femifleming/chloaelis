// Calibration: units from stolmine's er-301-habitat "house" package whose CPU load was read
// on a real ER-301 (see ../fetch-calibrators.sh for where the figures come from). Each is one
// stereo object with a ParameterAdapter per fader, as its Lua file builds it, at its defaults.
//
// These headers are compiled inside the SWIG wrapper in that package, so this file gets the
// wrapper's flags (-Os), like the real thing.
#include "bench.h"
#include <od/objects/adapters/ParameterAdapter.h>
#include <math.h>
#include <string.h>

#include "mods/house/atoms/CreamCoat.h"
#include "mods/house/atoms/Galactic.h"
#include "mods/house/atoms/KWoodRoom.h"
#include "mods/house/atoms/WoodenBox.h"
#include "mods/house/atoms/Verbity.h"
// the versions that were measured, in a namespace of their own
#define house house_as_measured
#include "as-measured/KWoodRoom.h"
#include "as-measured/WoodenBox.h"
#include "as-measured/Verbity.h"
#undef house

using namespace bench;

struct P { const char *name; float value; };
static void reverb(Graph &g, const char *title, od::Object *op, const P *params, int n)
{
  g.unit(title);
  g.add("op", op);
  g.connect(g.in, "Out1", op, "In L");
  g.connect(g.in, "Out2", op, "In R");
  g.out(op, "Out L");
  g.out(op, "Out R");
  for (int i = 0; i < n; i++)
    fader(g, params[i].name, {op}, params[i].name, params[i].value);
}
#define CAL(name, cls, ...)                                   \
  BENCH_SCENARIO(name, "calibration")                         \
  {                                                           \
    static const P p[] = {__VA_ARGS__};                       \
    reverb(g, #cls, new cls(), p, (int)(sizeof(p) / sizeof(p[0]))); \
  }

CAL("calib/creamcoat", house::CreamCoat, {"Select", 0.5f}, {"Regen", 0.5f}, {"DeRez", 1.0f}, {"Predlay", 0.0f}, {"Wetness", 0.25f})
CAL("calib/galactic", house::Galactic, {"Replace", 0.5f}, {"Brightness", 0.5f}, {"Detune", 0.5f}, {"BigDim", 1.0f}, {"DryWet", 1.0f})
CAL("calib/kwoodroom-as-measured", house_as_measured::KWoodRoom, {"Regen", 0.5f}, {"Time", 0.5f}, {"Tone", 0.25f}, {"Reflect", 0.5f}, {"Position", 0.75f}, {"Mix", 0.5f})
CAL("calib/kwoodroom-float-retrofit", house::KWoodRoom, {"Regen", 0.5f}, {"Time", 0.5f}, {"Tone", 0.25f}, {"Reflect", 0.5f}, {"Position", 0.75f}, {"Mix", 0.5f})
CAL("calib/woodenbox-as-measured", house_as_measured::WoodenBox, {"Select", 0.5f}, {"Reso", 0.5f}, {"Mix", 0.5f})
CAL("calib/woodenbox-float-retrofit", house::WoodenBox, {"Select", 0.5f}, {"Reso", 0.5f}, {"Mix", 0.5f})
CAL("calib/verbity-as-measured", house_as_measured::Verbity, {"Bigness", 0.25f}, {"Longness", 0.0f}, {"Darkness", 0.25f}, {"Wetness", 0.25f})
CAL("calib/verbity-float-retrofit", house::Verbity, {"Bigness", 0.25f}, {"Longness", 0.0f}, {"Darkness", 0.25f}, {"Wetness", 0.25f})
