#include "../main/nfc_mcu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void checksum(const uint8_t *p) {assert(p[312]==nfc_crc8(p,312));}
int main(void)
{
    uint8_t image[540], packet[49]={0x11}, out[313], again[313], reply[34];
    for(unsigned i=0;i<540;i++) image[i]=(uint8_t)(i*13+7);
    image[0]=4;
    assert(nfc_crc8((const uint8_t *)"123456789",9)==0xf4);
    nfc_unload();nfc_reset();nfc_peek(out);assert(out[0]==0xff);checksum(out);
    assert(!nfc_load(image,539));assert(nfc_load(image,540));
    assert(!nfc_power(2));assert(nfc_power(1));
    nfc_peek(out);assert(out[0]==1 && out[7]==1);checksum(out);nfc_commit();
    uint8_t cfg[]={0x21,0,4};
    assert(nfc_load(image,540));nfc_peek(out);assert(out[0]==1 && out[7]==1);nfc_commit();
    assert(!nfc_config(cfg,2,reply));assert(nfc_config(cfg,3,reply));
    assert(reply[33]==0xc8);
    nfc_peek(out);assert(out[7]==4);nfc_commit();
    for(unsigned i=0;i<11;i++) assert(!nfc_command(packet,i));
    packet[10]=2;packet[11]=1;assert(nfc_command(packet,49));
    packet[11]=4;assert(nfc_command(packet,49));
    nfc_peek(out);assert(out[0]==0x2a && out[7]==1 && out[15]==7);
    assert(!memcmp(out+16,image,3));assert(!memcmp(out+19,image+4,4));
    checksum(out);nfc_commit();
    assert(nfc_command(packet,49));nfc_peek(out);assert(out[7]==9);nfc_commit();
    packet[11]=6;
    assert(!nfc_command(packet,24));assert(nfc_command(packet,49));
    nfc_peek(out);nfc_peek(again);assert(!memcmp(out,again,313)); /* retry */
    assert(out[0]==0x3a && out[3]==1);
    assert(!memcmp(out+67,image,245));checksum(out);nfc_commit();
    nfc_peek(out);assert(out[3]==2);assert(!memcmp(out+7,image+245,295));checksum(out);nfc_commit();
    nfc_peek(out);assert(out[0]==0x2a && out[7]==4);checksum(out);nfc_commit();
    nfc_peek(out);assert(out[0]==0xff);
    /* No acknowledgement of an unsupported write or setup. */
    packet[18]=1;assert(!nfc_command(packet,49));packet[18]=0;
    packet[11]=8;assert(!nfc_command(packet,49));
    packet[11]=6;assert(nfc_command(packet,49));nfc_unload();
    nfc_peek(out);assert(out[0]==0x2a && out[15]==0);checksum(out);nfc_commit();
    assert(nfc_load(image,540));nfc_peek(out);assert(out[0]==0x2a && out[15]==7);checksum(out);nfc_commit();
    nfc_unload();nfc_commit();
    packet[11]=4;assert(nfc_command(packet,49));nfc_peek(out);assert(out[15]==0);nfc_commit();
    assert(nfc_load(image,540));nfc_reset();assert(nfc_power(1));assert(nfc_config(cfg,3,reply));
    packet[11]=6;assert(nfc_command(packet,49)); /* contents survive reconnect */
    packet[11]=2;assert(nfc_command(packet,49));nfc_peek(out);assert(out[0]==0xff);
    puts("PASS: CRC, MCU config, UID, 540-byte read, retries, bounds, unload, reconnect, write rejection");
    return 0;
}
