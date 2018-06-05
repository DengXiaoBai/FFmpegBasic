
#include "avcodec.h"
#include "avformat.h"
#include "swscale.h"

#include "SDL.h"
#include "SDL_thread.h"


#include <stdio.h>
#include <assert.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024

// 1 second of 48khz 32bit audio  48000 * 4
#define MAX_AUDIO_FRAME_SIZE 192000


#pragma mark - Packet Queue Impelementation (for Audio stream)
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

static PacketQueue audioq;

static int quit = 0;

static void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

/**
 SDL_LockMutex() locks the mutex in the queue so we can add something to it,
 and then SDL_CondSignal() sends a signal to our get function (if it is
 waiting) through our condition variable to tell it that there is data and
 it can proceed, then unlocks the mutex to let it go.
 */
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    
    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
    return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
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
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
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


#pragma mark - Audio decoding functions
static int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AVFrame *decoded_aframe;
    static AVPacket pkt, pktTemp;
    //    static uint8_t *audio_pkt_data = NULL;
    //    static int audio_pkt_size = 0;
    
    int len1, data_size;
    
    for(;;) {
        
        while(pktTemp.size > 0)
        {
            int got_frame = 0;
            
            if (!decoded_aframe) {
                if (!(decoded_aframe = av_frame_alloc())) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
            } else{
                  // avcodec_get_frame_defaults(decoded_aframe);
                av_frame_unref(decoded_aframe);
            }
            
            
            //data_size = buf_size; /// ????
            len1 = avcodec_decode_audio4(aCodecCtx, decoded_aframe, &got_frame, &pktTemp);
            
            
            /// Check if
            if (len1 < 0) {
                pktTemp.size = 0;
                break; // skip packet
            }
            
            
            if (got_frame) {
                printf("\nGot frame!");
                //printf("\nFrame data size: %d", sizeof(decoded_aframe->data[0]));
                data_size = av_samples_get_buffer_size(NULL, aCodecCtx->channels,
                                                       decoded_aframe->nb_samples,
                                                       aCodecCtx->sample_fmt, 1);
                if (data_size > buf_size) {
                    data_size = buf_size;
                }
                memcpy(audio_buf, decoded_aframe->data[0], data_size);
                
            }else{
                data_size = 0;
            }
            
            printf("\nData size %d", data_size);
            pktTemp.data += len1;
            pktTemp.size -= len1;
            
            if (data_size <= 0) {
                continue;
            }
            
            return data_size;
            /* Deprecated
             data_size = buf_size;
             len1 = avcodec_decode_audio2(aCodecCtx, (int16_t *)audio_buf, &data_size,
             audio_pkt_data, audio_pkt_size);
             if(len1 < 0) {
             // if error, skip frame
             audio_pkt_size = 0;
             break;
             }
             audio_pkt_data += len1;
             audio_pkt_size -= len1;
             if(data_size <= 0) {
             // No data yet, get more frames
             continue;
             }
             // We have data, return it and come back for more later
             return data_size;
             */
        }
        
        
        if(pkt.data)
            av_free_packet(&pkt);
        
        if(quit)
        {
            return -1;
        }
        
        
        /// Get packet from queue
        if(packet_queue_get(&audioq, &pkt, 1) < 0)
        {
            return -1;
        }
        
        
        av_init_packet(&pktTemp);
        
        pktTemp.data = pkt.data;
        pktTemp.size = pkt.size;
    }
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;
    
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    
    while(len > 0) {
        /// Need to read more audio data
        if(audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
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


#pragma mark - Main function
int play_sound(char* url){
    AVFormatContext *pFormatCtx = NULL;
    int             i, videoStream, audioStream;
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    AVFrame         *pFrame;
    AVPacket        packet;
    int             frameFinished;
    static struct SwsContext *img_convert_ctx;
    
    AVCodecContext  *aCodecCtx;
    AVCodec         *aCodec;
    
    SDL_Window *screen;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture;
    
    SDL_Rect        rect;
    SDL_Event       event;
    SDL_AudioSpec   wanted_spec, spec;
    
    Uint8 *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    int uvPitch;
    
   
    // Register all formats and codecs
    av_register_all();
    
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    
    // Open video file
    if(avformat_open_input(&pFormatCtx, url, NULL, NULL)!=0)
        return -1; // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, url, 0);
    
    /// Find the first video and audio stream
    videoStream=-1;
    audioStream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
           videoStream < 0) {
            videoStream=i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
           audioStream < 0) {
            audioStream=i;
        }
    }
    if(videoStream==-1)
        return -1; // Didn't find a video stream
    if(audioStream==-1)
        return -1;
    
    aCodecCtx=pFormatCtx->streams[audioStream]->codec;
    
    /// Set audio settings from codec info
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback; ///<----- Callback to feed audio data
    wanted_spec.userdata = aCodecCtx;
    
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    
    /// Find audio codec
    aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if(!aCodec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    /// Open codec
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
        return -1;
    
    // audio_st = pFormatCtx->streams[index]
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);
    
    // Get a pointer to the codec context for the video stream
    pCodecCtx=pFormatCtx->streams[videoStream]->codec;
    
    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
        return -1; // Could not open codec
    
    // Allocate video frame
    pFrame=av_frame_alloc();
    
    // Make a screen to put our video
    screen = SDL_CreateWindow(
                              "FFmpeg Tutorial",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              pCodecCtx->width / 2,
                              pCodecCtx->height / 2,
                              0
                              );
    
    if(!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }
    
    
    
    renderer = SDL_CreateRenderer(screen, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL: could not create renderer - exiting\n");
        exit(1);
    }
    
    
    int w = pCodecCtx->width;
    int h = pCodecCtx->height;
    
    // Allocate a place to put our YUV image on that screen
    texture = SDL_CreateTexture(
                                renderer,
                                SDL_PIXELFORMAT_YV12,
                                SDL_TEXTUREACCESS_STREAMING,
                                pCodecCtx->width,
                                pCodecCtx->height
                                );
    if (!texture) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        
    }
    
    
    img_convert_ctx = sws_getContext(pCodecCtx->width,
                                     pCodecCtx->height,
                                     pCodecCtx->pix_fmt,
                                     pCodecCtx->width,
                                     pCodecCtx->height,
                                     AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR,
                                     NULL,
                                     NULL,
                                     NULL
                                     );
    
    
    
    // set up YV12 pixel array (12 bits per pixel)
    yPlaneSz = pCodecCtx->width * pCodecCtx->height;
    uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
    yPlane = (Uint8*)malloc(yPlaneSz);
    uPlane = (Uint8*)malloc(uvPlaneSz);
    vPlane = (Uint8*)malloc(uvPlaneSz);
    if (!yPlane || !uPlane || !vPlane) {
        fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
        exit(1);
    }
    
    uvPitch = pCodecCtx->width / 2;
    
    
    // Read frames and save first five frames to disk
    i=0;
    
    while(av_read_frame(pFormatCtx, &packet)>=0) {
        /// Is this a packet from the video stream?
        if(packet.stream_index==videoStream) {
            // Decode video frame
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
            
            // Did we get a video frame?
            if(frameFinished) {
                AVPicture pict;
                pict.data[0] = yPlane;
                pict.data[1] = uPlane;
                pict.data[2] = vPlane;
                pict.linesize[0] = pCodecCtx->width;
                pict.linesize[1] = uvPitch;
                pict.linesize[2] = uvPitch;
                
                
                // Convert the image into YUV format that SDL uses
                sws_scale(img_convert_ctx, (uint8_t const * const *) pFrame->data,
                          pFrame->linesize, 0, pCodecCtx->height, pict.data,
                          pict.linesize);
                
                SDL_UpdateYUVTexture(
                                     texture,
                                     NULL,
                                     yPlane,
                                     pCodecCtx->width,
                                     uPlane,
                                     uvPitch,
                                     vPlane,
                                     uvPitch
                                     );
                
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                
                av_free_packet(&packet);
            }
            /// Is this a packet from audio streams?
        } else if(packet.stream_index == audioStream) {
            packet_queue_put(&audioq, &packet);
        } else {
            av_free_packet(&packet);
        }
        
        /// Free the packet that was allocated by av_read_frame
        SDL_PollEvent(&event);
        
        switch(event.type) {
            case SDL_QUIT:
                quit = 1;
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
        
    }
    
    // Free the YUV frame
    av_free(pFrame);
    
    // Close the codec
    avcodec_close(pCodecCtx);
    
    // Close the video file
    avformat_close_input(&pFormatCtx);
    
    return 0;
}

