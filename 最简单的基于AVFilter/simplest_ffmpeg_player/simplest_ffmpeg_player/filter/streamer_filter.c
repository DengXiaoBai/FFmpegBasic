
#include "filter_header.h"

#include <stdio.h>
#include "opt.h"
#include "time.h"
#include "mathematics.h"
#include "avcodec.h"
#include "avformat.h"
#include "avdevice.h"
#include "swscale.h"
#include "swresample.h"
#include "audio_fifo.h"

#include "avfiltergraph.h"
#include "buffersink.h"
#include "buffersrc.h"

/* note
 * 视频处理: 使用视频过滤器处理
 * 音频处理: 直接使用swr 处理
 * */

/*TODO
 * 用SDL监听用户键盘输入, 改变filter, SDL_Event
 * */

/* FIXME
 * 使用ffplay播放的时候音频不稳定, 有时候会没有
 * log显示有 push音频, ffplay也有收到音频
 * */

/*FIXME
 * 同样的filter_descr, 在javacv中得到的效果是不一样的.....
 * */

static int exit_thread = 0;
static int filter_inited = 0;

static const char *filter_descr = "null";
static const char *filter_mirror = "crop=iw/2:ih:0:0,split[left][tmp];[tmp]hflip[right];[left]pad=iw*2[a];[a][right]overlay=w";
static const char *filter_watermark = "movie=/Users/stringstech-macmini1/Desktop/AV_Study/media_src/leishen_logo.png[wm];[in][wm]overlay=5:5[out]";
static const char *filter_negate = "negate[out]";
static const char *filter_edge = "edgedetect[out]";
static const char *filter_split4 = "scale=iw/2:ih/2[in_tmp];[in_tmp]split=4[in_1][in_2][in_3][in_4];[in_1]pad=iw*2:ih*2[a];[a][in_2]overlay=w[b];[b][in_3]overlay=0:h[d];[d][in_4]overlay=w:h[out]";
static const char *filter_vintage = "curves=vintage";


static AVFilterContext *buffersink_ctx;
static AVFilterContext *buffersrc_ctx;
static AVFilterGraph *filter_graph;
static AVFilter *buffersrc;
static AVFilter *buffersink;
static AVFrame* picref;


static char* filter2string(FILTERS f){
    char *descr = filter_descr;
    if (f == FILTER_NULL){
        descr = filter_descr;
    } else if (f == FILTER_MIRROR){
        descr = filter_mirror;
    }else if (f == FILTER_WATERMATK){
        descr = filter_watermark;
    }else if (f == FILTER_NEGATE){
        descr = filter_negate;
    }else if (f == FILTER_EDGE){
        descr = filter_edge;
    }else if (f == FILTER_SPLIT4){
        descr = filter_split4;
    }else if (f == FILTER_VINTAGE){
        descr = filter_vintage;
    }

    return descr;
}

static int apply_filters(AVFormatContext *ifmt_ctx,FILTERS f) {
    char args[512];
    int ret;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    if (!outputs)
    {
        printf("Cannot alloc output\n");
        return -1;
    }
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!inputs)
    {
        printf("Cannot alloc input\n");
        return -1;
    }

    if (filter_graph)
        avfilter_graph_free(&filter_graph);
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
    {
        printf("Cannot create filter graph\n");
        return -1;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        ifmt_ctx->streams[0]->codec->width, ifmt_ctx->streams[0]->codec->height, ifmt_ctx->streams[0]->codec->pix_fmt,
        ifmt_ctx->streams[0]->time_base.num, ifmt_ctx->streams[0]->time_base.den,
        ifmt_ctx->streams[0]->codec->sample_aspect_ratio.num, ifmt_ctx->streams[0]->codec->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
        args, NULL, filter_graph);
    if (ret < 0) {
        printf("Cannot create buffer source\n");
        return ret;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
        NULL, NULL, filter_graph);
    if (ret < 0) {
        printf("Cannot create buffer sink\n");
        return ret;
    }

    /* Endpoints for the filter graph. */
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    char *f_descr = filter2string(f);
    printf("视频过滤器descr: %s \n",f_descr);
    if (strlen(f_descr) == 0 ){
        f_descr = filter_descr;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, f_descr,
        &inputs, &outputs, NULL)) < 0)
        return ret;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return 0;
}

