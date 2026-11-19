# Chloaelis ER-301 CPU benchmark. See README.md.
#   make            build the image and the simulator, run every scenario, write the report
#   make baseline   the same for the 1.0.0 package (its sources are expected in
#                   ../build/baseline/chloaelis, or give CHLOE_SRC=...)
#   make image tab sim   just build
# Variables: CHLOE_SRC (the chloaelis sources), CHLOE_SCN (its scenario table), OUT (results
# directory), FW (firmware tree), HABITAT (calibration sources, see fetch-calibrators.sh).

.DEFAULT_GOAL := all
ROOT := $(abspath ..)
FW ?= $(ROOT)/er-301-release
CHLOE_SRC ?= $(ROOT)/mods/chloaelis
CHLOE_SCN ?= target/scenarios_chloaelis.cpp
OUT ?= $(ROOT)/build/bench/current
HABITAT ?= $(ROOT)/build/bench/habitat
VENV ?= $(ROOT)/build/bench/venv
TI ?= $(HOME)/ti/gcc-arm-none-eabi-4_9-2015q3
JOBS ?= 4

CC := $(TI)/bin/arm-none-eabi-gcc
CXX := $(TI)/bin/arm-none-eabi-g++
PY := $(VENV)/bin/python

OBJ := $(OUT)/obj
ELF := $(OUT)/bench.elf
SIM := $(ROOT)/build/bench/a8sim

ARCHFLAGS := -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mabi=aapcs -Dfar= -D__DYNAMIC_REENT__
COMMON := -Wall -ffunction-sections -fdata-sections -g
INC := -I$(FW) -I$(FW)/arch/am335x -I$(FW)/mods -I$(FW)/libs/ne10/inc

# The firmware's own objects: its flags (scripts/env.mk, PROFILE=release, ARCH=am335x).
FWFLAGS := $(COMMON) $(ARCHFLAGS) -O3 -ftree-vectorize -ffast-math -Wno-unused \
           -Dgcc -Dam335x -Dam3359 -DevmAM335x $(INC)
# Package DSP sources: mods/chloaelis/mod.mk CFLAGS (PROFILE=testing, as dist/*.pkg was built).
PKGFLAGS := $(COMMON) $(ARCHFLAGS) -O3 -ffast-math -DBUILDOPT_TESTING \
            -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare -fno-tree-vectorize $(INC)
# Package classes compiled inside the SWIG wrapper: mod.mk CFLAGS.swig.
SWIGFLAGS := $(COMMON) $(ARCHFLAGS) -Os -include cstdint -fno-tree-vectorize $(INC)

STOCK_CPP ?= \
  mods/core/objects/pitch/ScaleQuantizer.cpp mods/core/objects/pitch/VoltPerOctave.cpp \
  mods/core/objects/oscillators/SineOscillator.cpp mods/core/objects/oscillators/SawtoothOscillator.cpp \
  mods/core/objects/oscillators/TriangleOscillator.cpp mods/core/objects/env/ADSR.cpp \
  mods/core/objects/filters/StereoLadderFilter.cpp mods/core/objects/filters/Equalizer3.cpp \
  mods/core/objects/delays/Delay.cpp mods/core/objects/delays/MicroDelay.cpp mods/core/objects/delays/DopplerDelay.cpp \
  mods/core/objects/granular/MonoGrainDelay.cpp mods/core/objects/granular/MonoGrain.cpp mods/core/objects/granular/Grain.cpp \
  mods/core/objects/freeverb/Freeverb.cpp mods/core/objects/freeverb/ReverbModel.cpp \
  mods/core/objects/Limiter.cpp mods/core/objects/Clipper.cpp mods/core/objects/SnapToZero.cpp
FW_CPP := \
  od/objects/Object.cpp od/objects/Inlet.cpp od/objects/Outlet.cpp od/objects/Parameter.cpp \
  od/objects/Option.cpp od/objects/Port.cpp od/objects/Followable.cpp od/objects/StateMachine.cpp \
  od/objects/adapters/ParameterAdapter.cpp od/objects/timing/Comparator.cpp \
  od/objects/math/GainBias.cpp od/objects/math/ConstantOffset.cpp od/objects/math/ConstantGain.cpp \
  od/objects/math/Multiply.cpp od/objects/math/Sum.cpp od/objects/math/Gain.cpp od/objects/math/Constant.cpp \
  od/objects/measurement/MinMax.cpp \
  od/objects/mixing/CrossFade.cpp od/objects/mixing/StereoCrossFade.cpp od/objects/mixing/Mixer.cpp \
  od/objects/mixing/StereoPanner.cpp od/objects/mixing/MonoPanner.cpp \
  od/extras/ReferenceCounted.cpp od/extras/LookupTables.cpp od/extras/LookupTable.cpp \
  od/extras/Profiler.cpp od/extras/BigHeap.cpp od/extras/LeastWasteAllocator.cpp od/audio/SampleFifo.cpp od/audio/Sample.cpp od/extras/Random.cpp od/extras/LinearRamp.cpp od/extras/CosineRamp.cpp \
  $(STOCK_CPP)
FW_C := hal/simd.c

