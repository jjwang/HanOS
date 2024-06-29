#pragma once

#define SERIAL_PORT     0x3F8

void serial_init();
void serial_write(char a);
void serial_puts(char *s);
