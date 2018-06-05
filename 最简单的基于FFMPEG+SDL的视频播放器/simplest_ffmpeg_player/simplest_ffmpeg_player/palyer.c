//
//  palyer.c
//  simplest_ffmpeg_player
//
//  Created by DengXiaoBai on 2018/4/24.
//  Copyright © 2018年 angle. All rights reserved.
//

#include <stdio.h>
#include "avcodec.h"
#include "avformat.h"
#include "swscale.h"
#include "imgutils.h"
#include "SDL.h"
#include "swresample.h"

//Refresh Event
#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)
#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)
int thread_exit=0;
int thread_pause=0;

int sfp_refresh_thread(void *opaque){
    thread_exit=0;
    thread_pause=0;
    
    while (!thread_exit) {
        if(!thread_pause){
            SDL_Event event;
            event.type = SFM_REFRESH_EVENT;
            SDL_PushEvent(&event);
        }
        SDL_Delay(40);
    }
    thread_exit=0;
    thread_pause=0;
    //Break
    SDL_Event event;
    event.type = SFM_BREAK_EVENT;
    SDL_PushEvent(&event);
    
    return 0;
}



int paly(char* media_url){
    AVFormatContext *pFormatCtx;
    
    // 视频
    int             i, videoindex;
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    AVFrame *pFrame,*pFrameYUV;
    unsigned char *out_buffer;
    AVPacket *packet;
    int ret, got_picture;
    struct SwsContext *img_convert_ctx;
    
    //SDL
    int screen_w,screen_h;
    SDL_Window *screen;
    SDL_Renderer* sdlRenderer;
    SDL_Texture* sdlTexture;
    SDL_Rect sdlRect;
    SDL_Rect sdlRect1;
    SDL_Rect sdlRect2;
    SDL_Rect sdlRect3;
    SDL_Thread *video_tid;
    SDL_Event event;
    
    
    //char filepath[]= "rtmp://localhost:1935/rtmplive";
    // char filepath[]= "/Users/stringstech-macmini1/Desktop/AV_Study/FFmpeg_Leixiaohua-master/最简单的基于FFMPEG+SDL的视频播放器/res/Titanic.ts";

    
    //!--------------- 1. 初始化组件 ----------
    // 组件初始化
    av_register_all();
    // 网络初始化
    avformat_network_init();
    // 定义上下文
    pFormatCtx = avformat_alloc_context();
    
    //!--------------- 2. 打开输入文件 ----------
    if(avformat_open_input(&pFormatCtx,media_url,NULL,NULL)!=0){
        printf("Couldn't open input stream.\n");
        return -1;
    }
    
    //!--------------- 3. Read packets of a media file to get stream information. ----------
    if(avformat_find_stream_info(pFormatCtx,NULL)<0){
        printf("Couldn't find stream information.\n");
        return -1;
    }
    
    
    
    //!--------------- 4. Find the right decoder for video----------
    videoindex=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++){
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO){
            videoindex=i;
            break;
        }
    }
    
    if(videoindex==-1){
        printf("Didn't find a video stream.\n");
        return -1;
    }
 
    
    
    pCodecCtx=pFormatCtx->streams[videoindex]->codec;
    
    pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec==NULL){
        printf("Codec not found.\n");
        return -1;
    }
    
    //!--------------- 5. OPEN the right decoder for video----------
    if(avcodec_open2(pCodecCtx, pCodec,NULL)<0){
        printf("Could not open codec.\n");
        return -1;
    }
    
    
    // 用来保存原始的frame
    pFrame=av_frame_alloc();
    // 用来保存转换成YUV的frame
    pFrameYUV=av_frame_alloc();
    // 计算AV_PIX_FMT_YUV420P 格式图片的大小
    // TODO:最后的那个1是什么鬼东西??
    out_buffer=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P,  pCodecCtx->width, pCodecCtx->height,1));
    // 初始化pFrameYUV参数
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize,out_buffer,
                         AV_PIX_FMT_YUV420P,pCodecCtx->width, pCodecCtx->height,1);
    
    
    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
                                     pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    
    packet=(AVPacket *)av_malloc(sizeof(AVPacket));
    

    // 初始化SDL
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        printf( "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    
    // 创建Window
    //SDL 2.0 Support for multiple windows
    screen_w = pCodecCtx->width / 2 ;
    screen_h = pCodecCtx->height / 2;
    screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              screen_w , screen_h ,SDL_WINDOW_OPENGL);
    
    if(!screen) {
        printf("SDL: could not create window - exiting:%s\n",SDL_GetError());
        return -1;
    }
    // 创建Renderer
    sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    
    // 创建Texture
    //IYUV: Y + U + V  (3 planes)
    //YV12: Y + V + U  (3 planes)
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,pCodecCtx->width,pCodecCtx->height);
    
    // 可以有多个Rect在Window上
    sdlRect.x=0;
    sdlRect.y=0;
    sdlRect.w=screen_w;
    sdlRect.h=screen_h;
    
    //    sdlRect1.x=screen_w;
    //    sdlRect1.y=0;
    //    sdlRect1.w=screen_w;
    //    sdlRect1.h=screen_h;
    //
    //    sdlRect2.x=0;
    //    sdlRect2.y=screen_h;
    //    sdlRect2.w=screen_w;
    //    sdlRect2.h=screen_h;
    //
    //    sdlRect3.x=screen_w;
    //    sdlRect3.y=screen_h;
    //    sdlRect3.w=screen_w;
    //    sdlRect3.h=screen_h;
    
  
    // 创建线程监听事件
    video_tid = SDL_CreateThread(sfp_refresh_thread,NULL,NULL);
    
    //------------SDL End------------
    // 文件写入
    // FILE* yuvfp = fopen("/Users/stringstech-macmini1/Desktop/AV_Study/media_src/YUVtest.yuv", "wb+");
    //FILE* h264fp = fopen("/Users/stringstech-macmini1/Desktop/AV_Study/media_src/h264test.h264", "wb+");
    
    int y_size = pCodecCtx->height * pCodecCtx->width;
    
    // Event Loop 死循环等待事件驱动
    for (;;) {
        
        //Wait
        SDL_WaitEvent(&event);
        
        if(event.type==SFM_REFRESH_EVENT){
            
            //!--------------- 6. 读取packet --> 解码成YUV ----------
            while (1) {
                // packet里面装的是264码流
                if (av_read_frame(pFormatCtx, packet) < 0) {
                    thread_exit = 1;
                }
                
                if (packet->stream_index == videoindex) {
                    break;
                }
            }
            
            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
            if(ret < 0){
                printf("Decode Error.\n");
                return -1;
            }
            
            if(got_picture){
                sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
                //printf("显示frame序号 : %d ,编码frame序号 : %d\n",pFrame->display_picture_number,pFrame->coded_picture_number);
                //printf("pkt_duration : %lld, pts: %lld , best_pts: %lld\n",pFrame->pkt_duration,pFrame->pts,pFrame->best_effort_timestamp);
                
                
                //                fwrite(packet->data, packet->size, 1, h264fp);
                //                fwrite(pFrameYUV->data[0], y_size, 1, yuvfp);
                //                fwrite(pFrameYUV->data[1], y_size/4, 1, yuvfp);
                //                fwrite(pFrameYUV->data[2], y_size/4, 1, yuvfp);
                
                
                // SDL---------------------------
                
                SDL_UpdateTexture( sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
                
                // TODO: 打印出来并没有差别
                //                printf("pFrame linesize---Y: %d, U: %d, V: %d\n",pFrame->linesize[0],pFrame->linesize[1],pFrame->linesize[2]);
                //                printf("pFrameYUV linesize---Y: %d, U: %d, V: %d\n",pFrameYUV->linesize[0],pFrameYUV->linesize[1],pFrameYUV->linesize[2]);
                SDL_RenderClear( sdlRenderer );
                SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect);
                //                SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect1);
                //                SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect2);
                //                SDL_RenderCopy( sdlRenderer, sdlTexture, NULL, &sdlRect3);
                // 展示
                SDL_RenderPresent( sdlRenderer);
                //SDL End-----------------------
            }
            
            // packet 复位
            av_packet_unref(packet);
  
        }else if(event.type==SDL_KEYDOWN){
            //Pause
            if(event.key.keysym.sym==SDLK_SPACE)
                thread_pause=!thread_pause;
        }else if(event.type==SDL_QUIT){
            thread_exit=1;
        }else if(event.type==SFM_BREAK_EVENT){
            break;
        }
        
    }
    
    sws_freeContext(img_convert_ctx);
  
    SDL_CloseAudio();//Close SDL
    SDL_Quit();
    

    //fclose(h264fp);
    //fclose(yuvfp);
    
    av_frame_free(&pFrameYUV);
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
    
    return 0;
}



