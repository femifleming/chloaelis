# Chloaelis for the ER-301. See README.md.
# (the Aeria screensaver package is built too when its sources are here)
PROJECTS = chloaelis $(if $(wildcard mods/aeria/mod.mk),aeria)

all: $(PROJECTS)

$(PROJECTS):
	+$(MAKE) -f mods/$@/mod.mk PKGNAME=$@

$(addsuffix -clean,$(PROJECTS)):
	$(eval PROJECT := $(@:-clean=))
	+$(MAKE) -f mods/$(PROJECT)/mod.mk clean PKGNAME=$(PROJECT)

$(addsuffix -install,$(PROJECTS)):
	$(eval PROJECT := $(@:-install=))
	+$(MAKE) -f mods/$(PROJECT)/mod.mk install PKGNAME=$(PROJECT)

# after `make ARCH=am335x`: the stack, alignment and import checks on what was built for the module
check:
	tools/check-package.py testing/am335x chloaelis

test:
	@mkdir -p build
	c++ -std=c++11 -O2 -Wall -Wno-unused-function -I mods/chloaelis -I tests/ref tests/test.cpp -o build/chloe_test
	build/chloe_test

clean: $(addsuffix -clean,$(PROJECTS))

.PHONY: all clean test check $(PROJECTS)
