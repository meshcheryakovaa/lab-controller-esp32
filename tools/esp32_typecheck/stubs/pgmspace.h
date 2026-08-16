#pragma once
#include <cstring>
#define PROGMEM
#define PGM_P const char*
#define PSTR(s) (s)
#define F(s) (s)
typedef const char* __FlashStringHelper;
#define pgm_read_byte(addr) (*(const unsigned char*)(addr))
#define pgm_read_word(addr) (*(const unsigned short*)(addr))
#define pgm_read_dword(addr) (*(const unsigned long*)(addr))
#define pgm_read_ptr(addr) (*(void* const*)(addr))
