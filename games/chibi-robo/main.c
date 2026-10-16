/* Entry point of Chibi-Robo!: the whole boot sequence is generic (runtime/core/boot).
 * Game-specific function replacements and patches will be installed in `install`. */
#include "boot/gcrt_boot.h"
#include "hle/gcrt_native.h"
#include "platform/gcrt_user_dir.h"

static const GcrtGameDesc kChibiRobo = {
    "Chibi-Robo!",
    NULL, /* install: no patch for now */
};

int main(int argc, char** argv) {
    /* User folder (gamepad settings, then saves): on Windows %APPDATA%\Chibi Robo PC\Chibi Robo\. Letters, digits
     * and spaces only, as SDL advises for these names (SDL_GetPrefPath). Changing them moves the player's settings
     * and saves to another folder (notes/decisions.md). */
    gcrt_set_user_identity("Chibi Robo PC", "Chibi Robo");
    /* Native memory card, always inserted (milestone 7): a 251-block (16 Mbit) card, the console's official card;
     * the game's 10 slots (6 blocks each) fit on it. Saves go to the "Saved Games" folder:
     * %USERPROFILE%\Saved Games\Chibi Robo PC\Chibi Robo\ (notes/decisions.md). */
    gcrt_native_card_set_size(16);

    //put setup here
    return gcrt_main(argc, argv, &kChibiRobo);
}
