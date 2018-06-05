//
//  main.c
//  simplest_ffmpeg_player
//
//  Created by angle on 27/03/2018.
//  Copyright © 2018 angle. All rights reserved.
//

#include <stdio.h>
#include "mainheader.h"

int main(int argc, const char * argv[]) {
    // insert code here...
    printf("Hello, World!\n");
    int  ret = 0;

//    char* url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/AS.mp4";
    char* url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/donghua.mp4";
//    char* url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/生命是一次奇遇.mp3";
//      ret = play_media(url);
      ret = play_media_with_threads(url);
//     ret = play_media_with_videosync(url);
//      ret = play_media_with_seek(url);
//      ret = play_media_vediosync2(url);
//     ret = play_sound(url);
//     ret = play_sound_only(url);

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


int is_little_endian(){
    short int x;
    char x0,x1;
    x=0x1122;
    x0=((char *)&x)[0];  //低地址单元
    x1=((char *)&x)[1];  //高地址单元
    printf("x0=0x%x,x1=0x%x",x0,x1);// 若x0=0x11,则是大端; 若x0=0x22,则是小端......=
    return x0 == 0x22;
}


