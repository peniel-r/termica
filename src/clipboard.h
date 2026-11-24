#ifndef TERMICA_CLIPBOARD_H
#define TERMICA_CLIPBOARD_H

#include "term.h"

TermClipboardResult clipboard_handle_read(void *userdata, const char *name,
                                           bool granted, char **text);

#endif
