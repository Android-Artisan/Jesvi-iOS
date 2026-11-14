#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// rwpref DOMAIN KEY bool|num VALUE  (writes mobile's preferences)
int main(int c,char**v){ if(c<5){fprintf(stderr,"usage\n");return 2;}
 CFStringRef d=CFStringCreateWithCString(0,v[1],kCFStringEncodingUTF8),k=CFStringCreateWithCString(0,v[2],kCFStringEncodingUTF8);
 CFTypeRef val; if(!strcmp(v[3],"bool")) val=atoi(v[4])?kCFBooleanTrue:kCFBooleanFalse; else {double x=atof(v[4]); val=CFNumberCreate(0,kCFNumberDoubleType,&x);}
 CFPreferencesSetValue(k,val,d,CFSTR("mobile"),kCFPreferencesAnyHost);
 return CFPreferencesSynchronize(d,CFSTR("mobile"),kCFPreferencesAnyHost)?0:1;}
