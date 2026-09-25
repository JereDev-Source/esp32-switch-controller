#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define NFC_MCU_SIZE 313
#define NFC_TAG_SIZE 540
void nfc_reset(void);
bool nfc_load(const uint8_t *data, size_t len);
void nfc_unload(void);
bool nfc_power(uint8_t value);
bool nfc_config(const uint8_t *args, size_t len, uint8_t reply[34]);
bool nfc_command(const uint8_t *packet, size_t len);
void nfc_peek(uint8_t out[NFC_MCU_SIZE]);
void nfc_commit(void);
uint8_t nfc_crc8(const uint8_t *data, size_t len);

typedef struct {
    uint8_t power, poll, last_cmd, last_sub;
    unsigned pending, received, rejected;
    bool present;
} nfc_diagnostics_t;
nfc_diagnostics_t nfc_diagnostics(void);
