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
#include "threads.h"

// temporary
#define MAX_AUDIO_FRAME_SIZE 192000
#define ANYNUMBER 2
#define MIN(X,Y) (((X)<(Y))?(X):(Y))

#define PACKET_QUEUE_SIZE 10

typedef struct {
	int start;
	int size;
	int max_size;
	bor_mutex *mutex;
	bor_cond *not_full;
	bor_cond *not_empty;
	void *data[ANYNUMBER];
} FixedSizeQueue;

typedef struct {
	FixedSizeQueue *packet_queue;
	vorbis_context vorbis_ctx;
	vpx_codec_ctx_t vpx_ctx;
	FixedSizeQueue *frame_queue;
	int frequency;
} audio_context;

typedef struct {
	FixedSizeQueue *packet_queue;
	SDL_Surface *screen;
	vpx_codec_ctx_t vpx_ctx;
	FixedSizeQueue *frame_queue;
	int width;
	int height;
	uint64_t frame_delay;
} video_context;

typedef struct {
	nestegg *demux_ctx;
	video_context *video_ctx;
	FixedSizeQueue *audio_queue;
	int audio_stream;
	int video_stream;
} decoder_context;


static int quit_video = 0;
uint64_t audio_clock = 0.0;

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

FixedSizeQueue *queue_init(int max_size)
{
	FixedSizeQueue *queue = malloc(sizeof(FixedSizeQueue) + ((max_size - ANYNUMBER) * sizeof(void *)));
	queue->start = 0;
	queue->size = 0;
	queue->max_size = max_size;
	queue->mutex = mutex_create();
	queue->not_full = cond_create();
	queue->not_empty = cond_create();
	return queue;
}

#define SPIT(fmt, ...) printf("%s:%i: " fmt, __func__, __LINE__, __VA_ARGS__)

void queue_insert(FixedSizeQueue *queue, void *data)
{
	mutex_lock(queue->mutex);
	//SPIT("size=%i\n", queue->size);
	if (queue->size == queue->max_size)
	{
		while(cond_wait_timed(queue->not_full, queue->mutex, 10) != 0)
		{
			if (quit_video)
			{
				mutex_unlock(queue->mutex);
				return;
			}
		}
	}
	assert(queue->size < queue->max_size);
	int index = (queue->start + queue->size) % queue->max_size;
	queue->data[index] = data;
	queue->size++;
	//SPIT("size=%i\n", queue->size);
	cond_signal(queue->not_empty);
	mutex_unlock(queue->mutex);
}

void *queue_get(FixedSizeQueue *queue)
{
	mutex_lock(queue->mutex);
	//SPIT("size=%i\n", queue->size);
	if (queue->size == 0)
	{
		while (cond_wait_timed(queue->not_empty, queue->mutex, 10) != 0)
		{
			if (quit_video)
			{
				mutex_unlock(queue->mutex);
				return NULL;
			}
		}
	}
	assert(queue->size > 0);
	void *data = queue->data[queue->start];
	--queue->size;
	queue->start = (queue->start + 1) % queue->max_size;
	//SPIT("size=%i\n", queue->size);
	cond_signal(queue->not_full);
	mutex_unlock(queue->mutex);
	return data;
}

void queue_destroy(FixedSizeQueue *queue)
{
	cond_destroy(queue->not_full);
	cond_destroy(queue->not_empty);
	mutex_destroy(queue->mutex);
	free(queue);
}

