/*
 * ps3recomp - trophy set data read from the player's own game files (host/trophies, 2026-11-22)
 *
 * The trophy list (ids, grades, hidden flags, names, descriptions, icons) is read at run time from the disc's
 * PS3_GAME/TROPDIR/<NPCOMMID>/TROPHY.TRP (or an installed update's copy, the newer trophy-set version wins) and
 * copied into <trophy storage>/<NPCOMMID>/ for the launcher and the in-game popup. Nothing of it is shipped.
 * No data found -> np_trophy_data_count() == 0 and sceNpTrophy behaves as before.
 */
#ifndef PS3RECOMP_NP_TROPHY_DATA_H
#define PS3RECOMP_NP_TROPHY_DATA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_TROPHY_DATA_MAX 128

typedef struct np_trophy_def {
    int  id;
    char grade;          /* 'P' platinum, 'G' gold, 'S' silver, 'B' bronze */
    int  hidden;
    int  pid;            /* the platinum it counts for (-1: none) */
    char name[160];      /* UTF-8, in the console language when that file exists */
    char detail[320];
} np_trophy_def;

/* Loads the set for "<commId>_<num 2 digits>" (e.g. NPWR00802_00); cache_root = the trophy storage folder.
 * Returns the number of trophies (0: no usable data). Safe to call more than once (the first result stays). */
int  np_trophy_data_load(const char* npcommid_full, const char* cache_root);
int  np_trophy_data_count(void);
const np_trophy_def* np_trophy_data_get(int id);
int  np_trophy_data_platinum(void);                    /* the platinum's id, -1 when the set has none */
const char* np_trophy_data_set_version(void);          /* "01.01"... or "" */
/* The trophy's icon in the cache (TROP###.PNG), "" when there is none. */
void np_trophy_data_icon_path(int id, char* out, size_t cap);

/* <tmp> replaces <dst> (atomic on Windows: MoveFileEx REPLACE_EXISTING | WRITE_THROUGH). 1 = done. */
int  np_trophy_replace_file(const char* tmp, const char* dst);
/* One lock for the trophy state (the game's unlock thread, test unlocks). */
void np_trophy_lock(void);
void np_trophy_unlock(void);

/* Unlock events for the in-game popup (a small ring; the oldest are dropped when it is full). */
typedef struct np_trophy_event { int id; char grade; unsigned long long unix_time; } np_trophy_event;
void np_trophy_event_push(int id, unsigned long long unix_time);
int  np_trophy_event_take(np_trophy_event* out);       /* 1 = one taken */
/* A hook for a later "trophy unlocked" sound (none today): called on every push, from the game's thread. */
extern void (*g_np_trophy_sound_hook)(int id, char grade);

#ifdef __cplusplus
}
#endif

#endif
