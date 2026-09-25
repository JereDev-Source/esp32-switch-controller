#include "pc_control.h"
#include "controller_state.h"
#include "input_report.h"
#include "nfc_mcu.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define LINE_SIZE 1400
static QueueHandle_t commands;
static int64_t last_state;
static bool linked;

static bool tag_loaded;
static uint32_t tag_crc;

/* Only the BTstack run loop accesses controller and tag state. */
static void answer(int id, bool ok, const char *message)
{
    nfc_diagnostics_t d=nfc_diagnostics();
    printf("@PC {\"id\":%d,\"ok\":%s,\"message\":\"%s\",\"connected\":%s,\"loaded\":%s,\"nfc_ready\":true,\"nfc_write\":false,\"crc\":%lu,\"mcu_power\":%u,\"nfc_poll\":%u,\"nfc_queue\":%u,\"nfc_rx\":%u,\"nfc_rejected\":%u,\"nfc_last_cmd\":%u,\"nfc_last_sub\":%u}\n",
           id, ok ? "true" : "false", message, linked ? "true" : "false",
           tag_loaded ? "true" : "false", (unsigned long)tag_crc,d.power,d.poll,d.pending,d.received,d.rejected,d.last_cmd,d.last_sub);
}
static void release(void)
{
    controller_state_init(controller_state_get());
    input_report_set_virtual_grip(false);
    last_state = 0;
}
void pc_control_connection(bool connected)
{
    linked = connected;
    release();
}
static bool number(cJSON *o, const char *key, unsigned max, unsigned *result)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < 0 || v->valuedouble > max ||
        v->valuedouble != floor(v->valuedouble)) return false;
    *result = (unsigned)v->valuedouble;
    return true;
}
static int nibble(char c)
{
    if(c >= '0' && c <= '9') return c-'0';
    if(c >= 'a' && c <= 'f') return c-'a'+10;
    if(c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
static uint32_t crc32(const uint8_t *bytes, size_t len)
{
    uint32_t crc = 0xffffffff;
    for(size_t i=0;i<len;i++) {
        crc ^= bytes[i];
        for(int b=0;b<8;b++) crc=(crc>>1)^((crc&1)?0xedb88320:0);
    }
    return crc ^ 0xffffffff;
}
static void command(const char *line)
{
    cJSON *o=cJSON_Parse(line);
    unsigned id=0;
    if (!o || !number(o,"id",2147483647,&id)) {
        answer(0,false,"Invalid JSON or id"); cJSON_Delete(o); return;
    }
    cJSON *cmd=cJSON_GetObjectItemCaseSensitive(o,"cmd");
    if (!cJSON_IsString(cmd)) { answer(id,false,"Missing cmd"); goto done; }
    if (!strcmp(cmd->valuestring,"status")) answer(id,true,"PC controller v1");
    else if (!strcmp(cmd->valuestring,"release")) { release(); answer(id,true,"Released"); }
    else if (!strcmp(cmd->valuestring,"state")) {
        unsigned r,s,l,lx,ly,rx,ry;
        if (!number(o,"r",255,&r)||!number(o,"s",63,&s)||!number(o,"l",255,&l)||
            !number(o,"lx",4095,&lx)||!number(o,"ly",4095,&ly)||
            !number(o,"rx",4095,&rx)||!number(o,"ry",4095,&ry)) {
            answer(id,false,"Invalid controller state"); goto done;
        }
        if (!linked) { release(); answer(id,false,"Switch disconnected"); goto done; }
        controller_state_t *state=controller_state_get();
        state->right_buttons=r; state->shared_buttons=s; state->left_buttons=l;
        state->left_stick_x=lx; state->left_stick_y=ly;
        state->right_stick_x=rx; state->right_stick_y=ry;
        last_state=esp_timer_get_time();
        answer(id,true,"State applied");
    } else if (!strcmp(cmd->valuestring,"load")) {
        cJSON *hex=cJSON_GetObjectItemCaseSensitive(o,"hex");
        unsigned expected;
        uint8_t candidate[540];
        if (!cJSON_IsString(hex)||strlen(hex->valuestring)!=1080||
            !number(o,"crc",0xffffffffu,&expected)) {
            answer(id,false,"Expected 540-byte raw BIN and CRC32"); goto done;
        }
        for(unsigned i=0;i<540;i++) {
            int hi=nibble(hex->valuestring[i*2]),lo=nibble(hex->valuestring[i*2+1]);
            if(hi<0||lo<0) { answer(id,false,"Invalid hex"); goto done; }
            candidate[i]=(hi<<4)|lo;
        }
        uint32_t actual=crc32(candidate,sizeof(candidate));
        if(actual!=expected) { answer(id,false,"CRC mismatch"); goto done; }
        /* Structural validation only: no authenticity/cryptographic claim. */
        if(candidate[0]!=0x04 || candidate[3]!=(0x88^candidate[0]^candidate[1]^candidate[2]) ||
           candidate[8]!=(candidate[4]^candidate[5]^candidate[6]^candidate[7])) {
            answer(id,false,"Invalid NTAG UID/BCC"); goto done;
        }
        nfc_load(candidate,sizeof(candidate)); tag_crc=actual; tag_loaded=true;
        answer(id,true,"BIN presented; NFC read experimental");
    } else if (!strcmp(cmd->valuestring,"unload")) {
        nfc_unload(); tag_loaded=false; tag_crc=0;
        answer(id,true,"BIN removed");
    } else answer(id,false,"Unknown command");
done:
    cJSON_Delete(o);
}
void pc_control_poll(void)
{
    char line[LINE_SIZE];
    for(int i=0;i<4 && xQueueReceive(commands,line,0)==pdTRUE;i++) command(line);
    if(last_state && esp_timer_get_time()-last_state>500000) release();
}
static void reader(void *unused)
{
    (void)unused;
    char line[LINE_SIZE];
    unsigned used=0;
    bool overflow=false;
    while(1) {
        uint8_t c;
        if(uart_read_bytes(UART_NUM_0,&c,1,pdMS_TO_TICKS(100))<=0) continue;
        if(c=='\r') continue;
        if(c=='\n') {
            if(!overflow && used) {
                line[used]=0;
                /* Fail closed: a missing state refresh releases after 500ms. */
                xQueueSend(commands,line,0);
            }
            used=0; overflow=false;
        } else if(used<LINE_SIZE-1 && !overflow) line[used++]=(char)c;
        else overflow=true;
    }
}
esp_err_t pc_control_init(void)
{
    commands=xQueueCreate(4,LINE_SIZE);
    if(!commands) return ESP_ERR_NO_MEM;
    esp_err_t err=uart_driver_install(UART_NUM_0,4096,0,0,NULL,0);
    if(err!=ESP_OK) { vQueueDelete(commands); commands=NULL; return err; }
    if(xTaskCreate(reader,"pc_uart",4096,NULL,4,NULL)!=pdPASS) {
        uart_driver_delete(UART_NUM_0); vQueueDelete(commands); commands=NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
