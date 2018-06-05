//
//  main.c
//  HelloWorld
//
//  Created by angle on 27/03/2018.
//  Copyright © 2018 angle. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>

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


int main(int argc, const char * argv[]) {
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
    
    return 0;
}
