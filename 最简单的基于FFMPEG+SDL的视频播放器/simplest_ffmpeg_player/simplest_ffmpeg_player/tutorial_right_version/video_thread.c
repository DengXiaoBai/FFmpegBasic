//
//  video_thread.c
//  simplest_ffmpeg_player
//
//  Created by DengXiaoBai on 2018/4/17.
//  Copyright © 2018年 angle. All rights reserved.
//
/*
 代码改动
 . av_frame_alloc() <---- avcodec_alloc_frame()
 . avcodec_get_frame_defaults(is->audio_frame) --> av_frame_unref;
 */


#include "avcodec.h"
#include "avformat.h"
#include "swscale.h"
#include "swresample.h"
#include "mem.h"

#include "SDL.h"
#include "time.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
//(1 * 1024 * 1024)//(5 * 16 * 1024)
#define MAX_AUDIO_SIZE (1 * 1024 * 1024)
//(5 * 256 * 1024)
#define MAX_VIDEO_SIZE (5 * 256 * 1024)

#define FF_ALLOC_EVENT (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio




/*
 * 存储没有解码的数据
 * */
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


/*
 * 存储解码后的视频数据
 * */
typedef struct VideoPicture {
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    
    AVFrame* rawdata;
    int width, height; /*source height & width*/
    int allocated;
} VideoPicture;

typedef struct VideoState {
    char filename[1024];
    // 输入上下文
    AVFormatContext *ic;
    // 音频 / 视频索引
    int video_index, audio_index;
    // 音频流
    AVStream *audio_st;
    // 音频帧
    AVFrame *audio_frame;
    // 音频队列
    PacketQueue audioq;
    // 音频buf, SDL音频回调使用
    unsigned int audio_buf_size;
    // 音频buf游标,SDL音频回调使用
    unsigned int audio_buf_index;
    
    // 音频包, 音频解码使用
    AVPacket audio_pkt;
    // 音频包里面的数据,同上
    uint8_t *audio_pkt_data;
    // 音频包大小,同上
    int audio_pkt_size;
    
    
    // 音频暂存buf
    uint8_t *audio_buf;
    DECLARE_ALIGNED(16,uint8_t,audio_buf2) [AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];
    
    // 音频格式参数
    enum AVSampleFormat audio_src_fmt;
    enum AVSampleFormat audio_tgt_fmt;
    int audio_src_channels;
    int audio_tgt_channels;
    int64_t audio_src_channel_layout;
    int64_t audio_tgt_channel_layout;
    int audio_src_freq;
    int audio_tgt_freq;
    
    // 音频转换
    struct SwrContext *swr_ctx;
    
    // 视频流
    AVStream *video_st;
    // 视频队列
    PacketQueue videoq;
    // I/O上下文
    AVIOContext *io_ctx;
    //视频转换
    struct SwsContext *sws_ctx;
    
    // VideoPicture数组, 大小为1
    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    // 数组大小,     数组读/写游标
    int pictq_size, pictq_rindex, pictq_windex;
    // 数组互斥锁
    SDL_mutex *pictq_mutex;
    // 数组信号
    SDL_cond *pictq_cond;
    
    // 线程
    SDL_Thread *parse_tid;
    SDL_Thread *audio_tid;
    SDL_Thread *video_tid;
    // 暂停标记
    int quit;
} VideoState;

// 全局数据
static VideoState *global_video_state;

// 队列初始化
static void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}


// 入队列, 队列FIFO, 只能一端出一端入
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    
    pkt1 = (AVPacketList *) av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt) {
        // 队列第一个元素
        q->first_pkt = pkt1;
    } else {
        // 更新上一个元素的next, 使其指向下一个入队列的元素
        q->last_pkt->next = pkt1;
    }
    
    // last_pkt始终指向最后进入队列的元素
    q->last_pkt = pkt1;

    q->nb_packets++;
    q->size += pkt1->pkt.size;
    if (pkt->stream_index == global_video_state->audio_index){
        printf("put A, q->size = %d\n",q->size);
    }else if(pkt->stream_index == global_video_state->video_index){
        printf("put V, q->size = %d\n",q->size);
    }

    // 发信号,告诉packet_queue_get, 有新元素进入队列
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    
    return 0;
}


