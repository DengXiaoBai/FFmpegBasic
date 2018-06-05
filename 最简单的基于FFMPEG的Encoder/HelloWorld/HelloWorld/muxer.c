//
//  muxer.c
//  HelloWorld
//
//  Created by DengXiaoBai on 2018/4/23.
//  Copyright © 2018年 angle. All rights reserved.
//

#include <stdio.h>
#include "avformat.h"
#include "time.h"

/*
 FIX: H.264 in some container format (FLV, MP4, MKV etc.) need
 "h264_mp4toannexb" bitstream filter (BSF)
 *Add SPS,PPS in front of IDR frame
 *Add start code ("0,0,0,1") in front of NALU
 H.264 in some container (MPEG2TS) don't need this BSF.
 */
//'1': Use H.264 Bitstream Filter
#define USE_H264BSF 0

/*
 FIX:AAC in some container format (FLV, MP4, MKV etc.) need
 "aac_adtstoasc" bitstream filter (BSF)
 */
//'1': Use AAC Bitstream Filter
#define USE_AACBSF 0


int mix_video_audio_to_mp4(char* video_url, char* audio_url, char*output_url,int push_to_server){
    AVOutputFormat *ofmt = NULL;
    //Input AVFormatContext and Output AVFormatContext
    AVFormatContext *ifmt_ctx_v = NULL, *ifmt_ctx_a = NULL,*ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i, decode_ret;
    int videoindex_v=-1,videoindex_out=-1;
    int audioindex_a=-1,audioindex_out=-1;
    int frame_index=0;
    int64_t cur_pts_v=0,cur_pts_a=0;
    
    int64_t start_time=0;
    
    
    // FOR TEST
    int printed = 0;
    int vedio_frame_index = 0;
    AVFrame *pFrame = av_frame_alloc();
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    int got_picture ;
    char *frame_name = "NOT FOUND";
    char *media_type = "VEDIO";

    int64_t  seek_time = 0;
    int64_t  seek_pts = 0;
    int64_t  seek_dts = 0;
    
    
    //const char *in_filename_v = "cuc_ieschool.ts";//Input file URL
    const char *in_filename_v = video_url;
    
    //const char *in_filename_a = "cuc_ieschool.mp3";
    //const char *in_filename_a = "gowest.m4a";
    //const char *in_filename_a = "gowest.aac";
    const char *in_filename_a = audio_url;
    
    const char *out_filename = output_url ;//"cuc_ieschool.mp4";//Output file URL
    av_register_all();
    avformat_network_init();
    
    //Input
    if ((ret = avformat_open_input(&ifmt_ctx_v, in_filename_v, 0, 0)) < 0) {
        printf( "Could not open input file.");
        goto end;
    }
    
    if ((ret = avformat_find_stream_info(ifmt_ctx_v, 0)) < 0) {
        printf( "Failed to retrieve input stream information");
        goto end;
    }
    
    if ((ret = avformat_open_input(&ifmt_ctx_a, in_filename_a, 0, 0)) < 0) {
        printf( "Could not open input file.");
        goto end;
    }
    if ((ret = avformat_find_stream_info(ifmt_ctx_a, 0)) < 0) {
        printf( "Failed to retrieve input stream information");
        goto end;
    }
    
    
   printf("===========Input Information==========\n");
    av_dump_format(ifmt_ctx_v, 0, in_filename_v, 0);
    av_dump_format(ifmt_ctx_a, 0, in_filename_a, 0);
    printf("======================================\n");


    
    //Output
    if (push_to_server) {
        out_filename = "rtmp://localhost:1935/rtmplive";
        avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_filename);
    }else{
        avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    }
    
    if (!ofmt_ctx) {
        printf( "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ofmt = ofmt_ctx->oformat;
    
    for (i = 0; i < ifmt_ctx_v->nb_streams; i++) {
        //Create output AVStream according to input AVStream
        if(ifmt_ctx_v->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO){
            AVStream *in_stream = ifmt_ctx_v->streams[i];
            AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
            videoindex_v=i;
            if (!out_stream) {
                printf( "Failed allocating output stream\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }
            videoindex_out=out_stream->index;
            //Copy the settings of AVCodecContext
            if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
                printf( "Failed to copy context from input to output stream codec context\n");
                goto end;
            }
            
            out_stream->codec->codec_tag = 0;
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
            break;
        }
    }
    
    for (i = 0; i < ifmt_ctx_a->nb_streams; i++) {
        //Create output AVStream according to input AVStream
        if(ifmt_ctx_a->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO){
            AVStream *in_stream = ifmt_ctx_a->streams[i];
            AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
            audioindex_a=i;
            if (!out_stream) {
                printf( "Failed allocating output stream\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }
            audioindex_out=out_stream->index;
            //Copy the settings of AVCodecContext
            if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
                printf( "Failed to copy context from input to output stream codec context\n");
                goto end;
            }
            
            out_stream->codec->codec_tag = 0;
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
            
            break;
        }
    }
    
    // --------JUST FOR TEST -----------//
    if (( ifmt_ctx_v != NULL) && (videoindex_v != -1)){
        pCodecCtx=ifmt_ctx_v->streams[videoindex_v]->codec;
        pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
        if(pCodec==NULL){
            printf("Codec not found.\n");
            return -1;
        }
    }

    if(avcodec_open2(pCodecCtx, pCodec,NULL)<0){
        printf("Could not open codec.\n");
        return -1;
    }
    // --------JUST FOR TEST -----------//

    
    
    printf("==========Output Information==========\n");
    av_dump_format(ofmt_ctx, 0, out_filename, 1);
    printf("======================================\n");
    
    //Open output file
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE) < 0) {
            printf( "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    
    //FIX
#if USE_H264BSF
    AVBitStreamFilterContext* h264bsfc =  av_bitstream_filter_init("h264_mp4toannexb");
#endif
    
#if USE_AACBSF
    AVBitStreamFilterContext* aacbsfc =  av_bitstream_filter_init("aac_adtstoasc");
#endif
    
    // 微秒 相当于时间戳A
    start_time=av_gettime();

    //Write file header
    if (avformat_write_header(ofmt_ctx, NULL) < 0) {
        printf( "Error occurred when opening output file\n");
        goto end;
    }

    // 略过前面的10s
    AVRational time_base_v = ifmt_ctx_v->streams[videoindex_v]->time_base;
    seek_time = 10 / av_q2d(time_base_v) ;
    // Seek to the keyframe at timestamp.
    // 这个时间戳指的是什么? 是seek到的那个packet的pts/dts?
    cur_pts_v = seek_time;
    seek_dts = seek_time;
    seek_pts = seek_time;
    int seek_ret = av_seek_frame(ifmt_ctx_v, videoindex_v, seek_time , AVSEEK_FLAG_ANY);
    if (seek_ret < 0) {
        printf("av_seek_frame FAILED!!!!");
    }

    // 赋值cur_pts_v
//    //TODO: 改成这样的话, 会先推10s的视频,然后退音频!!!!
//    if (av_read_frame(ifmt_ctx_v, &pkt) >=0){
//        cur_pts_v = pkt.pts;
//        seek_pts = pkt.pts;
//        seek_dts = pkt.dts;
//    }

    while (1) {
        AVFormatContext *ifmt_ctx;
        int stream_index=0;
        AVStream *in_stream, *out_stream;

        //printf("cur_pts_v = %ld,  cur_pts_a = %ld \n",cur_pts_v,cur_pts_a);
        //Get an AVPacket
        // 视频快进10s, seek_pts快进的ts
        // TODO: 播放的时候找不到视频流!!!!
        if(av_compare_ts((cur_pts_v - seek_pts ),ifmt_ctx_v->streams[videoindex_v]->time_base,
                cur_pts_a,ifmt_ctx_a->streams[audioindex_a]->time_base) <= 0)
        { // 写入视频
            
            ifmt_ctx=ifmt_ctx_v;
            stream_index=videoindex_out;
            
            // 满足条件的话,保证读取到packet才退出循环
            if(av_read_frame(ifmt_ctx, &pkt) >= 0){
                do{
                    in_stream  = ifmt_ctx->streams[pkt.stream_index];
                    out_stream = ofmt_ctx->streams[stream_index];
                    
                    if(pkt.stream_index==videoindex_v){
                        
                        //FIX：No PTS (Example: Raw H.264)
                        //Simple Write PTS
                        if(pkt.pts==AV_NOPTS_VALUE){
                            //Write PTS
                            AVRational time_base1=in_stream->time_base;
                            
                            /*
                             // 帧率
                            fps = q(r_frame_rate) = q(avg_frame_rate)
                             
                             // 秒为单位
                            calc_duration  = 1 / fps
                            pts_time = pts * q(time_base) = (frame_index * calc_duration)
                            dts_time = dts * q(time_base) = (frame_index * calc_duration)
                             
                            //pts / dts / duration单位是时间基, 不是简单的毫秒
                            pts = (frame_index * calc_duration) / q(time_base)
                            duration = calc_duration / q(time_base)
                            dts = pts
                            pts_diff = (frame_index + 1) *calc_duration / q(time_base) - frame_index *calc_duration / q(time_base)
                                     = calc_duration / q(time_base) = 1 / (q(time_base)*q(r_frame_rate) )
                             */
                            //Duration between 2 frames (us) 一帧的长度
                            // 可能是为了精度, 先xAV_TIME_BASE后除
                            int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);
                            pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            pkt.dts=pkt.pts;
                            
                            pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            frame_index++;
                        }

                        cur_pts_v=pkt.pts;
                        vedio_frame_index++;

                        //=============== JUST FOR TEST =============//
                        if (!printed) {
                            printed = 1;
                            printf("--------VEDIO INPUT STREAM TIMESTAMPS------------ \n");
                            // fps = r_frame_rate.num / r_frame_rate.den = avg_frame_rate.num / avg_frame_rate.den
                            printf("AVStream->avg_frame_rate : %d/%d \n", in_stream->avg_frame_rate.num, in_stream->avg_frame_rate.den);
                            printf("AVStream->r_frame_rate : %d/%d \n", in_stream->r_frame_rate.num, in_stream->r_frame_rate.den);
                            printf("AVStream->time_base  : %d/%d \n", in_stream->time_base.num, in_stream->time_base.den);
                            printf("PTS will increase: %f \n",(float)((in_stream->time_base.den * in_stream->r_frame_rate.den) /
                                    (in_stream->time_base.num * in_stream->r_frame_rate.num)) );
                            printf("--------VEDIO INPUT STREAM TIMESTAMPS------------ \n");
                        }

                        decode_ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, &pkt);
                        if(decode_ret < 0){
                            printf("Decode Error.\n");
                        }

                        if (pFrame->pict_type == 0) {
                            frame_name = "NONE";
                        }else if (pFrame->pict_type == 1){
                            frame_name = "I";
                        }else if (pFrame->pict_type == 2){
                            frame_name = "P";
                        }else if (pFrame->pict_type == 3){
                            frame_name = "B";
                        }

                        printf("INPUT %d video Packet.   key_frame: %d  type: %s    pts:%lld   dts:%lld    DTS_TIME: %f     PTS_TIME: %f\n",
                                vedio_frame_index ,
                                pFrame->key_frame,
                                frame_name,
                                pkt.pts,
                                pkt.dts,
                                (double)pkt.dts * av_q2d(in_stream->time_base),
                                (double)pkt.pts * av_q2d(in_stream->time_base));

                        //=============== JUST FOR TEST =============//


                        /*发送流媒体的数据的时候需要延时。不然的话，FFmpeg处理数据速度很快，瞬间就能把所有的数据发送出去，流媒体服务器是接受不了的。
                         因此需要按照视频实际的帧率发送数据。
                         */
                        if (push_to_server){
                            
                            AVRational time_base=in_stream->time_base;
                            // pkt.dts * time_base / time_base_q , 转成微秒
                            int64_t dts_time = av_rescale_q(pkt.dts, time_base, AV_TIME_BASE_Q);
                            int64_t seeked_time = av_rescale_q(seek_dts, time_base, AV_TIME_BASE_Q);
                            
                            // 相当于手动计算的dts, 实际上的dts
                            int64_t now_time = av_gettime() - start_time;
                            if ( (dts_time - seeked_time) > now_time){
                                printf("VEDIO av_usleep time : %ld, pkt.dts = %ld,  seek_dts = %ld  \n",dts_time - seeked_time - now_time, pkt.dts,seek_dts );
                                av_usleep(dts_time - seeked_time - now_time);
                            }
                           
                        }

                        break;
                    }
                }while(av_read_frame(ifmt_ctx, &pkt) >= 0);
            }else{
                break;
            }
            
        }else{ // 写入音频
            
            ifmt_ctx=ifmt_ctx_a;
            stream_index=audioindex_out;
            
            if(av_read_frame(ifmt_ctx, &pkt) >= 0){
                do{
                    in_stream  = ifmt_ctx->streams[pkt.stream_index];
                    out_stream = ofmt_ctx->streams[stream_index];
                    
                    if(pkt.stream_index==audioindex_a){
                        
                        //FIX：No PTS
                        //Simple Write PTS
                        if(pkt.pts==AV_NOPTS_VALUE){
                            //Write PTS
                            AVRational time_base1=in_stream->time_base;
                            //Duration between 2 frames (us)
                            int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);
                            
                            //Parameters
                            pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            pkt.dts=pkt.pts;
                            pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            frame_index++;
                        }
                        cur_pts_a=pkt.pts;
                        break;
                    }
                }while(av_read_frame(ifmt_ctx, &pkt) >= 0);
            }else{
                break;
            }
            
        }
        
        //FIX:Bitstream Filter
#if USE_H264BSF
        av_bitstream_filter_filter(h264bsfc, in_stream->codec, NULL, &pkt.data, &pkt.size, pkt.data, pkt.size, 0);
#endif
#if USE_AACBSF
        av_bitstream_filter_filter(aacbsfc, out_stream->codec, NULL, &pkt.data, &pkt.size, pkt.data, pkt.size, 0);
#endif
        
        //Convert PTS/DTS
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;
        pkt.stream_index=stream_index;
        
        if (pkt.stream_index == videoindex_v){
            media_type = "VEDIO";
        }else{
            media_type = "AUDIO";
        }
        
        
        printf("OUTPUT %s Packet.  pts:%lld   dts:%lld    DTS_TIME: %f     PTS_TIME: %f\n",
               media_type,
               pkt.pts,
               pkt.dts,
               (double)pkt.dts * av_q2d(ofmt_ctx->streams[pkt.stream_index]->time_base),
               (double)pkt.pts * av_q2d(ofmt_ctx->streams[pkt.stream_index]->time_base));
        
        //Write
        if (av_interleaved_write_frame(ofmt_ctx, &pkt) < 0) {
            printf( "Error muxing packet\n");
            break;
        }
        av_free_packet(&pkt);
        
    }

    //Write file trailer
    av_write_trailer(ofmt_ctx);
    
#if USE_H264BSF
    av_bitstream_filter_close(h264bsfc);
#endif
#if USE_AACBSF
    av_bitstream_filter_close(aacbsfc);
#endif
    
end:
    
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    
    avformat_close_input(&ifmt_ctx_v);
    avformat_close_input(&ifmt_ctx_a);
    
    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_close(ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    if (ret < 0 && ret != AVERROR_EOF) {
        printf( "Error occurred.\n");
        return -1;
    }
    return 0;
}
