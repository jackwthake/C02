#ifndef __COLORS_H__
#define __COLORS_H__

#ifdef _WIN32
  #include <windows.h>
  #define ENABLE_COLORS() do { \
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); \
    DWORD mode; \
    GetConsoleMode(h, &mode); \
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); \
  } while(0)
#else
  #define ENABLE_COLORS() ((void)0)
#endif

// Reset
#define RESET   "\033[0m"

// Regular colors
#define BLACK   "\033[30m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"

// Bold
#define BOLD_BLACK   "\033[1;30m"
#define BOLD_RED     "\033[1;31m"
#define BOLD_GREEN   "\033[1;32m"
#define BOLD_YELLOW  "\033[1;33m"
#define BOLD_BLUE    "\033[1;34m"
#define BOLD_MAGENTA "\033[1;35m"
#define BOLD_CYAN    "\033[1;36m"
#define BOLD_WHITE   "\033[1;37m"

// Bright
#define BRIGHT_BLACK   "\033[90m"
#define BRIGHT_RED     "\033[91m"
#define BRIGHT_GREEN   "\033[92m"
#define BRIGHT_YELLOW  "\033[93m"
#define BRIGHT_BLUE    "\033[94m"
#define BRIGHT_MAGENTA "\033[95m"
#define BRIGHT_CYAN    "\033[96m"
#define BRIGHT_WHITE   "\033[97m"

#endif