// 出队列, 队列FIFO, 只能一端出一端入
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;
    
    SDL_LockMutex(q->mutex);
    
    for (;;) {
        if (global_video_state->quit) {
            ret = -1;
            break;
        }
        
        // first_pkt 就是出口
        pkt1 = q->first_pkt;
        if (pkt1) {
            // 更新first_pkt 为后一个入队列的元素
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            if (pkt->stream_index == global_video_state->audio_index){
                printf("get A, q->size = %d\n",q->size);
            } else if(pkt->stream_index == global_video_state->video_index){
                printf("get V, q->size = %d\n",q->size);
            }
            
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            // 等待packet_queue_put信号
            // unlock the mutex we give it and then attempt to lock it again once we get the signal
            // 执行SDL_CondWait时unlock, 等到信号之后又lock,所以不会因为死循环造成不能push进queue的情况
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    
    SDL_UnlockMutex(q->mutex);
    
    return ret;
}

static int audio_decode_frame(VideoState *is) {
    int len1, len2, decoded_data_size;
    AVPacket *pkt = &is->audio_pkt;
    int got_frame = 0;
    int64_t dec_channel_layout;
    int wanted_nb_samples, resampled_data_size;
    
    for (;;) {

        // 有包数据,进行解析
        // 一个音频AVPacket里面可能有多个音频AVFrame, 因此需要设置audio_pkt_size参数
        while (is->audio_pkt_size > 0) {
            if (!is->audio_frame) {
                if (!(is->audio_frame = av_frame_alloc())) {
                    return AVERROR(ENOMEM);
                }
            } else{
                // avcodec_get_frame_defaults(is->audio_frame);
                av_frame_unref(is->audio_frame);
            }
            
            // 发生错误是负数, 其他表示所用掉的pkt的大小
            len1 = avcodec_decode_audio4(is->audio_st->codec, is->audio_frame,
                                         &got_frame, pkt);
            if (len1 < 0) {
                // 发生错误, 跳过这个包, 直接获取队列里面的下个音频包
                is->audio_pkt_size = 0;
                break;
            }

            // 移动packet游标
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            
            if (!got_frame)
                continue;
            
            // 计算解码出来的桢需要的缓冲大小 size = channels(声道数) * nb_samples(一帧中每个声道的采样数) * 每个采样的大小
            decoded_data_size = av_samples_get_buffer_size(NULL,
                                                           is->audio_frame->channels, is->audio_frame->nb_samples,
                                                           is->audio_frame->format, 1);

            // 确定实际上的layout, 以audio_frame为准
            dec_channel_layout =
            (is->audio_frame->channel_layout && is->audio_frame->channels == av_get_channel_layout_nb_channels( is->audio_frame->channel_layout)) ?
            is->audio_frame->channel_layout :  av_get_default_channel_layout(is->audio_frame->channels);
            
            wanted_nb_samples = is->audio_frame->nb_samples;

            // 如果实际的audio_frame参数和SDL 所用的参数不一致, 需要重新采样, audio_frame -> target
            // 一开始src = target = SDL实际使用的参数spec
            if (is->audio_frame->format != is->audio_src_fmt
                || dec_channel_layout != is->audio_src_channel_layout
                || is->audio_frame->sample_rate != is->audio_src_freq
                || (wanted_nb_samples != is->audio_frame->nb_samples && !is->swr_ctx)) {
                    if (is->swr_ctx)
                        swr_free(&is->swr_ctx);

                    // 如果实际的audio_frame参数和SDL 所用的参数不一致, 需要重新采样, audio_frame -> target
                    is->swr_ctx = swr_alloc_set_opts(NULL,
                            is->audio_tgt_channel_layout, is->audio_tgt_fmt,is->audio_tgt_freq,
                            dec_channel_layout,is->audio_frame->format, is->audio_frame->sample_rate,
                                                     0, NULL);
                
                
                    if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
                        fprintf(stderr, "swr_init() failed\n");
                        break;
                    }

                    // 更新source参数
                    is->audio_src_channel_layout = dec_channel_layout;
                    is->audio_src_channels = is->audio_st->codec->channels;
                    is->audio_src_freq = is->audio_st->codec->sample_rate;
                    is->audio_src_fmt = is->audio_st->codec->sample_fmt;
                }
            
            /* 这里我们可以对采样数进行调整，增加或者减少，一般可以用来做声画同步 */
            if (is->swr_ctx) { // 需要重采样

                // AV_SAMPLE_FMT_S16, 非平面, l/r存在一起, extended_data = data[0]
                const uint8_t **in = (const uint8_t **) is->audio_frame->extended_data;
                uint8_t *out[] = { is->audio_buf2 };

                //TODO: nb_samples不一致; 为什么会不一致,上面有赋值啊??
                if (wanted_nb_samples != is->audio_frame->nb_samples) {

                    // TODO:这两个参数还是不知道怎么算出来的.......
                    // sample_delta :delta in PTS per sample
                    int sample_delta = (wanted_nb_samples - is->audio_frame->nb_samples) * is->audio_tgt_freq / is->audio_frame->sample_rate;
                    // compensation_distance :number of samples to compensate for
                    int compensation_distance = wanted_nb_samples * is->audio_tgt_freq / is->audio_frame->sample_rate;
                    if (swr_set_compensation(is->swr_ctx,sample_delta,compensation_distance) < 0) {
                        fprintf(stderr, "swr_set_compensation() failed\n");
                        break;
                    }

                }

                // number of samples output per channel
                int  out_nb_samples = sizeof(is->audio_buf2) / is->audio_tgt_channels / av_get_bytes_per_sample(is->audio_tgt_fmt);
                len2 = swr_convert(is->swr_ctx, out, out_nb_samples, in, is->audio_frame->nb_samples);

                if (len2 < 0) {
                    fprintf(stderr, "swr_convert() failed\n");
                    break;
                }

                // Note that the samples may get buffered in swr if you provide insufficient output space or if sample rate conversion is done, which requires "future" samples.
                if (len2  == sizeof(is->audio_buf2) / is->audio_tgt_channels / av_get_bytes_per_sample(is->audio_tgt_fmt)) {
                    fprintf(stderr, "warning: audio buffer is probably too small\n");
                    swr_init(is->swr_ctx);
                }
                is->audio_buf = is->audio_buf2;
                // TODO:现象, 有声音的时候会跑的很快, 没有声音的时候跑的很慢!!!!
                resampled_data_size = len2 * is->audio_tgt_channels * av_get_bytes_per_sample(is->audio_tgt_fmt);
                printf("audio_buf:%d  size: %d \n",*(is->audio_buf),resampled_data_size);

            } else { // 不需要重采样

                // 经过测试: 如果直接走这里, 音频会一直有, 但是参数有问题
                resampled_data_size = decoded_data_size;
                is->audio_buf = is->audio_frame->data[0];
            }

            // We have data, return it and come back for more later
            return resampled_data_size;
        }


        // 没有packet数据, 去队列获取, 队列里面没有,等待输入
        if (pkt->data) // 有数据,重置为0, 因为已经用过了
            av_free_packet(pkt);
        memset(pkt, 0, sizeof(*pkt));

        if (is->quit)
            return -1;

        if (packet_queue_get(&is->audioq, pkt, 1) < 0)
            return -1;
        
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
    }
    
    return 0;
}

