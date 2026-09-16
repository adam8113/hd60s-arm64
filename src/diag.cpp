// minimal read-only diagnostic. Issues only MCU read requests and
// reports the real IOKit status. No GPIO, no reset, no configuration writes.
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "hd60s_ids.h"
static IOUSBDeviceInterface500** g=nullptr;
static const char* err(IOReturn r){
  switch(r){
    case kIOReturnSuccess: return "ok";
    case kIOUSBPipeStalled: return "PIPE STALLED";
    case kIOUSBTransactionTimeout: return "TIMEOUT";
    case kIOReturnNotResponding: return "NOT RESPONDING";
    case kIOReturnNoDevice: return "NO DEVICE";
    case kIOReturnNotOpen: return "NOT OPEN";
    case kIOReturnBadArgument: return "BAD ARGUMENT";
    case kIOReturnExclusiveAccess: return "EXCLUSIVE ACCESS";
    default: return "?";
  }
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  io_iterator_t it;io_service_t s;
  IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching(kIOUSBDeviceClassName),&it);
  while((s=IOIteratorNext(it))){IOCFPlugInInterface**pl=nullptr;SInt32 sc;
    if(IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc)==KERN_SUCCESS&&pl){
      IOUSBDeviceInterface500**d=nullptr;
      (*pl)->QueryInterface(pl,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);(*pl)->Release(pl);
      if(d){UInt16 v,p;(*d)->GetDeviceVendor(d,&v);(*d)->GetDeviceProduct(d,&p);
        if(isHD60S(v, p)){g=d;IOObjectRelease(s);break;}(*d)->Release(d);}}
    IOObjectRelease(s);} IOObjectRelease(it);
  if(!g){printf("  card not found\n");return 1;}
  IOReturn o=(*g)->USBDeviceOpen(g);
  printf("  USBDeviceOpen           : 0x%08x %s\n", o, err(o));
  UInt8 c=0; IOReturn gc=(*g)->GetConfiguration(g,&c);
  printf("  GetConfiguration        : 0x%08x %s -> cfg %u\n", gc, err(gc), c);
  if(c!=1){ IOReturn r=(*g)->SetConfiguration(g,1);
            printf("  SetConfiguration(1)     : 0x%08x %s\n", r, err(r));
            (*g)->GetConfiguration(g,&c);
            printf("  config now              : %u\n", c); }
  // standard request first: device descriptor via control pipe
  { IOUSBDevRequest q; memset(&q,0,sizeof q); uint8_t b[18];
    q.bmRequestType=USBmakebmRequestType(kUSBIn,kUSBStandard,kUSBDevice);
    q.bRequest=6; q.wValue=0x0100; q.wIndex=0; q.wLength=18; q.pData=b;
    IOReturn r=(*g)->DeviceRequest(g,&q);
    printf("  GET_DESCRIPTOR (standard): 0x%08x %s, %u bytes\n", r, err(r), (unsigned)q.wLenDone); }
  // now a vendor read request - the MCU read
  { uint8_t cmd[3]={0x9b,0x01,0x0e};
    IOUSBDevRequest q; memset(&q,0,sizeof q);
    q.bmRequestType=USBmakebmRequestType(kUSBOut,kUSBVendor,kUSBDevice);
    q.bRequest=0xC0; q.wValue=0x5066; q.wIndex=0; q.wLength=3; q.pData=cmd;
    IOReturn r=(*g)->DeviceRequest(g,&q);
    printf("  vendor OUT 9b 01 0e     : 0x%08x %s\n", r, err(r));
    uint8_t v=0; memset(&q,0,sizeof q);
    q.bmRequestType=USBmakebmRequestType(kUSBIn,kUSBVendor,kUSBDevice);
    q.bRequest=0xC0; q.wValue=0x5066; q.wIndex=0; q.wLength=1; q.pData=&v;
    IOReturn r2=(*g)->DeviceRequest(g,&q);
    printf("  vendor IN  (expect 0x6c): 0x%08x %s -> 0x%02x\n", r2, err(r2), v); }
  (*g)->USBDeviceClose(g);
  return 0;
}
