all:	revk_settings idfmon

revk_settings: revk_settings.c settings.def
	gcc -O -o $@ $< -g -Wall --std=gnu99 -lpopt
	./revk_settings settings.def -h settings.h -c settings.c

idfmon: idfmon.c
	gcc -O -o $@ $< -g -Wall --std=gnu99