static int flush_encoder(AVFormatContext *ifmt_ctx, AVFormatContext *ofmt_ctx, unsigned int stream_index, int framecnt){
    int ret;
    int got_frame;
    AVPacket enc_pkt;
    if (!(ofmt_ctx->streams[stream_index]->codec->codec->capabilities &
            CODEC_CAP_DELAY))
        return 0;
    while (1) {
        enc_pkt.data = NULL;
        enc_pkt.size = 0;
        av_init_packet(&enc_pkt);
        ret = avcodec_encode_video2(ofmt_ctx->streams[stream_index]->codec, &enc_pkt,
                NULL, &got_frame);
        av_frame_free(NULL);
        if (ret < 0)
            break;
        if (!got_frame){
            ret = 0;
            break;
        }
        printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n", enc_pkt.size);
        framecnt++;
        //Write PTS
        AVRational time_base = ofmt_ctx->streams[stream_index]->time_base;//{ 1, 1000 };
        AVRational r_framerate1 = ifmt_ctx->streams[0]->r_frame_rate;// { 50, 2 };
        AVRational time_base_q = { 1, AV_TIME_BASE };
        //Duration between 2 frames (us)
        int64_t calc_duration = (double)(AV_TIME_BASE)*(1 / av_q2d(r_framerate1));
        //Parameters
        enc_pkt.pts = av_rescale_q(framecnt*calc_duration, time_base_q, time_base);
        enc_pkt.dts = enc_pkt.pts;
        enc_pkt.duration = av_rescale_q(calc_duration, time_base_q, time_base);

        /* copy packet*/
        enc_pkt.pos = -1;

        //ofmt_ctx->duration = enc_pkt.duration * framecnt;

        /* mux encoded frame */
        ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
        if (ret < 0)
            break;
    }
    return ret;
}

static int flush_encoder_a(AVFormatContext *ifmt_ctx_a, AVFormatContext *ofmt_ctx, unsigned int stream_index, int nb_samples){
    int ret;
    int got_frame;
    AVPacket enc_pkt;
    if (!(ofmt_ctx->streams[stream_index]->codec->codec->capabilities &
            CODEC_CAP_DELAY))
        return 0;
    while (1) {
        enc_pkt.data = NULL;
        enc_pkt.size = 0;
        av_init_packet(&enc_pkt);
        ret = avcodec_encode_audio2(ofmt_ctx->streams[stream_index]->codec, &enc_pkt,
                NULL, &got_frame);
        av_frame_free(NULL);
        if (ret < 0)
            break;
        if (!got_frame){
            ret = 0;
            break;
        }
        printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n", enc_pkt.size);
        nb_samples+=1024;
        //Write PTS
        AVRational time_base = ofmt_ctx->streams[stream_index]->time_base;//{ 1, 1000 };
        AVRational r_framerate1 = { ifmt_ctx_a->streams[0]->codec->sample_rate, 1 };
        AVRational time_base_q = { 1, AV_TIME_BASE };
        //Duration between 2 frames (us)
        int64_t calc_duration = (double)(AV_TIME_BASE)*(1 / av_q2d(r_framerate1));
        //Parameters
        enc_pkt.pts = av_rescale_q(nb_samples*calc_duration, time_base_q, time_base);
        enc_pkt.dts = enc_pkt.pts;
        enc_pkt.duration = 1024;

        /* copy packet*/
        enc_pkt.pos = -1;

        //ofmt_ctx->duration = enc_pkt.duration * nb_samples;

        /* mux encoded frame */
        ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
        if (ret < 0)
            break;
    }
    return ret;
}

