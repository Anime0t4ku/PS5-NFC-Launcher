#pragma once
#include "core.h"
#define MAX_GAMES 512
typedef struct { char title[TITLE_CAP],name[256]; int icon; } Game;
int games_list(Game *games,int max,char *source,size_t cap);
int game_icon(const char *title,char *path,size_t cap);
