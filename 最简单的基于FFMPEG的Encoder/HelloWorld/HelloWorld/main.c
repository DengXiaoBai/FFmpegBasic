//
//  main.c
//  HelloWorld
//
//  Created by angle on 27/03/2018.
//  Copyright © 2018 angle. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include "MainHeader.h"


int main(int argc, const char * argv[]) {
    
    char* input_v = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/donghua.mp4";
    char* input_a = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/AS.mp4";
    char* muxer_output = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/muxer.flv";

    int ret = mix_video_audio_to_mp4(input_v, input_a, muxer_output,1);
  
    return ret;
}



// 没有成功!!!
int test_encode_pcm_to_acc(void){
    
    char* input_url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/tdjm.pcm";
    char* output_url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/tdjm.aac";
    
    int ret = encode_pcm_to_acc(input_url, output_url);
    
    return ret;
}



int test_ecoder_yuv_to_264(void){
    char* input_url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/ds_480x272.yuv";
    char* output_url = "/Users/stringstech-macmini1/Desktop/AV_Study/media_src/ds_480x272.h264";
    
    int ret = ecoder_yuv_to_264(input_url,output_url);
    
    return ret;
}



void test_ffmpeg(void){
    // insert code here...
    printf("Hello, World!\n");
    
    char *infostr=NULL;
    infostr=configurationinfo();
    printf("\n<<Configuration>>\n%s",infostr);
    free(infostr);
    
    infostr=urlprotocolinfo();
    printf("\n<<URLProtocol>>\n%s",infostr);
    free(infostr);
    
    infostr=avformatinfo();
    printf("\n<<AVFormat>>\n%s",infostr);
    free(infostr);
    
    infostr=avcodecinfo();
    printf("\n<<AVCodec>>\n%s",infostr);
    free(infostr);
    
    infostr=avfilterinfo();
    printf("\n<<AVFilter>>\n%s",infostr);
    free(infostr);
}

