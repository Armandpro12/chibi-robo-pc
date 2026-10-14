/*
 * User folders: settings (controls.ini) and saves (milestone 7).
 *
 * Generic: the folder names are given by the game (games/<game>/main.c), never by the core.
 * - Settings: SDL_GetPrefPath(org, app) from SDL 3 (SDL_filesystem.h), which creates the folder; on Windows
 *   "%APPDATA%\<org>\<app>\".
 * - Saves: the user's "Saved Games" folder (SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES):
 *   Windows FOLDERID_SavedGames, "%USERPROFILE%\Saved Games") followed by "<org>\<app>\"; where that folder
 *   does not exist (outside Windows), "<settings folder>/saves/". Choices and sources: notes/decisions.md
 *   (2026-10-11, milestone 7).
 * GCRT_USER_DIR variable: another folder for both (settings in <GCRT_USER_DIR>, saves in
 * <GCRT_USER_DIR>/saves/), created if needed: tests and automatic launches, which thus never write
 * into the user's real folders.
 */
#ifndef GCRT_USER_DIR_H
#define GCRT_USER_DIR_H

#ifdef __cplusplus
extern "C" {
#endif

/* To be called by the game before gcrt_main. `org` and `app`: letters, digits and spaces only (SDL
   guideline), never to be changed afterwards (the folder depends on them). */
void gcrt_set_user_identity(const char* org, const char* app);

/* User folder in UTF-8, ending with a separator; empty string if it is unavailable (the
   settings then keep their default values). Resolved on the first call, after the window (SDL)
   is initialized, then kept. Without an identity given by the game: org "gcrecomp", app "gcrt". */
const char* gcrt_user_dir(void);

/* Saves folder in UTF-8, ending with a separator (created by the first one that writes to it); empty string if it
   is unavailable. Resolved on the first call, then kept. */
const char* gcrt_save_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_USER_DIR_H */
