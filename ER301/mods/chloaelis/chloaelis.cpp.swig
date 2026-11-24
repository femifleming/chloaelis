%module chloaelis_libchloaelis
%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include "Units.h"
#include "Graphics.h"
#define SWIGLUA
%}

// SWIG keeps every class's Lua metatable in one table shared by all packages, under the class's
// name, and the first package to register a name wins: core has a "Delay" too, so a Chloaelis
// delay created before core's library had loaded left the stock Delay unbuildable. The classes
// are therefore registered under their C++ namespace (lib.chloaelis.Delay, metatable
// "chloaelis.Delay"; inside sürialeuis, built with -Dchloaelis=suri_chloaelis, lib.suri_chloaelis).
%feature("nspace", "1");
%include "Units.h"
%include "Graphics.h"
%feature("nspace", "0");
