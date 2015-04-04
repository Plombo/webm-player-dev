if [ `uname -s | grep -o MINGW` ]; then
  PLATFORM_FLAGS="C:/MinGW/lib/libSDL2.a `sdl2-config --cflags --libs` -I../libvpx -L../libvpx \
    -luser32 -lgdi32 -lwinmm -ldxguid -lpsapi -limm32 -lole32 -loleaut32 -lversion -mconsole"
else
  PLATFORM_FLAGS="`pkg-config sdl2 --cflags`"
fi

gcc -std=gnu99 -g -o vidplay vidplay.c vorbis.c threads_sdl.c yuv.c samplecvt.c nestegg/nestegg.c halloc/halloc.c \
    -I. -I../libvpx -lvorbis -logg -lvpx $PLATFORM_FLAGS