/*
 * SDL调用的回调, 获取解码后的音频
 * 没有声音的时候, 回调明显变慢
 * */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = (VideoState *) userdata;
    int len1, audio_data_size;
    /*   len是由SDL传入的SDL缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */
    while (len > 0) {
        /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，
        这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我们的缓冲为空，
         没有数据可供copy，这时候需要调用audio_decode_frame来解码出更 多的桢数据 */
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_data_size = audio_decode_frame(is);
            /* audio_data_size < 0 标示没能解码出数据，我们默认播放静音 */
            if (audio_data_size < 0) {
                /* silence */
                is->audio_buf_size = 1024;
                /* 清零，静音 */
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_data_size;
            }
            is->audio_buf_index = 0;
        }

        /*  查看stream可用空间，决定一次copy多少数据，剩下的下次继续copy */
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }

        memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);

        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

/*
 * 计时器绑定方法, 发送FF_REFRESH_EVENT事件
 * */
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}

/*
 * 添加计时器
 * */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

/*
 * 判断是否终止阻塞操作回调
 * */
static int decode_interrupt_cb(void *opaque) {
    return (global_video_state && global_video_state->quit);
}


/*
 * 显示视频
 * */
static void video_display(VideoState *is) {
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    
    vp = &is->pictq[is->pictq_rindex];
    if (vp->texture) {
        if (is->video_st->codec->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio)
            * is->video_st->codec->width / is->video_st->codec->height;
        }
        
        if (aspect_ratio <= 0.0) {
            aspect_ratio = (float) is->video_st->codec->width
            / (float) is->video_st->codec->height;
        }
        
        rect.x = 0;
        rect.y = 0;
        rect.w = vp->width ;
        rect.h = vp->height ;
        
        SDL_UpdateYUVTexture(vp->texture, &rect, vp->rawdata->data[0],
                             vp->rawdata->linesize[0], vp->rawdata->data[1],
                             vp->rawdata->linesize[1], vp->rawdata->data[2],
                             vp->rawdata->linesize[2]);
        
        SDL_RenderClear(vp->renderer);
        SDL_RenderCopy(vp->renderer, vp->texture, &rect, &rect);
        SDL_RenderPresent(vp->renderer);
    }
}

