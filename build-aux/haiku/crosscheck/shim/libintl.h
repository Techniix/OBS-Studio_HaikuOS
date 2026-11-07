/* harness shim: the host libstdc++ is configured with NLS and includes <libintl.h>; Haiku's isn't. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
char *gettext(const char *);
char *dgettext(const char *, const char *);
char *dcgettext(const char *, const char *, int);
char *textdomain(const char *);
char *bindtextdomain(const char *, const char *);
char *bind_textdomain_codeset(const char *, const char *);
#ifdef __cplusplus
}
#endif
