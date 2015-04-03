
gcc -g -o vidplay vidplay.c vorbis.c threads_sdl.c yuv.c samplecvt.c nestegg.o -I.. -I../nestegg/include -I C:/MinGW/include/SDL2 C:/MinGW/lib/libSDL2.a `sdl2-config --cflags --libs` -lvorbis -logg -lvpx -I../libvpx -std=gnu99 -L../libvpx ../nestegg/halloc.o -luser32 -lgdi32 -lwinmm -ldxguid -lpsapi -limm32 -lole32 -loleaut32 -lversion -mconsole