EXTRA_SCN ?= target/scenarios_stock.cpp target/scenarios_config.cpp
BENCH_CPP := target/main.cpp target/runtime.cpp $(EXTRA_SCN)
CHLOE_CPP := $(wildcard $(CHLOE_SRC)/*.cpp)

# calibration units, when they have been fetched (fetch-calibrators.sh); their package compiles
# them inside its SWIG wrapper: -Os, no -ffast-math (mods/house/mod.mk CFLAGS.swig)
CALIB_OBJ := $(if $(wildcard $(HABITAT)/as-measured/KWoodRoom.h),$(OBJ)/calib_scn.o)
CALIBFLAGS := $(COMMON) $(ARCHFLAGS) -Os -include cstdint $(INC)

OBJS := $(OBJ)/start.o \
        $(FW_CPP:%.cpp=$(OBJ)/fw/%.o) $(FW_C:%.c=$(OBJ)/fw/%.o) \
        $(BENCH_CPP:target/%.cpp=$(OBJ)/bench/%.o) \
        $(patsubst $(CHLOE_SRC)/%.cpp,$(OBJ)/chloe/%.o,$(CHLOE_CPP)) \
        $(OBJ)/chloe_scn.o $(CALIB_OBJ)

$(OBJ)/calib_scn.o: target/scenarios_calib.cpp target/bench.h
	@mkdir -p $(@D)
	@echo "[calib]  scenarios_calib.cpp + habitat house atoms (their SWIG-wrapper flags)"
	@$(CXX) $(CALIBFLAGS) -I$(HABITAT) -Itarget -std=gnu++11 -Wno-unused -c $< -o $@

all: report
image: $(ELF)
tab: $(OUT)/bench.tab
sim: $(SIM)

$(OBJ)/start.o: target/start.S
	@mkdir -p $(@D)
	@$(CC) $(ARCHFLAGS) -c $< -o $@
$(OBJ)/fw/%.o: $(FW)/%.cpp
	@mkdir -p $(@D)
	@echo "[fw c++] $*.cpp"
	@$(CXX) $(FWFLAGS) -std=gnu++11 -MMD -MP -c $< -o $@
$(OBJ)/fw/%.o: $(FW)/%.c
	@mkdir -p $(@D)
	@echo "[fw c]   $*.c"
	@$(CC) $(FWFLAGS) -std=gnu11 -MMD -MP -c $< -o $@
$(OBJ)/bench/%.o: target/%.cpp target/bench.h target/bench_abi.h
	@mkdir -p $(@D)
	@echo "[bench]  $*.cpp"
	@$(CXX) $(FWFLAGS) -std=gnu++11 -MMD -MP -c $< -o $@
$(OBJ)/chloe/%.o: $(CHLOE_SRC)/%.cpp $(wildcard $(CHLOE_SRC)/*.h)
	@mkdir -p $(@D)
	@echo "[chloe]  $*.cpp (package flags)"
	@$(CXX) $(PKGFLAGS) -I$(CHLOE_SRC) -std=gnu++11 -c $< -o $@
$(OBJ)/chloe_scn.o: $(CHLOE_SCN) $(wildcard $(CHLOE_SRC)/*.h) target/bench.h
	@mkdir -p $(@D)
	@echo "[chloe]  $(CHLOE_SCN) + Units.h (SWIG-wrapper flags)"
	@$(CXX) $(SWIGFLAGS) -I$(CHLOE_SRC) -Itarget -std=gnu++11 -c $< -o $@

# Linked against the toolchain's ARM-mode hard-float libraries (the "fpu" multilib), the same
# flavour the firmware links (scripts/am335x.mk).
$(ELF): $(OBJS) target/bench.ld
	@echo "[link]   $@"
	@$(CXX) -mfloat-abi=hard -mfpu=vfp -nostartfiles -Wl,-T,target/bench.ld -Wl,--gc-sections \
	   -Wl,-Map,$(OUT)/bench.map -o $@ $(OBJS) -lstdc++ -lm -lc -lgcc

$(OUT)/bench.tab: $(ELF) sim/decode.py | $(VENV)
	@echo "[decode] $@"
	@$(PY) sim/decode.py $(ELF) $@

# The simulator: a host program linked against the Unicorn engine from the venv's wheel.
UNICORN = $(shell ls -d $(VENV)/lib/python*/site-packages/unicorn 2>/dev/null | head -1)
$(SIM): sim/a8sim.c target/bench_abi.h | $(VENV)
	@echo "[host]   $@"
	@cc -O2 -Wall -I$(UNICORN)/include sim/a8sim.c -o $@ $(UNICORN)/lib/libunicorn.2.dylib -Wl,-rpath,$(UNICORN)/lib -lm 2>/dev/null \
	  || cc -O2 -Wall -I$(UNICORN)/include sim/a8sim.c -o $@ $(UNICORN)/lib/libunicorn.so.2 -Wl,-rpath,$(UNICORN)/lib -lm

$(VENV):
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q 'unicorn>=2.1.4' capstone pyelftools

RUNFLAGS ?=
report: $(ELF) $(OUT)/bench.tab $(SIM)
	@$(PY) tools/run.py run --out $(OUT) --jobs $(JOBS) $(RUNFLAGS)

# the package as it was before the optimisation work
baseline:
	@$(MAKE) CHLOE_SRC=$(ROOT)/build/baseline/chloaelis CHLOE_SCN=target/scenarios_chloaelis_1.0.0.cpp \
	   OUT=$(ROOT)/build/bench/baseline RUNFLAGS="$(RUNFLAGS)"

compare:
	@$(PY) tools/run.py compare $(ROOT)/build/bench/baseline/results.json $(OUT)/results.json

clean:
	rm -rf $(OUT)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)

.PHONY: all image tab sim report baseline compare clean
