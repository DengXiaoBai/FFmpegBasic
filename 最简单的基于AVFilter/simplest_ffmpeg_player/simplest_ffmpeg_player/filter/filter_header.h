//
//  filter_header.h
//  simplest_ffmpeg_player
//
//  Created by DengXiaoBai on 2018/5/22.
//  Copyright © 2018年 angle. All rights reserved.
//

#ifndef filter_header_h
#define filter_header_h

typedef enum{
    FILTER_NULL = 48,
    FILTER_MIRROR,
    FILTER_WATERMATK,
    FILTER_NEGATE,
    FILTER_EDGE,
    FILTER_SPLIT4,
    FILTER_VINTAGE
}FILTERS;

extern  int video_watermark(char *file);

extern int audio_filter(char* audio_file, char* d);

extern int push_with_filter(char* vedio_url, char* audio_url,FILTERS f);

#endif /* filter_header_h */
