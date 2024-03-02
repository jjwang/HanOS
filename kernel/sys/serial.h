#pragma once

#define SERIAL_PORT     0x3F8

int serial_init();
void serial_write(char a);
void serial_puts(char *s);