int audio_decode_frame(audio_context *audio_ctx, uint8_t *audio_buf, int buf_size)
{
	vorbis_context *vorbis_ctx = &audio_ctx->vorbis_ctx;
	static int avail_samples = 0;
	static int samples = 0;

	if (avail_samples == 0)
	{
		nestegg_packet *pkt;
		uint64_t timestamp;
		int chunk, num_chunks;
		
		if ((pkt = queue_get(audio_ctx->packet_queue)) == NULL)
			return -1;
		nestegg_packet_tstamp(pkt, &timestamp);
		audio_clock = timestamp;
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
	else audio_clock += 1000000000LL * samples / audio_ctx->frequency;
	
	samples = MIN(avail_samples, buf_size / (vorbis_ctx->channels * 2));
	vorbis_getpcm(vorbis_ctx, audio_buf, samples);
	avail_samples -= samples;
	return samples * vorbis_ctx->channels * 2;
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
	audio_context *audio_ctx = (audio_context *)userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while(len > 0) {
		if(audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(audio_ctx, audio_buf, sizeof(audio_buf));
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

int init_audio(nestegg *ctx, int track, audio_context *audio_ctx)
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
	wanted_spec.userdata = audio_ctx;
	printf("%f Hz, %d channels, %d bits/sample\n", audioParams.rate, audioParams.channels, audioParams.depth);
	
	audio_ctx->vorbis_ctx.channels = audioParams.channels;
	audio_ctx->frequency = (int)audioParams.rate;
	audio_ctx->packet_queue = queue_init(PACKET_QUEUE_SIZE);
	
	if (SDL_OpenAudio(&wanted_spec, NULL) == -1)
		printf("failed to open audio device\n");
	SDL_PauseAudio(0);
}

int video_thread(void *data)
{
	video_context *ctx = (video_context*) data;
	
	while(!quit_video)
	{
		unsigned int chunk, chunks;
		nestegg_packet *pkt;
		
		pkt = queue_get(ctx->packet_queue);
		if (quit_video) break;
		nestegg_packet_count(pkt, &chunks);

		for (chunk = 0; chunk < chunks; ++chunk)
		{
			unsigned char *data;
			size_t data_size;
			nestegg_packet_data(pkt, chunk, &data, &data_size);
			
			vpx_image_t *img;
			vpx_codec_iter_t iter = NULL;
			if (vpx_codec_decode(&ctx->vpx_ctx, data, data_size, NULL, 0))
			{
				printf("Failed to decode frame\n");
				exit(1);
			}
			while((img = vpx_codec_get_frame(&ctx->vpx_ctx, &iter)))
			{
				static int nFrames = 0;
				assert(img->d_w == ctx->width);
				assert(img->d_h == ctx->height);
				SDL_Overlay *overlay = SDL_CreateYUVOverlay(img->d_w, img->d_h, SDL_YV12_OVERLAY, ctx->screen);
				
				SDL_LockYUVOverlay(overlay);
				for (int y=0; y < img->d_h; ++y)
					memcpy(overlay->pixels[0]+(overlay->pitches[0]*y),
						img->planes[0]+(img->stride[0]*y),
						overlay->pitches[0]);
				for (int y=0; y < img->d_h/2; ++y)
					memcpy(overlay->pixels[1]+(overlay->pitches[1]*y),
							img->planes[2]+(img->stride[2]*y),
							overlay->pitches[1]);
				for (int y=0; y < img->d_h/2; ++y)
					memcpy(overlay->pixels[2]+(overlay->pitches[2]*y),
							img->planes[1]+(img->stride[1]*y),
							overlay->pitches[2]);
				SDL_UnlockYUVOverlay(overlay);
				
				queue_insert(ctx->frame_queue, (void *)overlay);
				
				//printf("FPS: %f\n", nFrames * 1000.0 / SDL_GetTicks());
				++nFrames;
			}
		}
		nestegg_free_packet(pkt);
	}
	return 0;
}

int demux_thread(void *data)
{
	decoder_context *ctx = (decoder_context *)data;
	nestegg_packet *pkt;
	int r;
	while ((r = nestegg_read_packet(ctx->demux_ctx, &pkt)) > 0)
	{
		unsigned int track;
		nestegg_packet_track(pkt, &track);
		
		if (track == ctx->audio_stream)
			queue_insert(ctx->audio_queue, pkt);
		else if (track == ctx->video_stream)
			queue_insert(ctx->video_ctx->packet_queue, pkt);
		
		if (quit_video) break;
	}
	quit_video = 1;
	return 0;
}

int main(int argc, char **argv)
{
	FILE *webmfile;
	nestegg_io io;
	nestegg *demux_ctx;
	video_context video_ctx;
	audio_context audio_ctx;
	SDL_Surface *screen;
	int video_stream = -1, audio_stream = -1;
	
	if (argc < 2)
	{
		fprintf(stderr, "Please provide a video file\n");
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
	vorbis_init(&audio_ctx.vorbis_ctx);
	nestegg_track_codec_data_count(demux_ctx, audio_stream, &chunks);
	assert(chunks == 3);
	for (chunk=0; chunk<chunks; chunk++)
	{
		unsigned char *data;
		size_t data_size;
		nestegg_track_codec_data(demux_ctx, audio_stream, chunk, &data, &data_size);
		vorbis_headerpacket(&audio_ctx.vorbis_ctx, data, data_size, chunk);
	}
	vorbis_prepare(&audio_ctx.vorbis_ctx);
	init_audio(demux_ctx, audio_stream, &audio_ctx);

	// init libvpx
	if (vpx_codec_dec_init(&video_ctx.vpx_ctx, vpx_codec_vp8_dx(), NULL, 0))
		exit(1);
	video_ctx.packet_queue = queue_init(PACKET_QUEUE_SIZE);
	video_ctx.frame_queue = queue_init(PACKET_QUEUE_SIZE);
	video_ctx.screen = SDL_SetVideoMode(video_params.width, video_params.height, 32, SDL_ANYFORMAT);
	video_ctx.width = video_params.width;
	video_ctx.height = video_params.height;
	
	// FIXME use actual packet timestamps
	uint64_t default_frame_duration;
	nestegg_track_default_duration(demux_ctx, video_stream, &default_frame_duration);
	video_ctx.frame_delay = default_frame_duration;
	
	// start video thread
	bor_thread *the_video_thread = thread_create(video_thread, &video_ctx);
	
	// start demux thread
	decoder_context decoder_ctx;
	decoder_ctx.demux_ctx = demux_ctx;
	decoder_ctx.video_ctx = &video_ctx;
	decoder_ctx.audio_queue = audio_ctx.packet_queue;
	decoder_ctx.audio_stream = audio_stream;
	decoder_ctx.video_stream = video_stream;
	bor_thread *the_demux_thread = thread_create(demux_thread, &decoder_ctx);
	
	uint64_t next_frame_time = 0;
	SDL_Rect rect = {0, 0, video_params.width, video_params.height};
	
	while(!quit_video)
	{
		SDL_Event event;

		SDL_PollEvent(&event);
		switch(event.type)
		{
		case SDL_QUIT:
			quit_video = 1;
			break;
		default:
			break;
		}
		
		if (next_frame_time <= audio_clock)
		{
			SDL_Overlay *overlay = (SDL_Overlay *)queue_get(video_ctx.frame_queue);
			SDL_DisplayYUVOverlay(overlay, &rect);
			SDL_FreeYUVOverlay(overlay);
			next_frame_time += default_frame_duration;
		}
	}
	
	thread_join(the_demux_thread);
	thread_join(the_video_thread);
	
	nestegg_destroy(demux_ctx);
	SDL_Quit();
	
	return 0;
}

