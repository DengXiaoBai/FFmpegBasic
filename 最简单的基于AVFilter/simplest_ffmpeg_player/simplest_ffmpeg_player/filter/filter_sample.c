//
//  filter_sample.c
//  HelloWorld
//
//  Created by DengXiaoBai on 2018/5/21.
//  Copyright © 2018年 angle. All rights reserved.
//

/* 播放之后是绿色的
 * 原因: 没有转换frame, 直接播放了
 * */
#include <stdio.h>

#include "avcodec.h"
#include "avformat.h"
#include "avfiltergraph.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "avutil.h"
#include "swscale.h"
#include "SDL.h"
#include "imgutils.h"

//Enable SDL?
#define ENABLE_SDL 1
//Output YUV data?
#define ENABLE_YUVFILE 0

// 解码-> filter -> 编码 -> 推流, see streamer_filter.c

// const char *filter_descr = "movie=/Users/stringstech-macmini1/Desktop/AV_Study/media_src/leishen_logo.png[wm];[in][wm]overlay=5:5[out]";
// const char *filter_descr = "split [main][tmp]; [tmp] crop=iw:ih/2:0:0, vflip [flip]; [main][flip] overlay=0:H/2";
const char *filter_descr = "curves=enable='gte(t,3)':preset=cross_process";

static AVFormatContext *pFormatCtx;
static AVCodecContext *pCodecCtx;
AVFilterContext *buffersrc_ctx;
AVFilterContext *buffersink_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;

static int open_input_file(const char *filename){
    int ret;
    AVCodec *dec;
    
    if ((ret = avformat_open_input(&pFormatCtx, filename, NULL, NULL)) < 0) {
        printf( "Cannot open input file\n");
        return ret;
    }
    
    if ((ret = avformat_find_stream_info(pFormatCtx, NULL)) < 0) {
        printf( "Cannot find stream information\n");
        return ret;
    }
    
    /* select the video stream */
    ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        printf( "Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;
    pCodecCtx = pFormatCtx->streams[video_stream_index]->codec;
    
    /* init the video decoder */
    if ((ret = avcodec_open2(pCodecCtx, dec, NULL)) < 0) {
        printf( "Cannot open video decoder\n");
        return ret;
    }
    
    return 0;
}

static int init_filters(const char *filters_descr){
    char args[512];
    int ret;
    AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("buffersink");

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    // 支持的格式
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    AVBufferSinkParams *buffersink_params;
    
    filter_graph = avfilter_graph_alloc();
    
    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    // 构建format string
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
             pCodecCtx->time_base.num, pCodecCtx->time_base.den,
             pCodecCtx->sample_aspect_ratio.num, pCodecCtx->sample_aspect_ratio.den);

    // 创建buffersrc_ctx, 并添加到filter_graph中
    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        printf("Cannot create buffer source\n");
        return ret;
    }
    
    /* buffer video sink: to terminate the filter chain. */
    buffersink_params = av_buffersink_params_alloc();
    buffersink_params->pixel_fmts = pix_fmts;

    // 创建buffersink_ctx, 并添加到filter_graph中
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, buffersink_params, filter_graph);
    av_free(buffersink_params);
    if (ret < 0) {
        printf("Cannot create buffer sink\n");
        return ret;
    }
    
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;
    
    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        return ret;
    
    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;
    return 0;
}

