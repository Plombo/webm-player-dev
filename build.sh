
gcc -g -o vidplay vidplay.c vorbis.c threads_sdl.c yuv.c -I.. -I../nestegg/include -I C:/MinGW/include/SDL nestegg.o -lvorbis -logg -lvpx -I../libvpx -std=gnu99 -L../libvpx ../nestegg/halloc.o -lSDL -luser32 -lgdi32 -lwinmm -ldxguid -lpsapi
