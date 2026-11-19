PKGNAME ?= chloaelis
PKGVERSION ?= 1.1.0

include scripts/env.mk

# Header dependency tracking, so editing a .h rebuilds what includes it.
DEPFLAGS = -MMD -MP

LIBNAME = lib$(PKGNAME)
OUT_DIR = $(PROFILE)/$(ARCH)
LIB_FILE = $(OUT_DIR)/$(LIBNAME).so
PACKAGE_FILE = $(OUT_DIR)/$(PKGNAME)-$(PKGVERSION).pkg

MOD_DIR = mods/$(PKGNAME)
ASSET_DIR = $(MOD_DIR)/assets

# Engine.cpp is the only C++ source: the ER-301 classes are header-only (compiled inside the
# SWIG wrapper), the DSP is compiled here at full speed.
MOD_CPP = $(wildcard $(MOD_DIR)/*.cpp)

OBJECTS = $(addprefix $(OUT_DIR)/,$(MOD_CPP:%.cpp=%.o))

SWIG_SOURCE = $(MOD_DIR)/$(PKGNAME).cpp.swig
SWIG_WRAPPER = $(OUT_DIR)/$(MOD_DIR)/$(PKGNAME)_swig.cpp
SWIG_OBJECT = $(SWIG_WRAPPER:%.cpp=%.o)
OBJECTS += $(SWIG_OBJECT)

SWIG_HEADER_DEPS := $(call rwildcard, $(MOD_DIR), *.h)
ASSETS := $(call rwildcard, $(ASSET_DIR), *)

INCLUDES = $(MOD_DIR) mods $(SDKPATH) $(SDKPATH)/arch/$(ARCH) $(SDKPATH)/emu

SYMBOLS =

CFLAGS.common = -Wall -ffunction-sections -fdata-sections
CFLAGS.speed = -O3 -ffast-math
CFLAGS.size = -Os

CFLAGS.release = $(CFLAGS.speed) -Wno-unused
CFLAGS.testing = $(CFLAGS.speed) -DBUILDOPT_TESTING
CFLAGS.debug = -g -DBUILDOPT_TESTING

ifeq ($(ARCH),am335x)
CFLAGS.am335x = -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mabi=aapcs -Dfar= -D__DYNAMIC_REENT__
LFLAGS = -nostdlib -nodefaultlibs -r
endif

ifeq ($(ARCH),linux)
LINUX_HOST_ARCH := $(shell uname -m)
ifeq ($(LINUX_HOST_ARCH),x86_64)
  LINUX_SIMD_FLAGS = -msse4
else
  LINUX_SIMD_FLAGS =
endif
CFLAGS.linux = -Wno-deprecated-declarations $(LINUX_SIMD_FLAGS) -fPIC
LFLAGS = -shared
endif

ifeq ($(ARCH),darwin)
ifeq ($(shell uname -m),arm64)
CFLAGS.darwin = -Wno-deprecated-declarations -march=armv8.2-a -fPIC
else
CFLAGS.darwin = -Wno-deprecated-declarations -march=native -fPIC
endif
LFLAGS = -shared -Wl,-undefined,dynamic_lookup
endif

CFLAGS += $(CFLAGS.common) $(CFLAGS.$(ARCH)) $(CFLAGS.$(PROFILE))
CFLAGS += $(addprefix -I,$(INCLUDES))
CFLAGS += $(addprefix -D,$(SYMBOLS))
CFLAGS += -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare
# No auto-vectorising, anywhere: on the Cortex-A8 GCC's vectorised loads and stores carry
# alignment promises that trap on the module (the habitat project's top NEON-safety rule; see
# tools/check-package.py, which checks the result), and it keeps the emulator sounding like the
# hardware. The vector code in this package is written by hand (ChloeSimd.h).
CFLAGS += -fno-tree-vectorize

SWIGFLAGS = -lua -no-old-metatable-bindings -nomoduleglobal -small -fvirtual
SWIGFLAGS += $(addprefix -I,$(INCLUDES))

CFLAGS.swig = $(CFLAGS.common) $(CFLAGS.$(ARCH)) $(CFLAGS.size)
CFLAGS.swig += $(addprefix -I,$(INCLUDES)) -I$(SDKPATH)/libs/lua54
CFLAGS.swig += $(addprefix -D,$(SYMBOLS))
CFLAGS.swig += -include cstdint -fno-tree-vectorize

all: $(PACKAGE_FILE)

$(LIB_FILE): $(OBJECTS)
	@echo [LINK $@]
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LFLAGS)

$(PACKAGE_FILE): $(LIB_FILE) $(ASSETS)
	@echo [ZIP $@]
	@rm -f $@
	@cd $(ASSET_DIR) && zip -rq $(abspath $@) *
	@zip -jq $@ $(LIB_FILE)

$(OUT_DIR)/%.o: %.cpp
	@echo [C++ $<]
	@mkdir -p $(@D)
	@$(CPP) $(CFLAGS) $(DEPFLAGS) -std=gnu++11 -c $< -o $@

$(SWIG_WRAPPER): $(SWIG_SOURCE) $(SWIG_HEADER_DEPS)
	@echo [SWIG $<]
	@mkdir -p $(@D)
	@$(SWIG) -c++ $(SWIGFLAGS) -o $@ $<

$(SWIG_OBJECT): $(SWIG_WRAPPER) $(SWIG_HEADER_DEPS)
	@echo [C++ SWIG $<]
	@mkdir -p $(@D)
	@$(CPP) $(CFLAGS.swig) -std=gnu++11 -I$(MOD_DIR) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf $(OUT_DIR)

install: $(PACKAGE_FILE)
	mkdir -p $(HOME)/.od/front/ER-301/packages
	cp $(PACKAGE_FILE) $(HOME)/.od/front/ER-301/packages/

.PHONY: all clean install
