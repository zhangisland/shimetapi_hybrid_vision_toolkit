#include <stdint.h>
#ifdef TRACE_FAKE_SDK
static int count;
static int correct=1;
int32_t camera_reg_i2c_write8(int32_t bus,int32_t width,int32_t addr,uint32_t reg,uint8_t value) {
    ++count;
    if(bus!=6 || width!=16 || addr!=0x3c || value!=0x87 || (reg!=0x15b && reg!=0x2222)) correct=0;
    return -37;
}
int trace_fake_count(void) {return count;}
int trace_fake_correct(void) {return correct;}
#else
#include <stdio.h>
int32_t camera_reg_i2c_write8(int32_t,int32_t,int32_t,uint32_t,uint8_t);
int trace_fake_count(void);
int trace_fake_correct(void);
int main(void) {
    if(camera_reg_i2c_write8(6,16,0x3c,0x15b,0x87)!=-37) return 1;
    if(camera_reg_i2c_write8(6,16,0x3c,0x2222,0x87)!=-37) return 2;
    if(trace_fake_count()!=2 || !trace_fake_correct()) return 3;
    puts("PASS: tracer forwards all arguments, preserves error, adds no writes");
    return 0;
}
#endif
