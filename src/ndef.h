#pragma once
#include <stddef.h>
#include <stdint.h>
#include "core.h"
#define NDEF_TITLE_SIZE 28
int ndef_encode_title(const char *title,uint8_t out[NDEF_TITLE_SIZE]);
int ndef_decode_title(const uint8_t *data,size_t len,char title[TITLE_CAP]);
