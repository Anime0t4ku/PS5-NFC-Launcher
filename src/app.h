#pragma once
#include "core.h"
#include <pthread.h>
extern pthread_mutex_t state_lock;
extern Config config;
extern Session session;
extern char reader_status[256], action_status[256], usb_inventory[2048];
extern int launch_available, close_available;
int sony_init(void);
int sony_launch(const char *title, unsigned *app);
int sony_close(unsigned app);
int config_save(const Config *c);
void config_load(void);
void *http_main(void *arg);
void *reader_main(void *arg);
void action_message(const char *fmt, ...);
void reader_message(const char *fmt, ...);
int enqueue_action(const char *title, int close_owned);
void process_action(void);
extern char card_title[TITLE_CAP],write_status[256];
int arm_write(const char *title);
int get_write_request(char title[TITLE_CAP]);
void finish_write(const char *message);
int http_prepare(void);
extern int operation_busy;
