#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
/* Signature: D-Robotics/x5-libcam-inc develop/camera_reg.h.
 * Passive interposer: forwards existing SDK calls, never adds a write. */
static int32_t (*real_write)(int32_t,int32_t,int32_t,uint32_t,uint8_t);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void resolve(void) {
    *(void **)(&real_write)=dlsym(RTLD_NEXT,"camera_reg_i2c_write8");
    if(!real_write) {fprintf(stderr,"APX_TRACE missing next write symbol\n");abort();}
}
int32_t camera_reg_i2c_write8(int32_t bus,int32_t width,int32_t addr,uint32_t reg,uint8_t value) {
    pthread_once(&once,resolve);
    int32_t ret=real_write(bus,width,addr,reg,value);
    if(reg==0x0157 || reg==0x015a || reg==0x015b) {
        struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
        fprintf(stderr,"APX_TRACE t=%ld.%09ld bus=%d width=%d addr=0x%x reg=0x%x value=0x%x ret=%d\n",ts.tv_sec,ts.tv_nsec,bus,width,addr,reg,value,ret);
    }
    return ret;
}
