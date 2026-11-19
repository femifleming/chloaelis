%module chloaelis_libchloaelis
%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include "Units.h"
#include "Graphics.h"
#define SWIGLUA
%}

%include "Units.h"
%include "Graphics.h"
