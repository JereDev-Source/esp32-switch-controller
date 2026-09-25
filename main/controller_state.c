/**
 * @file controller_state.c
 * @brief Implementación del estado del controlador.
 */
#include "controller_state.h"
#include <string.h>

/* Estado global del controlador */
static controller_state_t s_ctrl_state;

void controller_state_init(controller_state_t *state)
{
    memset(state, 0, sizeof(*state));
    /* Centro del stick: 0x800 (2048 de 4095) */
    state->left_stick_x  = 0x800;
    state->left_stick_y  = 0x800;
    state->right_stick_x = 0x800;
    state->right_stick_y = 0x800;
}

controller_state_t *controller_state_get(void)
{
    return &s_ctrl_state;
}
