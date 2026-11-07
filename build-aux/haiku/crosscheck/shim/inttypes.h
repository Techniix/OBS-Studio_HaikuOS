/* harness shim: host libstdc++'s <cinttypes> expects wcstoimax/wcstoumax, which Haiku's libroot lacks
 * (Haiku's own libstdc++ is configured without them). */
#include_next <inttypes.h>
#ifndef HCHECK_WCSTOIMAX
#define HCHECK_WCSTOIMAX
#ifdef __cplusplus
extern "C" {
#endif
intmax_t wcstoimax(const wchar_t *, wchar_t **, int);
uintmax_t wcstoumax(const wchar_t *, wchar_t **, int);
#ifdef __cplusplus
}
#endif
#endif
