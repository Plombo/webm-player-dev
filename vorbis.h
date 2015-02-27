#ifndef VORBIS_H
#define VORBIS_H

#include <stdlib.h>
#include "nestegg/nestegg.h"

#if TREMOR
#include <tremor/ivorbiscodec.h>
#include "clipto15.h"
#else
#include <vorbis/codec.h>
#include "vorbisfpu.h"
#endif

typedef struct {
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state v;
	vorbis_block vb;
	nestegg *demux_ctx;
	int channels;
} vorbis_context;

void vorbis_init(vorbis_context *ctx);
void vorbis_prepare(vorbis_context *ctx);
void vorbis_headerpacket(vorbis_context *ctx, void *data, size_t size, int packetCount);
int vorbis_packet(vorbis_context *ctx, void *data, size_t size);
void vorbis_getpcm(vorbis_context *ctx, char *buffer, size_t samples);

#endif
