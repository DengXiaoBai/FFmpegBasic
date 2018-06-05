//
//  mainheader.h
//  simplest_ffmpeg_player
//
//  Created by DengXiaoBai on 2018/4/24.
//  Copyright © 2018年 angle. All rights reserved.
//

#ifndef mainheader_h
#define mainheader_h

extern int paly(char* media_url);


extern int play_media(char* media_url);
extern int play_media_with_threads(char* url);
extern int play_media_with_videosync(char* url);
extern int play_media_with_seek(char * url);

extern int play_media_vediosync2(char* url);
extern int play_sound(char* url);
extern int play_sound_only(char* sound_url);

extern int play_media_with_video_sync_right(char* url);
#endif /* mainheader_h */
