/*
 * NFC read-only MCU implementation, adapted from the protocol/state flow in
 * Poohl/joycontrol joycontrol/mcu.py (GPL-3.0), commit
 * 0b79bf7569c07d078576a6fa0e7fd0c212bb5ee3.
 * This C implementation replaces asyncio with a bounded transactional queue.
 * See ../docs/NFC_REFERENCE.md and ../docs/GPL-3.0.txt.
 */
#include "nfc_mcu.h"
#include <string.h>

static uint8_t tag[NFC_TAG_SIZE];
static bool present, seen;
static uint8_t power_state, poll_state;
static uint8_t queue[4][NFC_MCU_SIZE];
static unsigned head, count;
static unsigned received, rejected;
static uint8_t last_cmd, last_sub;
static void status(void);
static void poll(void);
nfc_diagnostics_t nfc_diagnostics(void)
{
    return (nfc_diagnostics_t){power_state,poll_state,last_cmd,last_sub,
                             count,received,rejected,present};
}

uint8_t nfc_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc=0;
    for(size_t i=0;i<len;i++) {
        crc ^= data[i];
        for(unsigned b=0;b<8;b++) crc=(crc&0x80)?(uint8_t)((crc<<1)^7):(uint8_t)(crc<<1);
    }
    return crc;
}
static void flush(void) { head=count=0; }
void nfc_reset(void) { flush(); power_state=0; poll_state=0; seen=false; received=rejected=0; last_cmd=last_sub=0; }
bool nfc_load(const uint8_t *data,size_t len)
{
    if(!data || len!=NFC_TAG_SIZE) return false;
    memcpy(tag,data,len); present=true; seen=false; flush();
    /* Replace stale tag packets but restore the pending MCU/poll status. */
    if(power_state==4 && poll_state) {poll_state=1;poll();}
    else if(power_state) status();
    return true;
}
void nfc_unload(void)
{
    present=false; seen=false; memset(tag,0,sizeof(tag)); flush();
    if(poll_state) {poll_state=1; if(power_state==4) poll();}
}
static void uid(uint8_t *out)
{
    memcpy(out,tag,3); memcpy(out+3,tag+4,4);
}
static uint8_t *reserve(void)
{
    if(count==4) return NULL;
    uint8_t *p=queue[(head+count++)%4];
    memset(p,0,NFC_MCU_SIZE);
    return p;
}
static void finish(uint8_t *p) { p[312]=nfc_crc8(p,312); }
static void status(void)
{
    uint8_t *p=reserve(); if(!p) return;
    if(!power_state) p[0]=0xff;
    else {
        const uint8_t header[]={1,0,0,0,8,0,0x1b};
        memcpy(p,header,sizeof(header));p[7]=power_state;
    }
    finish(p);
}
bool nfc_power(uint8_t value)
{
    if(value>1) return false;
    flush();power_state=value;poll_state=0;seen=false;status();return true;
}
bool nfc_config(const uint8_t *args,size_t len,uint8_t reply[34])
{
    if(!args || len<3) return false;
    if(args[2]!=0 && args[2]!=1 && args[2]!=4) return false;
    if(power_state && args[2]) {
        power_state=args[2];poll_state=0;seen=false;flush();status();
    }
    memset(reply,0,34);
    const uint8_t header[]={1,0,0xff,0,8,0,0x1b,1};
    memcpy(reply,header,sizeof(header));reply[33]=nfc_crc8(reply,33);
    return true;
}
static void poll(void)
{
    uint8_t *p=reserve();if(!p)return;
    const uint8_t header[]={0x2a,0,5,0,0,9,0x31};
    memcpy(p,header,sizeof(header));
    if(present && poll_state) {
        poll_state=seen?9:1;seen=true;
        const uint8_t found[]={0,0,0,1,1,2,0,7};
        memcpy(p+8,found,sizeof(found));uid(p+16);
    }
    p[7]=poll_state;finish(p);
}
static void read_tag(void)
{
    /* All three fragments are reserved together; polling cannot evict them. */
    flush();
    uint8_t *p=reserve();
    const uint8_t wire[]={0x3a,0x00,0x07,0x01,0x00,0x01,0x31,0x02,0x00,0x00,0x00,0x01,0x02,0x00,0x07};
    memcpy(p,wire,sizeof(wire));
    uid(p+sizeof(wire));
    const uint8_t meta[]={
        0,0,0,0,0x7d,0xfd,0xf0,0x79,0x36,0x51,0xab,0xd7,0x46,0x6e,
        0x39,0xc1,0x91,0xba,0xbe,0xb8,0x56,0xce,0xed,0xf1,0xce,0x44,
        0xcc,0x75,0xea,0xfb,0x27,9,0x4d,8,0x7a,0xe8,3,0,0x3b,0x3c,
        0x77,0x78,0x86,0,0
    };
    memcpy(p+sizeof(wire)+7,meta,sizeof(meta));
    memcpy(p+sizeof(wire)+7+sizeof(meta),tag,245);
    finish(p);
    p=reserve();
    const uint8_t second[]={0x3a,0,7,2,0,9,0x27};
    memcpy(p,second,sizeof(second));memcpy(p+7,tag+245,295);finish(p);
    p=reserve();
    const uint8_t tail[]={0x2a,0,5,0,0,9,0x31,4,0,0,0,1,1,2,0,7};
    memcpy(p,tail,sizeof(tail));uid(p+sizeof(tail));finish(p);
}
static bool handle_command(const uint8_t *packet,size_t len)
{
    if(!packet || len<11 || packet[0]!=0x11) return false;
    if(packet[10]==1) {status();return true;}
    if(packet[10]!=2 || len<12 || power_state!=4) return false;
    const uint8_t *data=packet+12;
    size_t size=len-12;
    switch(packet[11]) {
    case 1: poll_state=1;return true;
    case 2: poll_state=0;seen=false;flush();return true;
    case 4: poll();return true;
    case 6:
        if(size<13 || !present) return false;
        for(unsigned i=6;i<13;i++) if(data[i]) return false; /* write setup unsupported */
        read_tag();return true;
    default: return false; /* No fake success for unsupported writes. */
    }
}
bool nfc_command(const uint8_t *packet,size_t len)
{
    received++;
    last_cmd=packet && len>10?packet[10]:0;
    last_sub=packet && len>11?packet[11]:0;
    bool ok=handle_command(packet,len);
    if(!ok) rejected++;
    return ok;
}
void nfc_peek(uint8_t out[NFC_MCU_SIZE])
{
    if(count) memcpy(out,queue[head],NFC_MCU_SIZE);
    else {memset(out,0,NFC_MCU_SIZE);out[0]=0xff;finish(out);}
}
void nfc_commit(void)
{
    if(count) {head=(head+1)%4;count--;}
}
