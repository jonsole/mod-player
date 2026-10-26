/* The status screen on the HDMI console (issue #22): the Song's name,
 * where it is (Position, Pattern, Row), speed and tempo, and its Sample
 * names. Drawn into a copy in RAM; only changed cells go to the console,
 * and only in the vertical blanking, so the screen never shows half an
 * update and the Replayer never waits for it. */
#ifndef STATUS_H
#define STATUS_H

/* Call often (the main loop): follows the player, and redraws the whole
 * screen when a new Song is loaded. Does nothing until the first Song, so
 * the power-on test screen stays up till then. */
void status_update(void);

#endif
