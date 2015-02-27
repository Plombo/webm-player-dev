#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include "nestegg/nestegg.h"

// libvpx
#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

// SDL
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#ifdef _WIN32
#undef main
#endif

// our headers
#include "vorbis.h"

// temporary
#define MAX_AUDIO_FRAME_SIZE 192000

#define MIN(X,Y) (((X)<(Y))?(X):(Y))

typedef struct PacketList {
	nestegg_packet *pkt;
	struct PacketList *next;
} PacketList;

typedef struct PacketQueue {
	PacketList *first_pkt, *last_pkt;
	int nb_packets;
	//int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;
static int quit = 0;

void packet_queue_init(PacketQueue *q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, nestegg_packet *pkt)
{

	PacketList *pkt1;
	pkt1 = malloc(sizeof(PacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = pkt;
	pkt1->next = NULL;
	
	SDL_LockMutex(q->mutex);
	
	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	SDL_CondSignal(q->cond);
	
	SDL_UnlockMutex(q->mutex);
	return 0;
}
static int packet_queue_get(PacketQueue *q, nestegg_packet **pkt, int block)
{
	PacketList *pkt1;
	int ret;
	
	SDL_LockMutex(q->mutex);
	
	for(;;) {
		if(quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			*pkt = pkt1->pkt;
			free(pkt1);
			ret = 1;
			break;
		} else if (!block) {
			ret = 0;
			break;
		} else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

int audio_decode_frame(vorbis_context *vorbis_ctx, uint8_t *audio_buf, int buf_size)
{
	static int avail_samples = 0;
	int samples;

	if (avail_samples == 0)
	{
		nestegg_packet *pkt;
		int chunk, num_chunks;
		
		if (packet_queue_get(&audioq, &pkt, 1) < 0)
			return -1;
		nestegg_packet_count(pkt, &num_chunks);
		for (chunk=0; chunk<num_chunks; chunk++)
		{
			unsigned char *data;
			size_t data_size;
			nestegg_packet_data(pkt, chunk, &data, &data_size);
			avail_samples = vorbis_packet(vorbis_ctx, data, data_size);
		}
		nestegg_free_packet(pkt);
	}
	
	samples = MIN(avail_samples, buf_size / (vorbis_ctx->channels * 2));
	vorbis_getpcm(vorbis_ctx, audio_buf, samples);
	avail_samples -= samples;
	return samples * vorbis_ctx->channels * 2;
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
	vorbis_context *vorbis_ctx = (vorbis_context *)userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while(len > 0) {
		if(audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(vorbis_ctx, audio_buf, sizeof(audio_buf));
			if(audio_size < 0) {
				/* If error, output silence */
				audio_buf_size = 1024; // arbitrary?
				memset(audio_buf, 0, audio_buf_size);
			} else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if(len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

int webm_read(void *buffer, size_t length, void *userdata)
{
	FILE *webmfile = (FILE*)userdata;
	int bytesRead = fread(buffer, 1, length, webmfile);
	if (bytesRead == length) return 1;
	else if (ferror(webmfile)) return -1;
	else
	{
		assert(feof(webmfile));
		return 0;
	}
}

int webm_seek(int64_t offset, int whence, void *userdata)
{
	return fseek((FILE*)userdata, offset, whence);
}

int64_t webm_tell(void *userdata)
{
	return ftell((FILE*)userdata);
}

int init_audio(nestegg *ctx, int track, vorbis_context *vorbis_ctx)
{
	SDL_AudioSpec wanted_spec;
	nestegg_audio_params audioParams;
	nestegg_track_audio_params(ctx, track, &audioParams);
	wanted_spec.freq = (int)audioParams.rate;
	wanted_spec.format = (audioParams.depth / audioParams.channels) == 8 ? AUDIO_U8 : AUDIO_S16;
	wanted_spec.channels = audioParams.channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = 1024;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = vorbis_ctx;
	printf("%f Hz, %d channels, %d bits/sample\n", audioParams.rate, audioParams.channels, audioParams.depth);
	
	vorbis_ctx->channels = audioParams.channels;
	vorbis_ctx->demux_ctx = ctx;
	packet_queue_init(&audioq);
	
	if (SDL_OpenAudio(&wanted_spec, NULL) == -1)
		printf("failed to open audio device\n");
	SDL_PauseAudio(0);
}

int main(int argc, char **argv)
{
	FILE *webmfile;
	nestegg_io io;
	nestegg *demux_ctx;
	vpx_codec_ctx_t vpx_ctx;
	SDL_Surface *screen;
	SDL_Overlay *overlay;
	int video_stream = -1, audio_stream = -1;
	vorbis_context vorbis_ctx;
	
	if (argc < 2)
	{
		fprintf(stderr, "Please provide a movie file\n");
		return 1;
	}
	
	// init SDL
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE);
	
	// set up I/O callbacks
	io.read = webm_read;
	io.seek = webm_seek;
	io.tell = webm_tell;
	
	// open video file
	webmfile = fopen(argv[1], "rb");
	assert(webmfile);
	io.userdata = webmfile;
	nestegg_init(&demux_ctx, io, NULL, -1);
	
	// get number of tracks
	int num_tracks, i, j;
	if (nestegg_track_count(demux_ctx, &num_tracks) != 0) exit(2);
	
	// find the first video and audio tracks
	for (i=0; i<num_tracks; i++)
	{
		int track_type = nestegg_track_type(demux_ctx, i);
		int codec = nestegg_track_codec_id(demux_ctx, i);
		if (track_type == NESTEGG_TRACK_VIDEO)
		{
			assert(codec == NESTEGG_CODEC_VP8);
			// TODO assert other things
			video_stream = i;
		}
		else if (track_type == NESTEGG_TRACK_AUDIO)
		{
			assert(codec == NESTEGG_CODEC_VORBIS);
			// TODO assert other things
			audio_stream = i;
		}
	}
	
	// VP8 params
	nestegg_video_params video_params;
	nestegg_track_video_params(demux_ctx, video_stream, &video_params);
	printf("stereo mode=%i\n", video_params.stereo_mode);
	printf("resolution=%i*%i\n", video_params.width, video_params.height);
	printf("display resolution=%i*%i\n", video_params.display_width, video_params.display_height);
	assert(video_params.stereo_mode == NESTEGG_VIDEO_MONO);
	
	// init vorbis
	int chunk, chunks;
	vorbis_init(&vorbis_ctx);
	nestegg_track_codec_data_count(demux_ctx, audio_stream, &chunks);
	assert(chunks == 3);
	for (chunk=0; chunk<chunks; chunk++)
	{
		unsigned char *data;
		size_t data_size;
		nestegg_track_codec_data(demux_ctx, audio_stream, chunk, &data, &data_size);
		vorbis_headerpacket(&vorbis_ctx, data, data_size, chunk);
	}
	vorbis_prepare(&vorbis_ctx);
	init_audio(demux_ctx, audio_stream, &vorbis_ctx);

	// init libvpx
	if (vpx_codec_dec_init(&vpx_ctx, vpx_codec_vp8_dx(), NULL, 0))
		exit(1);

	screen = SDL_SetVideoMode(video_params.width, video_params.height, 32, SDL_ANYFORMAT);
	
	// Allocate a place to put our YUV image on that screen
	overlay = SDL_CreateYUVOverlay(video_params.width, video_params.height, SDL_YV12_OVERLAY, screen);
	
	nestegg_packet *pkt;
	int r;
	while ((r = nestegg_read_packet(demux_ctx, &pkt)) > 0)
	{
		unsigned int track;
		nestegg_packet_track(pkt, &track);
		
		if (track == audio_stream)
		{
			packet_queue_put(&audioq, pkt);
			continue;
		}
		
		unsigned int chunk, chunks;
		nestegg_packet_count(pkt, &chunks);
		//printf("%i chunks\n", chunks);

		for (chunk = 0; chunk < chunks; ++chunk)
		{
			unsigned char *data;
			size_t data_size;
			nestegg_packet_data(pkt, chunk, &data, &data_size);
			
			if (track == video_stream)
			{
				vpx_image_t *img;
				vpx_codec_iter_t iter = NULL;
				if (vpx_codec_decode(&vpx_ctx, data, data_size, NULL, 0))
				{
					printf("Failed to decode frame\n");
					exit(1);
				}
				while((img = vpx_codec_get_frame(&vpx_ctx, &iter)))
				{
					static int nFrames = 0;
					SDL_Rect rect = {0, 0, img->d_w, img->d_h};
					
					SDL_LockYUVOverlay(overlay);
					for (int y=0; y < video_params.height; ++y)
						memcpy(overlay->pixels[0]+(overlay->pitches[0]*y),
							img->planes[0]+(img->stride[0]*y),
							overlay->pitches[0]);
					for (int y=0; y < video_params.height/2; ++y)
						memcpy(overlay->pixels[1]+(overlay->pitches[1]*y),
								img->planes[2]+(img->stride[2]*y),
								overlay->pitches[1]);
					for (int y=0; y < video_params.height/2; ++y)
						memcpy(overlay->pixels[2]+(overlay->pitches[2]*y),
								img->planes[1]+(img->stride[1]*y),
								overlay->pitches[2]);
					SDL_UnlockYUVOverlay(overlay);
					SDL_DisplayYUVOverlay(overlay, &rect);
                    
                    //printf("FPS: %f\n", (double)(++nFrames) * 1000 / SDL_GetTicks());
				}
			}
		}
		nestegg_free_packet(pkt);
	}
	nestegg_destroy(demux_ctx);
	SDL_Quit();
	
	return 0;
}

