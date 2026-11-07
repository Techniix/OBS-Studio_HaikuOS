/* harness shim: Haiku's libroot lacks quick_exit, which host libstdc++ <cstdlib> expects */
#include_next <stdlib.h>
#ifndef HCHECK_QUICK_EXIT
#define HCHECK_QUICK_EXIT
#ifdef __cplusplus
extern "C" {
#endif
int at_quick_exit(void (*)(void));
void quick_exit(int) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#endif
