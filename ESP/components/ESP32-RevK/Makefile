# popt from mise (see mise.toml) if installed, else from the system
POPT_DIR ?= $(shell mise where conda:popt 2>/dev/null)
ifneq ($(POPT_DIR),)
POPT := -I$(POPT_DIR)/include -L$(POPT_DIR)/lib -Wl,-rpath,$(POPT_DIR)/lib
endif

all:	revk_settings idfmon

revk_settings: revk_settings.c settings.def
	gcc -O -o $@ $< -g -Wall --std=gnu99 $(POPT) -lpopt
	./revk_settings settings.def -h settings.h -c settings.c

idfmon: idfmon.c
	gcc -O -o $@ $< -g -Wall --std=gnu99
