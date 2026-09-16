// live HD60 S input status via the real protocol.
// 0x55 is the signal-lock flag: 0x03 with a locked HDMI input, 0x00 without.
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include "hd60s_ids.h"
static IOUSBDeviceInterface500** g=nullptr;
static int ctl(uint8_t bReq,uint16_t wv,void* b,uint16_t n,bool in){
    IOUSBDevRequest r; memset(&r,0,sizeof r);
    r.bmRequestType=USBmakebmRequestType(in?kUSBIn:kUSBOut,kUSBVendor,kUSBDevice);
    r.bRequest=bReq; r.wValue=wv; r.wIndex=0; r.wLength=n; r.pData=b;
    return (*g)->DeviceRequest(g,&r);
}
static int mcu(uint8_t c,uint8_t reg){
    uint8_t cmd[3]={c,0x01,reg};
    if(ctl(0xC0,0x5066,cmd,3,false)!=kIOReturnSuccess) return -1;
    uint8_t v=0; if(ctl(0xC0,0x5066,&v,1,true)!=kIOReturnSuccess) return -1;
    return v;
}
int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    int secs=argc>1?atoi(argv[1]):20;
    io_iterator_t it; io_service_t s;
    IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching(kIOUSBDeviceClassName),&it);
    while((s=IOIteratorNext(it))){
        IOCFPlugInInterface**pl=nullptr;SInt32 sc;
        if(IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,
             kIOCFPlugInInterfaceID,&pl,&sc)==KERN_SUCCESS&&pl){
            IOUSBDeviceInterface500**d=nullptr;
            (*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);
            (*pl)->Release(pl);
            if(d){UInt16 v,p;(*d)->GetDeviceVendor(d,&v);(*d)->GetDeviceProduct(d,&p);
                  if(isHD60S(v, p)){g=d;IOObjectRelease(s);break;}(*d)->Release(d);}
        } IOObjectRelease(s);
    } IOObjectRelease(it);
    if(!g){printf("HD60 S not found\n");return 1;}
    (*g)->USBDeviceOpen(g);
    UInt8 c=0;(*g)->GetConfiguration(g,&c); if(c!=1)(*g)->SetConfiguration(g,1);
    printf("Windows values while streaming:  55=0x03  11=0xf4  12=0xa2  09=0xb2  0e=0x6c\n");
    printf("polling for %ds - plug/unplug HDMI to see it change\n\n", secs);
    for(int i=0;i<secs;i++){
        int lock=mcu(0x9d,0x55), r11=mcu(0x9d,0x11), r12=mcu(0x9d,0x12),
            r09=mcu(0x9b,0x09), r0e=mcu(0x9b,0x0e);
        printf("  t+%02ds  lock55=0x%02x %-12s  11=0x%02x 12=0x%02x  09=0x%02x 0e=0x%02x\n",
               i, lock, lock==0x03?"<< LOCKED":(lock==0?"(no signal)":"(?)"),
               r11,r12,r09,r0e);
        sleep(1);
    }
    (*g)->USBDeviceClose(g);
    return 0;
}
