/**-----------------------------------------------------------------------------

 @file    input_srv.h
 @brief   Spawn and drive the userspace input server

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

/* Create the IRQ/endpoint resources, spawn the input server and start the
 * kernel relay task. Returns false when the server could not be started, in
 * which case the in-kernel keyboard driver keeps handling the interrupt. */
bool input_server_start(void);

bool input_server_active(void);
