//
//  MainHeader.h
//  HelloWorld
//
//  Created by DengXiaoBai on 2018/4/23.
//  Copyright © 2018年 angle. All rights reserved.
//

#ifndef MainHeader_h
#define MainHeader_h

/**
 * Protocol Support Information
 */
extern char * urlprotocolinfo(void);
/**
 * AVFormat Support Information
 */
extern char * avformatinfo(void);
/**
 * AVCodec Support Information
 */
extern char * avcodecinfo(void);
/**
 * AVFilter Support Information
 */
extern char * avfilterinfo(void);
/**
 * Configuration Information
 */
extern char * configurationinfo(void);

extern int ecoder_yuv_to_264(char* uyv_url, char* h264_url);

extern  int encode_pcm_to_acc(char* pcm_url, char* acc_url);


extern int mix_video_audio_to_mp4(char* video_url, char* audio_url, char*output_url,int push_to_server);


#endif /* MainHeader_h */
