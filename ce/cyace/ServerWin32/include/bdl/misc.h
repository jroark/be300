// common.h

#define MAX_CHARS(b)  (sizeof(b) - 1)
#define MAX_WCHARS(b) ((sizeof(b) / sizeof(WCHAR)) - 1)

#ifdef UNICODE
#define MAX_TCHARS(b) MAX_WCHARS(b)
#else
#define MAX_TCHARS(b) MAX_CHARS(b)
#endif
