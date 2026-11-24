/*
 * termica - keyboard, text/IME, pointer, focus and clipboard-policy routing.
 */
#ifndef TERMICA_INPUT_H
#define TERMICA_INPUT_H

#include "app.h"

void input_handle_key_down(App *app, const SDL_KeyboardEvent *event);
void input_handle_text_input(App *app, const SDL_TextInputEvent *event);
void input_handle_text_editing(App *app, const SDL_TextEditingEvent *event);
void input_handle_mouse_button(App *app, const SDL_MouseButtonEvent *event);
void input_handle_mouse_motion(App *app, const SDL_MouseMotionEvent *event);
void input_handle_mouse_wheel(App *app, const SDL_MouseWheelEvent *event);
void input_handle_focus(App *app, bool gained);

void input_reset_mouse_shape(App *app);
void input_autoscroll_start(App *app, int direction);
void input_autoscroll_tick(App *app);

/* OSC 52 / iTerm2 / kitty clipboard write callback installed on the session's
 * terminal; userdata is App *. */
TermClipboardResult input_handle_clipboard_write(void *userdata,
                                                 const char *text,
                                                 size_t length,
                                                 const char *name);

#endif /* TERMICA_INPUT_H */
