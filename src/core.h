#pragma once
#include <stddef.h>
#include <stdint.h>
#define MAX_MAPS 128
#define UID_CAP 21
#define TITLE_CAP 10
#define NAME_CAP 96
#define VERSION "0.2.0"
typedef struct { char uid[UID_CAP], title[TITLE_CAP], name[NAME_CAP]; } Mapping;
typedef struct { Mapping maps[MAX_MAPS]; int count, hold; unsigned removal_ms; } Config;
typedef struct {
    char present[UID_CAP], last_uid[UID_CAP], blocked_uid[UID_CAP], owned_title[TITLE_CAP];
    unsigned owned_app;
    int session_hold, reader_ready, owned_manual;
    uint64_t absent_since;
} Session;
typedef int (*LaunchFn)(const char *, unsigned *);
typedef int (*CloseFn)(unsigned);
uint64_t now_ms(void);
int valid_uid(const char *s);
int valid_title(const char *s);
void session_scan(Session *s, const Config *c, const char *uid, uint64_t now, LaunchFn launch, CloseFn close_app);
void session_disconnect(Session *s);
void session_forget(Session *s);
int pn532_frame(uint8_t cmd, const uint8_t *data, size_t len, uint8_t *out, size_t cap);
int pn532_parse(const uint8_t *buf, size_t len, uint8_t cmd, uint8_t *out, size_t cap, size_t *used);