/*
 * 计时器方法 -> 发送FF_REFRESH_EVENT事件 -> 响应方法显示视频,FF_REFRESH_EVENT响应事件
 * 控制视频流播放速度
 * */
static void video_refresh_timer(void *userdata) {
    VideoState *is = (VideoState *) userdata;
    //VideoPicture *vp;
    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            //vp = &is->pictq[is->pictq_rindex];
            /* Now, normally here goes a ton of code
             about timing, etc. we're just going to
             guess at a delay for now. You can
             increase and decrease this value and hard code
             the timing - but I don't suggest that ;)
             We'll learn how to do it for real later.
             */
            schedule_refresh(is, 50);
            
            /* show the picture! */
            video_display(is);
            
            /* update queue for next picture! */
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

/*
 * FF_ALLOC_EVENT响应方法
 * 初始化AVPicture来存储新数据
 * */
static void alloc_picture(void *userdata) {
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;
    
    vp = &is->pictq[is->pictq_windex];
    if (vp->texture) {
        // we already have one make another, bigger/smaller
        SDL_DestroyTexture(vp->texture);
    }
    
    if(vp->rawdata) {
        av_free(vp->rawdata);
    }
    
    // Allocate a place to put our YUV image on that screen
    vp->screen = SDL_CreateWindow("My Player Window", SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED, is->video_st->codec->width,
                                  is->video_st->codec->height,
                                    SDL_WINDOW_OPENGL);
    
    vp->renderer = SDL_CreateRenderer(vp->screen, -1, 0);
    vp->texture = SDL_CreateTexture(vp->renderer, SDL_PIXELFORMAT_YV12,
                                SDL_TEXTUREACCESS_STREAMING, is->video_st->codec->width, is->video_st->codec->height);
    
    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;
    
    
    AVFrame* pFrameYUV = av_frame_alloc();
    if (pFrameYUV == NULL)
        return;
    
    int numBytes = avpicture_get_size(AV_PIX_FMT_YUV420P, vp->width,
                                      vp->height);
    
    uint8_t* buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    
    avpicture_fill((AVPicture *) pFrameYUV, buffer, AV_PIX_FMT_YUV420P,
                   vp->width, vp->height);
    
    vp->rawdata = pFrameYUV;
    
    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;
    // 初始化之后,发送信号
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

/*
 * 加入视频缓存
 * */
 static int queue_picture(VideoState *is, AVFrame *pFrame) {
    VideoPicture *vp;
    //int dst_pic_fmt
    AVPicture pict;
    
    // 如果pictq里面已经有VideoPicture, 等待读取信号
    SDL_LockMutex(is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);
    
    if (is->quit)
        return -1;
    
    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];
    
    /* allocate or resize the buffer ! */
    // 重新分配VideoPicture
    if (!vp->texture || vp->width != is->video_st->codec->width
        || vp->height != is->video_st->codec->height) {
        SDL_Event event;
        
        vp->allocated = 0;
        /* we have to do it in the main thread */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
        
        /* wait until we have a picture allocated */
        // 等待创建VideoPicture信号
        SDL_LockMutex(is->pictq_mutex);
        while (!vp->allocated && !is->quit) {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if (is->quit) {
        return -1;
    }
    
    /* We have a place to put our picture on the queue */
    if (vp->rawdata) {
        // Convert the image into YUV format that SDL uses
        // 转换
        sws_scale(is->sws_ctx, (uint8_t const * const *) pFrame->data,
                  pFrame->linesize, 0, is->video_st->codec->height,
                  vp->rawdata->data, vp->rawdata->linesize);
        
        /* now we inform our display thread that we have a pic ready */
        if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

/*
 * 视频解码线程绑定 视频解码方法
 * arg: 全局的VideoState
 * */
static int video_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVPacket pkt1, *packet = &pkt1;
    int frameFinished;
    AVFrame *pFrame;
    
    pFrame = av_frame_alloc();
    
    for (;;) {
        if (packet_queue_get(&is->videoq, packet, 1) < 0) {
            // means we quit getting packets
            break;
        }
        
        // Decode video frame
        // 一个视频AVPacket 一般只有一个AVFrame
        avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished,
                              packet);
        
        // Did we get a video frame?
        if (frameFinished) {
            if (queue_picture(is, pFrame) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }
    
    av_free(pFrame);
    return 0;
}


/*
 * 打开音频设置
 * */
static int audio_stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    // 所需要的
    SDL_AudioSpec wanted_spec;
    int64_t wanted_channel_layout = 0;
    int wanted_nb_channels;

    // 实际上的
    SDL_AudioSpec  spec;

    /*  SDL支持的声道数为 1, 2, 4, 6 */
    /*  后面我们会使用这个数组来纠正不支持的声道数目 */
    const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
    
    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }
    
    codecCtx = ic->streams[stream_index]->codec;

    // 声道数
    // channels -> wanted_nb_channels -> channels
    wanted_nb_channels = codecCtx->channels;

    // wanted_channel_layout 操作
    if (!wanted_channel_layout
        || wanted_nb_channels != av_get_channel_layout_nb_channels( wanted_channel_layout)) {
            wanted_channel_layout = av_get_default_channel_layout( wanted_nb_channels);
            wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
        }
    wanted_spec.channels = av_get_channel_layout_nb_channels( wanted_channel_layout);

    wanted_spec.freq = codecCtx->sample_rate;

    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        fprintf(stderr, "Invalid sample rate or channel count!\n");
        return -1;
    }

    //Signed 16-bit samples, 大端序还是小端序有系统cpu决定, Mac是小端序
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;            // 0指示静音
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  // 自定义SDL缓冲区大小
    wanted_spec.callback = audio_callback;        // 音频解码的关键回调函数
    wanted_spec.userdata = is;                    // 传给上面回调函数的外带数据
    
    /*  打开音频设备，这里使用一个while来循环尝试打开不同的声道数(由上面 next_nb_channels数组指定）直到一个成功打开，或者全部失败*/
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n", wanted_spec.channels, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            fprintf(stderr,
                    "No more channel combinations to tyu, audio open failed\n");
            return -1;
        }
        wanted_channel_layout = av_get_default_channel_layout( wanted_spec.channels);
    }
    
    /* 检查实际使用的配置（保存在spec,由SDL_OpenAudio()填充） */
    if (spec.format != AUDIO_S16SYS) {
        fprintf(stderr, "SDL advised audio format %d is not supported!\n",
                spec.format);
        return -1;
    }

    // 以实际的为准
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            fprintf(stderr, "SDL advised channel count %d is not supported!\n",
                    spec.channels);
            return -1;
        }
    }
    
    /* 把设置好的参数保存到大结构中 */
    // AV_SAMPLE_FMT_*的字节序是native-endian order, 就是运行的cpu的字节序
    // FFmpeg 的 AV_SAMPLE_FMT_S16 对应 SDL 的 AUDIO_S16SYS
    is->audio_src_fmt = is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
    is->audio_src_freq = is->audio_tgt_freq = spec.freq;
    is->audio_src_channel_layout = is->audio_tgt_channel_layout = wanted_channel_layout;
    is->audio_src_channels = is->audio_tgt_channels = spec.channels;
    
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_index = stream_index;
            is->audio_st = ic->streams[stream_index];
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0); // 开始播放静音
            break;
        default:
            break;
    }
    
    return 0;
}

