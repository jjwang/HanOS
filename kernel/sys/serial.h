/**-----------------------------------------------------------------------------

 @file    serial.h
 @brief   Definitions for serial port communication
 @details
 @verbatim

  This file contains the function declarations and constants used for serial
  port communication within the HanOS kernel. It includes the initialization
  function for the serial port, and functions for writing individual characters
  and strings to the serial port. The serial port base address is also defined.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define SERIAL_PORT     0x3F8

void serial_init();
void serial_write(char a);
void serial_puts(char *s);
void serial_flush();