int push_with_filter(char* vedio_url, char* audio_url,FILTERS f) {
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ifmt_ctx_a = NULL;
    AVFormatContext *ofmt_ctx;
    AVInputFormat* ifmt;
    AVStream* video_st;
    AVStream* audio_st;
    AVCodecContext* pCodecCtx;
    AVCodecContext* pCodecCtx_a;

    AVCodec* pCodec;
    AVCodec* pCodec_a;

    AVPacket *dec_pkt, enc_pkt;
    AVPacket *dec_pkt_a, enc_pkt_a;
    AVFrame *pframe, *pFrameYUV;
    struct SwsContext *img_convert_ctx;
    struct SwrContext *aud_convert_ctx;

    int framecnt = 0;
    int nb_samples = 0;
    int videoindex;
    int audioindex;
    int i;
    int ret;

    const char* out_path = "rtmp://10.0.1.104:1935/live/dxb";
    int dec_got_frame, enc_got_frame;
    int dec_got_frame_a, enc_got_frame_a;

    int aud_next_pts = 0;
    int vid_next_pts = 0;
    int encode_video = 1, encode_audio = 1;

    AVRational time_base_q = { 1, AV_TIME_BASE };

    av_register_all();
    //Register Device
    avdevice_register_all();
    avformat_network_init();

    //Register Filter
    avfilter_register_all();
    buffersrc = avfilter_get_by_name("buffer");
    buffersink = avfilter_get_by_name("buffersink");


    if(avformat_open_input(&ifmt_ctx,vedio_url,NULL,NULL)!=0){
        printf("Couldn't open input stream.\n");
        return -1;
    }

    if(avformat_find_stream_info(ifmt_ctx,NULL)<0){
        printf("Couldn't find stream information.\n");
        return -1;
    }


    videoindex = -1;
    for (i = 0; i < ifmt_ctx->nb_streams; i++){
        if (ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
            break;
        }
    }

    if (videoindex == -1) {
        printf("Couldn't find a video stream.\n");
        return -1;
    }

    if (avcodec_open2(ifmt_ctx->streams[videoindex]->codec, avcodec_find_decoder(ifmt_ctx->streams[videoindex]->codec->codec_id), NULL) < 0) {
        printf("Could not open video codec.\n");
        return -1;
    }


    //input audio initialize
    if(avformat_open_input(&ifmt_ctx_a,audio_url,NULL,NULL)!=0){
        printf("Couldn't open input stream.\n");
        return -1;
    }

    if (avformat_find_stream_info(ifmt_ctx_a, NULL) < 0) {
        printf("Couldn't find audio stream information.\n");
        return -1;
    }

    audioindex = -1;
    for (i = 0; i < ifmt_ctx_a->nb_streams; i++){
        if (ifmt_ctx_a->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioindex = i;
            break;
        }
    }

    if (audioindex == -1) {
        printf("Couldn't find a audio stream.\n");
        return -1;
    }

    if (avcodec_open2(ifmt_ctx_a->streams[audioindex]->codec, avcodec_find_decoder(ifmt_ctx_a->streams[audioindex]->codec->codec_id), NULL) < 0) {
        printf("Could not open audio codec.\n");
        return -1;
    }

    //output initialize
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_path);

    //output video encoder initialize
    pCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!pCodec){
        printf("Can not find output video encoder!)\n");
        return -1;
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);
    pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    pCodecCtx->width = ifmt_ctx->streams[videoindex]->codec->width;
    pCodecCtx->height = ifmt_ctx->streams[videoindex]->codec->height;
    pCodecCtx->time_base.num = 1;
    pCodecCtx->time_base.den = 25;
    pCodecCtx->bit_rate = 300000;
    pCodecCtx->gop_size = 250;
    /* Some formats want stream headers to be separate. */
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        pCodecCtx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    //H264 codec param
    //pCodecCtx->me_range = 16;
    //pCodecCtx->max_qdiff = 4;
    //pCodecCtx->qcompress = 0.6;
    pCodecCtx->qmin = 10;
    pCodecCtx->qmax = 51;
    //Optional Param
    pCodecCtx->max_b_frames = 0;
    // Set H264 preset and tune
    AVDictionary *param = 0;
    av_dict_set(&param, "preset", "fast", 0);
    av_dict_set(&param, "tune", "zerolatency", 0);

    if (avcodec_open2(pCodecCtx, pCodec, &param) < 0){
        printf("Failed to open output video encoder! \n");
        return -1;
    }

    //Add a new stream to output,should be called by the user before avformat_write_header() for muxing
    video_st = avformat_new_stream(ofmt_ctx, pCodec);
    if (video_st == NULL){
        return -1;
    }
    video_st->time_base.num = 1;
    video_st->time_base.den = 25;
    video_st->codec = pCodecCtx;


    //output audio encoder initialize
    pCodec_a = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!pCodec_a){
        printf("Can not find output audio encoder! \n");
        return -1;
    }
    pCodecCtx_a = avcodec_alloc_context3(pCodec_a);
    pCodecCtx_a->channels = 2;
    pCodecCtx_a->channel_layout = av_get_default_channel_layout(2);
    pCodecCtx_a->sample_rate = ifmt_ctx_a->streams[audioindex]->codec->sample_rate;
    pCodecCtx_a->sample_fmt = pCodec_a->sample_fmts[0];
    pCodecCtx_a->bit_rate = 32000;
    pCodecCtx_a->time_base.num = 1;
    pCodecCtx_a->time_base.den = pCodecCtx_a->sample_rate;
    /** Allow the use of the experimental AAC encoder */
    pCodecCtx_a->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /* Some formats want stream headers to be separate. */
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        pCodecCtx_a->flags |= CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(pCodecCtx_a, pCodec_a, NULL) < 0){
        printf("Failed to open ouput audio encoder! \n");
        return -1;
    }

    //Add a new stream to output,should be called by the user before avformat_write_header() for muxing
    audio_st = avformat_new_stream(ofmt_ctx, pCodec_a);
    if (audio_st == NULL){
        return -1;
    }
    audio_st->time_base.num = 1;
    audio_st->time_base.den = pCodecCtx_a->sample_rate;
    audio_st->codec = pCodecCtx_a;

    //Open output URL,set before avformat_write_header() for muxing
    if (avio_open(&ofmt_ctx->pb, out_path, AVIO_FLAG_READ_WRITE) < 0){
        printf("Failed to open output file! \n");
        return -1;
    }

    //Show some Information
    av_dump_format(ofmt_ctx, 0, out_path, 1);


    //Write File Header
    avformat_write_header(ofmt_ctx, NULL);

    //prepare before decode and encode
    dec_pkt = (AVPacket *)av_malloc(sizeof(AVPacket));

    //camera data may has a pix fmt of RGB or sth else,convert it to YUV420
    img_convert_ctx = sws_getContext(ifmt_ctx->streams[videoindex]->codec->width, ifmt_ctx->streams[videoindex]->codec->height,
            ifmt_ctx->streams[videoindex]->codec->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    // Initialize the resampler to be able to convert audio sample formats
    aud_convert_ctx = swr_alloc_set_opts(NULL,
            av_get_default_channel_layout(pCodecCtx_a->channels),
            pCodecCtx_a->sample_fmt,
            pCodecCtx_a->sample_rate,
            av_get_default_channel_layout(ifmt_ctx_a->streams[audioindex]->codec->channels),
            ifmt_ctx_a->streams[audioindex]->codec->sample_fmt,
            ifmt_ctx_a->streams[audioindex]->codec->sample_rate,
            0, NULL);

    /**
    * Perform a sanity check so that the number of converted samples is
    * not greater than the number of samples to be converted.
    * If the sample rates differ, this case has to be handled differently
    */
    //av_assert0(pCodecCtx_a->sample_rate == ifmt_ctx_a->streams[audioindex]->codec->sample_rate);
    swr_init(aud_convert_ctx);

    //Initialize the buffer to store YUV frames to be encoded.
    pFrameYUV = av_frame_alloc();
    uint8_t *out_buffer = (uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height));
    avpicture_fill((AVPicture *)pFrameYUV, out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height);

    //Initialize the FIFO buffer to store audio samples to be encoded.
    AVAudioFifo *fifo = NULL;
    fifo = av_audio_fifo_alloc(pCodecCtx_a->sample_fmt, pCodecCtx_a->channels, 1);

    //Initialize the buffer to store converted samples to be encoded.
    uint8_t **converted_input_samples = NULL;
    /**
    * Allocate as many pointers as there are audio channels.
    * Each pointer will later point to the audio samples of the corresponding
    * channels (although it may be NULL for interleaved formats).
    */
    if (!(converted_input_samples = (uint8_t**)calloc(pCodecCtx_a->channels,
            sizeof(**converted_input_samples)))) {
        printf("Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    printf("\n --------call started----------\n");

    //start decode and encode
    int64_t start_time = av_gettime();

    while (encode_video || encode_audio) {
        // 视频处理
        if (encode_video && (!encode_audio || av_compare_ts(vid_next_pts, time_base_q, aud_next_pts, time_base_q) <= 0)) {

            while (1) {
                // 读取视频packet
                ret = av_read_frame(ifmt_ctx, dec_pkt);

                if (ret < 0) {// 读取失败
                    if (ret == AVERROR_EOF) {
                        // 终止视频处理
                        printf("END OF VEDIO FILE \n");
                        encode_video = 0;
                        return ret;
                    } else{
                        printf("av_read_frame VEDIO FILE FAILED\n");
                        break;
                    }
                } else {// 读取成功
                    if (dec_pkt->stream_index == videoindex) {
                        break;
                    } else {
                        continue;
                    }
                }
            }

            if (exit_thread)
                break;

            av_log(NULL, AV_LOG_DEBUG, "Going to reencode the frame\n");
            pframe = av_frame_alloc();
            if (!pframe) {
                ret = AVERROR(ENOMEM);
                return ret;
            }
            ret = avcodec_decode_video2(ifmt_ctx->streams[dec_pkt->stream_index]->codec, pframe, &dec_got_frame, dec_pkt);
            if (ret < 0) {
                av_frame_free(&pframe);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }
            if (dec_got_frame){
                pframe->pts = av_frame_get_best_effort_timestamp(pframe);

                // USE FILTER
                if (!filter_inited){
                    filter_inited = 1;
                    apply_filters(ifmt_ctx,f);
                }

                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame(buffersrc_ctx, pframe) < 0) {
                    printf("Error while feeding the filtergraph\n");
                    break;
                }

                picref = av_frame_alloc();

                /* pull filtered pictures from the filtergraph */
                while (1) {
                    ret = av_buffersink_get_frame_flags(buffersink_ctx, picref, 0);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;

                    if (ret < 0)
                        return ret;

                    if (picref) {
                        img_convert_ctx = sws_getContext(picref->width, picref->height, picref->format, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
                        sws_scale(img_convert_ctx, (const uint8_t* const*)picref->data, picref->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
                        sws_freeContext(img_convert_ctx);
                        pFrameYUV->width = picref->width;
                        pFrameYUV->height = picref->height;
                        pFrameYUV->format = AV_PIX_FMT_YUV420P;

                        enc_pkt.data = NULL;
                        enc_pkt.size = 0;
                        av_init_packet(&enc_pkt);
                        ret = avcodec_encode_video2(pCodecCtx, &enc_pkt, pFrameYUV, &enc_got_frame);
                        av_frame_free(&pframe);
                        if (enc_got_frame == 1){
                            //printf("Succeed to encode frame: %5d\tsize:%5d\n", framecnt, enc_pkt.size);
                            framecnt++;
                            enc_pkt.stream_index = video_st->index;

                            //Write PTS
                            AVRational time_base = ofmt_ctx->streams[0]->time_base;//{ 1, 1000 };
                            AVRational r_framerate1 = ifmt_ctx->streams[videoindex]->r_frame_rate;//{ 50, 2 };
                            //Duration between 2 frames (us)
                            int64_t calc_duration = (double)(AV_TIME_BASE)*(1 / av_q2d(r_framerate1));
                            //Parameters
                            //enc_pkt.pts = (double)(framecnt*calc_duration)*(double)(av_q2d(time_base_q)) / (double)(av_q2d(time_base));
                            enc_pkt.pts = av_rescale_q(framecnt*calc_duration, time_base_q, time_base);
                            enc_pkt.dts = enc_pkt.pts;
                            enc_pkt.duration = av_rescale_q(calc_duration, time_base_q, time_base); //(double)(calc_duration)*(double)(av_q2d(time_base_q)) / (double)(av_q2d(time_base));
                            enc_pkt.pos = -1;
                            //printf("video pts : %d\n", enc_pkt.pts);

                            vid_next_pts=framecnt*calc_duration; //general timebase

                            //Delay
                            int64_t pts_time = av_rescale_q(enc_pkt.pts, time_base, time_base_q);
                            int64_t now_time = av_gettime() - start_time;
                            if ((pts_time > now_time) && ((vid_next_pts + pts_time - now_time)<aud_next_pts))
                                av_usleep(pts_time - now_time);

                            ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
                            printf("push V| pts=%f\n",(float) pts_time / AV_TIME_BASE);
                            av_free_packet(&enc_pkt);
                        }

                        av_frame_unref(picref);
                    }
                }
            } else {
                av_frame_free(&pframe);
            }
            av_free_packet(dec_pkt);


        } else { // 音频处理
            //audio trancoding here
            const int output_frame_size = pCodecCtx_a->frame_size;

            if (exit_thread)
                break;

            /**
            * Make sure that there is one frame worth of samples in the FIFO
            * buffer so that the encoder can do its work.
            * Since the decoder's and the encoder's frame size may differ, we
            * need to FIFO buffer to store as many frames worth of input samples
            * that they make up at least one frame worth of output samples.
            */
            // 解码写入队列
            while (av_audio_fifo_size(fifo) < output_frame_size) {
                /**
                * Decode one frame worth of audio samples, convert it to the
                * output sample format and put it into the FIFO buffer.
                */
                AVFrame *input_frame = av_frame_alloc();
                if (!input_frame) {
                    ret = AVERROR(ENOMEM);
                    return ret;
                }

                /** Decode one frame worth of audio samples. */
                /** Packet used for temporary storage. */
                AVPacket input_packet;
                av_init_packet(&input_packet);
                input_packet.data = NULL;
                input_packet.size = 0;

                /** Read one audio frame from the input file into a temporary packet. */
                // 一直读取, 直到出错或者读取到音频packet
                while (1){
                    ret = av_read_frame(ifmt_ctx_a, &input_packet);
                    if (ret < 0) {// 读取失败
                        /** If we are at the end of the file, flush the decoder below. */
                        if (ret == AVERROR_EOF) {
                            encode_audio = 0;
                            printf("END OF AUDIO FILE \n");
                            return ret;
                        } else{
                            printf("av_read_frame AUDIO FILE FAILED \n");
                            break;
                        }
                    } else{ // 读取成功
                        if (input_packet.stream_index != audioindex){
                            continue;
                        } else{
                            break;
                        }
                    }
                }

                /**
                * Decode the audio frame stored in the temporary packet.
                * The input audio stream decoder is used to do this.
                * If we are at the end of the file, pass an empty packet to the decoder
                * to flush it.
                */
                if ((ret = avcodec_decode_audio4(ifmt_ctx_a->streams[audioindex]->codec, input_frame, &dec_got_frame_a, &input_packet)) < 0) {
                    printf("Could not decode audio frame, audioindex=%d,ret=%d\n",audioindex,ret);
                    return ret;
                }
                av_packet_unref(&input_packet);
                /** If there is decoded data, convert and store it */
                if (dec_got_frame_a) {
                    /**
                    * Allocate memory for the samples of all channels in one consecutive
                    * block for convenience.
                    */
                    if ((ret = av_samples_alloc(converted_input_samples, NULL,
                            pCodecCtx_a->channels,
                            input_frame->nb_samples,
                            pCodecCtx_a->sample_fmt, 0)) < 0) {
                        printf("Could not allocate converted input samples\n");
                        av_freep(&(*converted_input_samples)[0]);
                        free(*converted_input_samples);
                        return ret;
                    }

                    /**
                    * Convert the input samples to the desired output sample format.
                    * This requires a temporary storage provided by converted_input_samples.
                    */
                    /** Convert the samples using the resampler. */
                    if ((ret = swr_convert(aud_convert_ctx,
                            converted_input_samples, input_frame->nb_samples,
                            (const uint8_t**)input_frame->extended_data, input_frame->nb_samples)) < 0) {
                        printf("Could not convert input samples\n");
                        return ret;
                    }

                    /** Add the converted input samples to the FIFO buffer for later processing. */
                    /**
                    * Make the FIFO as large as it needs to be to hold both,
                    * the old and the new samples.
                    */
                    if ((ret = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + input_frame->nb_samples)) < 0) {
                        printf("Could not reallocate FIFO\n");
                        return ret;
                    }

                    /** Store the new samples in the FIFO buffer. */
                    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            input_frame->nb_samples) < input_frame->nb_samples) {
                        printf("Could not write data to FIFO\n");
                        return AVERROR_EXIT;
                    }
                }
            }

            /**
            * If we have enough samples for the encoder, we encode them.
            * At the end of the file, we pass the remaining samples to
            * the encoder.
            */
            if (av_audio_fifo_size(fifo) >= output_frame_size){
                /**
                * Take one frame worth of audio samples from the FIFO buffer,
                * encode it and write it to the output file.
                */

                /** Temporary storage of the output samples of the frame written to the file. */
                AVFrame *output_frame=av_frame_alloc();
                if (!output_frame)
                {
                    ret = AVERROR(ENOMEM);
                    return ret;
                }
                /**
                * Use the maximum number of possible samples per frame.
                * If there is less than the maximum possible frame size in the FIFO
                * buffer use this number. Otherwise, use the maximum possible frame size
                */
                const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                        pCodecCtx_a->frame_size);

                /** Initialize temporary storage for one output frame. */
                /**
                * Set the frame's parameters, especially its size and format.
                * av_frame_get_buffer needs this to allocate memory for the
                * audio samples of the frame.
                * Default channel layouts based on the number of channels
                * are assumed for simplicity.
                */
                output_frame->nb_samples = frame_size;
                output_frame->channel_layout = pCodecCtx_a->channel_layout;
                output_frame->format = pCodecCtx_a->sample_fmt;
                output_frame->sample_rate = pCodecCtx_a->sample_rate;

                /**
                * Allocate the samples of the created frame. This call will make
                * sure that the audio frame can hold as many samples as specified.
                */
                if ((ret = av_frame_get_buffer(output_frame, 0)) < 0) {
                    printf("Could not allocate output frame samples\n");
                    av_frame_free(&output_frame);
                    return ret;
                }

                /**
                * Read as many samples from the FIFO buffer as required to fill the frame.
                * The samples are stored in the frame temporarily.
                */
                if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
                    printf("Could not read data from FIFO\n");
                    return AVERROR_EXIT;
                }

                /** Encode one frame worth of audio samples. */
                /** Packet used for temporary storage. */
                AVPacket output_packet;
                av_init_packet(&output_packet);
                output_packet.data = NULL;
                output_packet.size = 0;

                /** Set a timestamp based on the sample rate for the container. */
                if (output_frame) {
                    nb_samples += output_frame->nb_samples;
                }

                /**
                * Encode the audio frame and store it in the temporary packet.
                * The output audio stream encoder is used to do this.
                */
                if ((ret = avcodec_encode_audio2(pCodecCtx_a, &output_packet,
                        output_frame, &enc_got_frame_a)) < 0) {
                    printf("Could not encode frame\n");
                    av_packet_unref(&output_packet);
                    return ret;
                }

                /** Write one audio frame from the temporary packet to the output file. */
                if (enc_got_frame_a) {

                    output_packet.stream_index = 1;

                    AVRational time_base = ofmt_ctx->streams[1]->time_base;
                    AVRational r_framerate1 = { ifmt_ctx_a->streams[audioindex]->codec->sample_rate, 1 };// { 44100, 1};
                    int64_t calc_duration = (double)(AV_TIME_BASE)*(1 / av_q2d(r_framerate1));

                    output_packet.pts = av_rescale_q(nb_samples*calc_duration, time_base_q, time_base);
                    output_packet.dts = output_packet.pts;
                    output_packet.duration = output_frame->nb_samples;

                    //printf("audio pts : %d\n", output_packet.pts);
                    aud_next_pts = nb_samples*calc_duration;

                    int64_t pts_time = av_rescale_q(output_packet.pts, time_base, time_base_q);
                    int64_t now_time = av_gettime() - start_time;
                    if ((pts_time > now_time) && ((aud_next_pts + pts_time - now_time)<vid_next_pts))
                        av_usleep(pts_time - now_time);

                    if ((ret = av_interleaved_write_frame(ofmt_ctx, &output_packet)) < 0) {
                        printf("Could not write frame\n");
                        av_packet_unref(&output_packet);
                        return ret;
                    }
                    printf("push A| pts=%f\n",(float) pts_time / AV_TIME_BASE);
                    av_packet_unref(&output_packet);
                }
                av_frame_free(&output_frame);
            }
        }
    }


    //Flush Encoder
    ret = flush_encoder(ifmt_ctx, ofmt_ctx, 0, framecnt);
    if (ret < 0) {
        printf("Flushing encoder failed\n");
        return -1;
    }
    ret = flush_encoder_a(ifmt_ctx_a, ofmt_ctx, 1, nb_samples);
    if (ret < 0) {
        printf("Flushing encoder failed\n");
        return -1;
    }


    //Write file trailer
    av_write_trailer(ofmt_ctx);

    //Clean up
    if (filter_graph)
        avfilter_graph_free(&filter_graph);

    if (video_st)
        avcodec_close(video_st->codec);

    if (audio_st)
        avcodec_close(audio_st->codec);
    av_free(out_buffer);

    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        //free(converted_input_samples);
    }

    if (fifo)
        av_audio_fifo_free(fifo);
    avio_close(ofmt_ctx->pb);
    avformat_free_context(ifmt_ctx);
    avformat_free_context(ifmt_ctx_a);
    avformat_free_context(ofmt_ctx);

    return 0;
}