/*
 * 打开视频设置
 * */
static int video_stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->ic;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    
    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }
    
    // Get a pointer to the codec context for the video stream
    codecCtx = pFormatCtx->streams[stream_index]->codec;
    
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            is->video_index = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            is->sws_ctx = sws_getContext(is->video_st->codec->width,
                                         is->video_st->codec->height, is->video_st->codec->pix_fmt,
                                         is->video_st->codec->width, is->video_st->codec->height,
                                         AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
            break;
        default:
            break;
    }
    return 0;
}

static int decode_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFormatContext *pFormatCtx = NULL;

    // packet ---> pkt1
    AVPacket pkt1, *packet = &pkt1;
    
    int video_index = -1;
    int audio_index = -1;
    int i;
    
    is->video_index = -1;
    is->audio_index = -1;
    
    /*当ffmpeg遇到阻塞操作是, 会不断调用AVIOInterruptCB来判断是否要终止当前阻塞
      AVIOInterruptCB 返回 1, 终止阻塞操作, 返回AVERROR_EXIT
     详细看:www.bubuko.com/infodetail-733781.html
     */
    AVIOInterruptCB interupt_cb;
    // 把传入的VideoState赋值给全局VideoState
    global_video_state = is;
    interupt_cb.callback = decode_interrupt_cb;
    interupt_cb.opaque = is;
    
    // 初始化io_ctx
    if (avio_open2(&is->io_ctx, is->filename, 0, &interupt_cb, NULL)) {
        fprintf(stderr, "Cannot open I/O for %s\n", is->filename);
        return -1;
    }
    
    // 打开文件, 初始化pFormatCtx
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
        return -1; //Couldn't open file
    }
    is->ic = pFormatCtx;
    
    // Retrieve stream infomation
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        return -1; // Couldn't find stream information
    }
    
    // 打印信息
    av_dump_format(pFormatCtx, 0, is->filename, 0);
    
    // 获取 video_index audio_index
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->coder_type == AVMEDIA_TYPE_VIDEO
            && video_index < 0) {
            video_index = i;
        }
        
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
            && audio_index < 0) {
            audio_index = i;
        }
    }
    
    if (audio_index >= 0) {
        /* 所有设置SDL音频流信息的步骤都在这个函数里完成 */
        audio_stream_component_open(is, audio_index);
    }
    
    if (video_index >= 0) {
        video_stream_component_open(is, video_index);
    }
    
    if (is->video_index < 0 || is->audio_index <= 0) {
        fprintf(stderr, "%s: could not open codec\n", is->filename);
        goto fail;
    }
    
    //main decode loop
    /* 读包的主循环， av_read_frame不停的从文件中读取数据包*/
    for (;;) {
        if (is->quit) {
            break;
        }

        //seek  stuff goes here
        //如果有足够的数据解码后的数据,  packet不用读取那么快
        if (is->audioq.size > MAX_AUDIO_SIZE ) {
            SDL_Delay(10);
            printf("is->audioq.size > MAX_AUDIO_SIZE\n");
            continue;
        }

        // 瓶颈这里!!
        // 因为视频流播放有interval 刷新率限制, 导致视频消耗量总是远远少于音频量.
        // 音频播放的很快,快于视频播放, 而且视频缓存很快就会满
//        if ( is->videoq.size > MAX_VIDEO_SIZE) {
//            SDL_Delay(10);
//            printf("is->videoq.size > MAX_VIDEO_SIZE\n");
//            continue;
//        }

        // 读包
        if (av_read_frame(is->ic, packet) < 0) {
            if (is->ic->pb->error == 0) {// no error; wait for user input 或者直接break
                 SDL_Delay(100);
                 continue;
            } else { // error
                break;
            }
        }

         // Is this a packet from the video stream?
        if (packet->stream_index == is->video_index) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audio_index) {
            // 一直有音频包
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
    
    /*all done - wait for it*/
    while (!is->quit) {
        SDL_Delay(100);
    }
    
fail: if (1) {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
}
    return 0;
}


int play_media_with_threads(char* url){
    //char *filename =  "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/AS.mp4"; //argv[1];
    SDL_Event event;
    
    VideoState *is;
    is = av_malloc(sizeof(VideoState));
    
    // Register all formats and codecs
    av_register_all();
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    
    av_strlcpy(is->filename, url, sizeof(is->filename));
    
    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();
    
    // 每间隔40毫秒 发送一次FF_REFRESH_EVENT事件, 参数是VideoState
    schedule_refresh(is, 40);
    
    // 创建解码线程, 绑定解码方法decode_thread, 参数是VideoState
    is->parse_tid = SDL_CreateThread(decode_thread, "parse_thread", is);
    if (!is->parse_tid) {
        av_free(is);
        return -1;
    }
    
    // 事件处理
    for (;;) {
        SDL_WaitEvent(&event);
        switch (event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                SDL_CondSignal(is->audioq.cond);
                SDL_CondSignal(is->videoq.cond);
                is->quit = 1;
                SDL_Quit();
                return 0;
                break;
            case FF_ALLOC_EVENT:
                alloc_picture(event.user.data1);
                break;
                
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
        }
    }
    
    return 0;
}

