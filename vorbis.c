#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "vorbis.h"

void vorbis_init(vorbis_context *ctx)
{
	vorbis_info_init(&ctx->vi);
	vorbis_comment_init(&ctx->vc);
}

// call after reading header packets but before audio data
void vorbis_prepare(vorbis_context *ctx)
{
	if (vorbis_synthesis_init(&ctx->v, &ctx->vi) != 0) exit(1);
	vorbis_block_init(&ctx->v, &ctx->vb);
}

void vorbis_headerpacket(vorbis_context *ctx, void *data, size_t size, int packetCount)
{
	ogg_packet op;
	memset(&op, 0, sizeof(op));
	op.packet = data;
	op.bytes = size;
	op.b_o_s = (packetCount == 0);
	op.e_o_s = 0;
	op.granulepos = 0;
	op.packetno = packetCount;
	
	int result = vorbis_synthesis_headerin(&ctx->vi, &ctx->vc, &op);
	if (result != 0) fprintf(stdout, "vorbis_synthesis_headerin returned %i\n", result);
}

// return number of samples available
int vorbis_packet(vorbis_context *ctx, void *data, size_t size)
{
#if TREMOR
	ogg_int32_t **pcm;
#else
	float **pcm;
#endif
	
	ogg_packet op;
	memset(&op, 0, sizeof(op));
	op.packet = data;
	op.bytes = size;
	op.b_o_s = 0;
	op.e_o_s = 0;
	op.granulepos = 0;
	op.packetno = 3;
	
	int result = vorbis_synthesis(&ctx->vb, &op);
	if (result != 0) fprintf(stdout, "vorbis_synthesis returned %i\n", result);
	result = vorbis_synthesis_blockin(&ctx->v, &ctx->vb);
	if (result != 0) fprintf(stdout, "vorbis_synthesis_blockin returned %i\n", result);
	int samples = vorbis_synthesis_pcmout(&ctx->v, &pcm);
	vorbis_synthesis_read(&ctx->v, 0);
	return samples;
}

void vorbis_getpcm(vorbis_context *ctx, char *buffer, size_t samples)
{
#if TREMOR
	ogg_int32_t **pcm;
#else
	float **pcm;
#endif
	
	int avail_samples = vorbis_synthesis_pcmout(&ctx->v, &pcm);
	assert(avail_samples >= samples);
	
	// pack audio data
	long channels = ctx->channels;
	//char *buffer = malloc(samples * channels * 2);
	int i, j;

#if TREMOR
	for (i=0; i<channels; i++)
	{
		ogg_int32_t *src = pcm[i];
		short *dest = ((short *)buffer)+i;
		for (j=0; j<samples; j++)
		{
			*dest = CLIP_TO_15(src[j]>>9);
			dest += channels;
		}
	}
#else
	int word = 2;
	int sgned = 1;
	int bigendianp = 0, host_endian = 0;
	long bytespersample = word * channels;
	vorbis_fpu_control fpu;

	/* a tight loop to pack each size */
	{
		int val;
		if(word==1){
			int off=(sgned?0:128);
			vorbis_fpu_setround(&fpu);
			for(j=0;j<samples;j++)
				for(i=0;i<channels;i++){
					val=vorbis_ftoi(pcm[i][j]*128.f);
					if(val>127)val=127;
					else if(val<-128)val=-128;
					*buffer++=val+off;
				}
			vorbis_fpu_restore(fpu);
		}else{
			int off=(sgned?0:32768);

			if(host_endian==bigendianp){
				if(sgned){

					vorbis_fpu_setround(&fpu);
					for(i=0;i<channels;i++) { /* It's faster in this order */
						float *src=pcm[i];
						short *dest=((short *)buffer)+i;
						for(j=0;j<samples;j++) {
							val=vorbis_ftoi(src[j]*32768.f);
							if(val>32767)val=32767;
							else if(val<-32768)val=-32768;
							*dest=val;
							dest+=channels;
						}
					}
					vorbis_fpu_restore(fpu);

				}else{

					vorbis_fpu_setround(&fpu);
					for(i=0;i<channels;i++) {
						float *src=pcm[i];
						short *dest=((short *)buffer)+i;
						for(j=0;j<samples;j++) {
							val=vorbis_ftoi(src[j]*32768.f);
							if(val>32767)val=32767;
							else if(val<-32768)val=-32768;
							*dest=val+off;
							dest+=channels;
						}
					}
					vorbis_fpu_restore(fpu);

				}
			}
		}
	}
#endif

    vorbis_synthesis_read(&ctx->v, samples);
}



