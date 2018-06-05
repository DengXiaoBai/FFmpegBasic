
//  tutorial03.c
//  simplest_ffmpeg_player


#include "avcodec.h"
#include "avformat.h"
#include "swscale.h"

#include "SDL.h"
#include "SDL_thread.h"
#include "SDL_mutex.h"


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


#pragma mark - 包队列
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

// push
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
        // 存储第一个
        q->first_pkt = pkt1;
    else
        // 更新上一个的next,使其指向最新一个
        q->last_pkt->next = pkt1;

    // 始终存储最新一个
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
    return 0;
}

// pop
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

        // First In First Out
        // first_pkt = pkt1   pkt1->next = NULL  last_pkt = pkt1
        // first_pkt = pkt1   pkt1->next = pkt2  last_pkt = pkt2  pkt2->next = NULL
        // 有数据拿数据, 没数据等push进来的数据
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
            //unlock the mutex we give it and then attempt to lock it again once we get the signal
            //执行SDL_CondWait时unlock, 等到信号之后又lock,所以不会因为死循环造成不能push进queue的情况
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}


#pragma mark - 解码音频
static int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

    // 局部全局变量, 只有第一次调用才会初始化, 其他调用不会初始化
    // 下次调用可能需要用到上次调用的数据
    static AVPacket pkt;
    static int audio_pkt_size = 0;
    static uint8_t *audio_pkt_data = NULL;
    static AVFrame frame;


    int len1, data_size = 0;

    for(;;) {

        while(audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
            if(len1 < 0) {
                /* if error, skip frame */
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            if(got_frame) {
                data_size = av_samples_get_buffer_size(NULL,
                                                       aCodecCtx->channels,
                                                       frame.nb_samples,
                                                       aCodecCtx->sample_fmt,
                                                       1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, frame.data[0], data_size);
            }
            if(data_size <= 0) {
                /* No data yet, get more frames */
                continue;
            }
            /* We have data, return it and come back for more later */
            return data_size;
        }


        if(pkt.data)
            av_free_packet(&pkt);

        if(quit) {
            return -1;
        }

        if(packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}




#pragma mark - SDL播放音频回调
/* stream : SDL 存储数据buffer
 * len    : buffer大小
 * */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;

    // 局部全局变量, 只有第一次调用才会初始化, 其他调用不会初始化
     // 内部自己用来存储解析后的音频的缓存区
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
     // 某一次调用audio_decode_frame存入audio_buf里面的数据的大小
    static unsigned int audio_buf_size = 0;
     // 在audio_buf中的位置
    static unsigned int audio_buf_position = 0;


    // 只要stream有空间, 就往里面塞数据
    while(len > 0) {
        // 如果没有数据,就调用audio_decode_frame去读取
        if(audio_buf_position >= audio_buf_size) {
            // 把解析好的数据存入audio_buf
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if(audio_size < 0) {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                /* 清零，静音 */
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }

            // 重置
            audio_buf_position = 0;
        }

        // 查看audio_buf剩余数据
        len1 = audio_buf_size - audio_buf_position;
        if(len1 > len)
            len1 = len;

        // copy 数据到stream
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_position, len1);

        len -= len1;
        stream += len1;
        audio_buf_position += len1;
    }
}

#pragma mark - 主方法
/* 这个demo存在的问题
 * 视频播放很快
 * 音频播放也很快,听不出原来的声音: 可能是那个哪个频率参数设置错误
 * */
int play_media(char* media_url){
    
    AVFormatContext *pFormatCtx = NULL;
    int             i, videoStream, audioStream;
    
    // video
    AVCodecContext  *pCodecCtxOrig = NULL;
    AVCodecContext  *pCodecCtx = NULL;
    AVCodec         *pCodec = NULL;
    
    // audio
    AVCodecContext  *aCodecCtxOrig = NULL;
    AVCodecContext  *aCodecCtx = NULL;
    AVCodec         *aCodec = NULL;
    
    AVFrame         *pFrame = NULL;
    AVPacket        packet;
    int             frameFinished;
    struct SwsContext *sws_ctx = NULL;
    
    
    SDL_AudioSpec   wanted_spec, spec;
    
    SDL_Event event;
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    
    Uint8 *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    int uvPitch;
    
    // char filepath[]= "rtmp://localhost:1935/rtmplive";
    // char filepath[]  = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/AS.mp4";
    
    // Register all formats and codecs
    av_register_all();
    
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    
    // Open video file
    if(avformat_open_input(&pFormatCtx,media_url, NULL, NULL)!=0)
        return -1; // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, media_url, 0);
    
    // Find the first video stream
    videoStream=-1;
    audioStream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO &&
           videoStream < 0) {
            videoStream=i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
           audioStream < 0) {
            audioStream=i;
        }
    }
//    if(videoStream==-1)
//        return -1; // Didn't find a video stream
    
    if(audioStream==-1)
        return -1;
    
    aCodecCtxOrig=pFormatCtx->streams[audioStream]->codec;
    aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id);
    if(!aCodec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    // Copy context
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if(avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }
    
    // Set audio settings from codec info
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    
    wanted_spec.userdata = aCodecCtx; // 会由SDL传给回调
    
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    
    avcodec_open2(aCodecCtx, aCodec, NULL);
    
    // audio_st = pFormatCtx->streams[index]
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);
    
    
    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig=pFormatCtx->streams[videoStream]->codec;
    
    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
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
    
    
    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
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
        // Is this a packet from the video stream?
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
                sws_scale(sws_ctx, (uint8_t const * const *) pFrame->data,
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
        } else if(packet.stream_index==audioStream) {
            packet_queue_put(&audioq, &packet);
        } else {
            av_free_packet(&packet);
        }
        // Free the packet that was allocated by av_read_frame
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
    av_frame_free(&pFrame);
    
    // Close the codecs
    avcodec_close(pCodecCtxOrig);
    avcodec_close(pCodecCtx);
    avcodec_close(aCodecCtxOrig);
    avcodec_close(aCodecCtx);
    
    // Close the video file
    avformat_close_input(&pFormatCtx);
    
    return 0;
}




