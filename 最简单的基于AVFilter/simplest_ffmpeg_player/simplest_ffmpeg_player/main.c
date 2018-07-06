//
//  main.c
//  simplest_ffmpeg_player
//
//  Created by angle on 27/03/2018.
//  Copyright © 2018 angle. All rights reserved.
//
 
// TODO: 先用ani.mp4这个资源把整个流程走完. 现在是seek有点问题. 之后去看看ffplay源码

#include <stdio.h>
#include "filter_header.h"
#include "PlayerIssueHeader.h"
#include "PlayerRightHeader.h"


int main(int argc, const char * argv[]) {
    int  ret = 0;
    char *file = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/apink.ts";
    char *v_file = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/ani.mp4";
    char *a_file = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/Shape_of_you.mp3";

//  ret = video_watermark(file);

//  audio_filter(file, "500");
//    ret = push_with_filter(v_file, a_file, FILTER_SPLIT4);

    // FIXME : 播放ani.mp4没有问题, 播放shameless.mkv没有人的声音 播放4k视频有问题
//    ret = play_media_with_sync(v_file);

    ret = play_sound_only("/Users/stringstech-macmini1/Desktop/AV_Study/media_src/说散就散.ape");

    return ret;
}


/*
 * 视频播放器
 */
int test_video_player(void){
    char* url = "/Users/stringstech-macmini1/Desktop/AV_Study/FFmpeg_Leixiaohua-master/最简单的基于FFMPEG+SDL的视频播放器/res/Titanic.ts";
    
    // char* url = "rtmp://localhost:1935/rtmplive";
    int ret = paly(url);
    return ret;
}

/*
 * 判断大小端存储
 * 其实不用我们自己判断, 很多库已经帮我们判断好并且会选择对应的FORMAT
 */
int is_little_endian(){
    short int x;
    char x0,x1;
    x=0x1122;
    x0=((char *)&x)[0];  //低地址单元
    x1=((char *)&x)[1];  //高地址单元
    printf("x0=0x%x,x1=0x%x",x0,x1);// 若x0=0x11,则是大端; 若x0=0x22,则是小端......=
    return x0 == 0x22;
}