int video_watermark(char *file){
    int ret;
    AVPacket packet;
    int got_frame;
    AVFrame *pFrame, *pFrame_out, *pFrameYUV;

    unsigned char *out_buffer;
    struct SwsContext *img_convert_ctx;


    av_register_all();
    avfilter_register_all();
    
    if ((ret = open_input_file(file)) < 0)
        goto end;

    if ((ret = init_filters(filter_descr)) < 0)
        goto end;
    
#if ENABLE_YUVFILE
    FILE *fp_yuv=fopen("test.yuv","wb+");
#endif

#if ENABLE_SDL
    int screen_w,screen_h;
    SDL_Window *screen;
    SDL_Renderer* sdlRenderer;
    SDL_Texture* sdlTexture;
    
    SDL_Rect sdlRect;
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        printf( "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    
    screen_w = pCodecCtx->width ;
    screen_h = pCodecCtx->height;
    screen = SDL_CreateWindow("AVFILTER", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              screen_w , screen_h ,SDL_WINDOW_OPENGL);
    if(!screen) {
        printf("SDL: could not set video mode - exiting\n");
        return -1;
    }
    
    // 创建Renderer
    sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    
    // 创建Texture
    //IYUV: Y + U + V  (3 planes)
    //YV12: Y + V + U  (3 planes)
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,pCodecCtx->width,pCodecCtx->height);
    
    sdlRect.x=0;
    sdlRect.y=0;
    sdlRect.w=screen_w;
    sdlRect.h=screen_h;
#endif
    // 解码后的frame
    pFrame=av_frame_alloc();
    // 从sink获取的frame
    pFrame_out=av_frame_alloc();
    // 转换后的frame
    pFrameYUV=av_frame_alloc();

    // 计算AV_PIX_FMT_YUV420P 格式图片的大小
    out_buffer=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P,  pCodecCtx->width, pCodecCtx->height,1));
    // 初始化pFrameYUV参数
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize,out_buffer,
            AV_PIX_FMT_YUV420P,pCodecCtx->width, pCodecCtx->height,1);

    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
            pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    
    /* read all packets */
    while (1) {
        
        ret = av_read_frame(pFormatCtx, &packet);
        if (ret< 0)
            break;
        
        if (packet.stream_index == video_stream_index) {
            got_frame = 0;
            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_frame, &packet);
            if (ret < 0) {
                printf( "Error decoding video\n");
                break;
            }
            
            if (got_frame) {
                pFrame->pts = av_frame_get_best_effort_timestamp(pFrame);
                
                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame(buffersrc_ctx, pFrame) < 0) {
                    printf( "Error while feeding the filtergraph\n");
                    break;
                }
                
                /* pull filtered pictures from the filtergraph */
                while (1) {
                    
                    ret = av_buffersink_get_frame(buffersink_ctx, pFrame_out);
                    if (ret < 0)
                        break;

                    //FIXME: 内部会自动encode? 怎么获取到编码后的数据

                    
                    printf("Process 1 frame!\n");
                    
                    if (pFrame_out->format==AV_PIX_FMT_YUV420P) {
#if ENABLE_YUVFILE
                        //Y, U, V
                        for(int i=0;i<pFrame_out->height;i++){
                            fwrite(pFrame_out->data[0]+pFrame_out->linesize[0]*i,1,pFrame_out->width,fp_yuv);
                        }
                        for(int i=0;i<pFrame_out->height/2;i++){
                            fwrite(pFrame_out->data[1]+pFrame_out->linesize[1]*i,1,pFrame_out->width/2,fp_yuv);
                        }
                        for(int i=0;i<pFrame_out->height/2;i++){
                            fwrite(pFrame_out->data[2]+pFrame_out->linesize[2]*i,1,pFrame_out->width/2,fp_yuv);
                        }
#endif
#if ENABLE_SDL
                        sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame_out->data, pFrame_out->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);

                        SDL_UpdateTexture( sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
                        SDL_RenderClear( sdlRenderer );
                        SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect);
                        SDL_RenderPresent( sdlRenderer);
                        //Delay 40ms
                        SDL_Delay(40);
#endif
                    }
                    av_frame_unref(pFrame_out);
                }
            }
            av_frame_unref(pFrame);
        }
        av_free_packet(&packet);
    }
    
#if ENABLE_YUVFILE
    fclose(fp_yuv);
#endif
    
end:
    avfilter_graph_free(&filter_graph);
    if (pCodecCtx)
        avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
    
    
    if (ret < 0 && ret != AVERROR_EOF) {
        char buf[1024];
        av_strerror(ret, buf, sizeof(buf));
        printf("Error occurred: %s\n", buf);
        return -1;
    }
    
    return 0;
}
