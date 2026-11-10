#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := Saadin
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

ifeq ($(wildcard /bin/csh),)
$(error	Please install /bin/csh or equivalent)
endif

all:	main/settings.h
	@echo Make: $(PROJECT_NAME)$(SUFFIX).bin
	@idf.py build
	@cp build/$(PROJECT_NAME).bin $(PROJECT_NAME)$(SUFFIX).bin
	@cp build/bootloader/bootloader.bin $(PROJECT_NAME)$(SUFFIX)-bootloader.bin
	@echo Done: $(PROJECT_NAME)$(SUFFIX).bin

main/settings.h:     components/ESP32-RevK/revk_settings main/settings.def components/ESP32-RevK/settings.def
	components/ESP32-RevK/revk_settings $^

components/ESP32-RevK/revk_settings: components/ESP32-RevK/revk_settings.c
	make -C components/ESP32-RevK revk_settings

components/ESP32-RevK/idfmon: components/ESP32-RevK/idfmon.c
	make -C components/ESP32-RevK idfmon

# idf.py keeps the values in an existing sdkconfig and only takes new options from sdkconfig.defaults, so start again
# from the defaults whenever they change. The build directory goes too, as it may be set up for another target.
sdkconfig: sdkconfig.defaults
	rm -rf sdkconfig build

c3 c5 c6 s3 s3eth s3n8 pico wroom1 wroom solo: sdkconfig

# set:    main/settings.h c3 c5 c6 wroom1 wroom solo pico s3n8 s3
set:    main/settings.h pico s3eth s3

c3:
	components/ESP32-RevK/setbuildsuffix -C3${BUILD}
	@make

c5:
	components/ESP32-RevK/setbuildsuffix -C5${BUILD}
	@make

c6:
	components/ESP32-RevK/setbuildsuffix -C6${BUILD}
	@make

s3:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2${BUILD}
	@make

s3eth:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-ETH${BUILD}
	@make

s3n8:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N8${BUILD}
	@make

pico:
	components/ESP32-RevK/setbuildsuffix -S1-PICO${BUILD}
	@make

wroom1:
	components/ESP32-RevK/setbuildsuffix -S1-V1${BUILD}
	@make

wroom:
	components/ESP32-RevK/setbuildsuffix -S1${BUILD}
	@make

solo:
	components/ESP32-RevK/setbuildsuffix -S1-SOLO${BUILD}
	@make

flash:
	idf.py flash

monitor:
	idf.py monitor

clean:
	idf.py clean

menuconfig:
	idf.py menuconfig

.PHONY: test
test:	main/test/faikout_auto_test main/test/heatctl_test
	./main/test/faikout_auto_test
	./main/test/heatctl_test

main/test/faikout_auto_test: main/test/faikout_auto_test.c main/faikout_auto.c main/faikout_auto.h
	gcc -O -o $@ main/test/faikout_auto_test.c main/faikout_auto.c -g -Wall --std=gnu99 -lm

main/test/heatctl_test: main/test/heatctl_test.c main/heatctl.c main/heatctl.h
	gcc -O -o $@ main/test/heatctl_test.c main/heatctl.c -g -Wall -Wextra --std=gnu99 -lm

pull:
	git pull
	git submodule update --recursive

update:
	-git pull
	-git commit -a
	git submodule update --init --recursive --remote
	idf.py update-dependencies
	-git commit -a -m "Library update"
	-git push

# Program the FTDI
ftdi: ftdizap/ftdizap
	./ftdizap/ftdizap --serial="RevK" --description="Saadin" --cbus2-mode=17 --self-powered=1

ifeq ($(shell uname),Darwin)
INCLUDES=-I/usr/local/include/
LIBS=-L/usr/local/Cellar/popt/1.18/lib/
else
LIBS=
INCLUDES=
endif

SQLlib/sqllib.o: SQLlib/sqllib.c
	make -C SQLlib
AXL/axl.o: AXL/axl.c
	make -C AXL
AJL/ajl.o: AJL/ajl.c
	make -C AJL
