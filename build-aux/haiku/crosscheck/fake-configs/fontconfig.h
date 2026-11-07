/* Harness stub of the fontconfig API subset used by OBS (real header not reachable from this environment). */
#ifndef _FONTCONFIG_H_
#define _FONTCONFIG_H_
#define FC_MAJOR 2
#define FC_MINOR 15
#define FC_REVISION 0
#define FC_VERSION ((FC_MAJOR * 10000) + (FC_MINOR * 100) + (FC_REVISION))
typedef unsigned char FcChar8;
typedef int FcBool;
typedef struct _FcPattern FcPattern;
typedef struct _FcConfig FcConfig;
typedef enum _FcResult { FcResultMatch, FcResultNoMatch, FcResultTypeMismatch, FcResultNoId, FcResultOutOfMemory } FcResult;
typedef enum _FcMatchKind { FcMatchPattern, FcMatchFont, FcMatchScan } FcMatchKind;
#define FC_FAMILY "family"
#define FC_STYLE "style"
#define FC_SLANT "slant"
#define FC_WEIGHT "weight"
#define FC_SIZE "size"
#define FC_INDEX "index"
#define FC_FILE "file"
#define FC_WEIGHT_REGULAR 80
#define FC_WEIGHT_BOLD 200
#define FC_SLANT_ROMAN 0
#define FC_SLANT_ITALIC 100
#ifdef __cplusplus
extern "C" {
#endif
FcBool FcInit(void);
void FcFini(void);
FcPattern *FcPatternCreate(void);
void FcPatternDestroy(FcPattern *p);
FcBool FcPatternAddString(FcPattern *p, const char *object, const FcChar8 *s);
FcBool FcPatternAddInteger(FcPattern *p, const char *object, int i);
FcBool FcPatternAddDouble(FcPattern *p, const char *object, double d);
FcResult FcPatternGetInteger(const FcPattern *p, const char *object, int n, int *i);
FcResult FcPatternGetString(const FcPattern *p, const char *object, int n, FcChar8 **s);
FcBool FcConfigSubstitute(FcConfig *config, FcPattern *p, FcMatchKind kind);
void FcDefaultSubstitute(FcPattern *pattern);
FcPattern *FcFontMatch(FcConfig *config, FcPattern *p, FcResult *result);
FcChar8 *FcPatternFormat(FcPattern *pat, const FcChar8 *format);
void FcStrFree(FcChar8 *s);
#ifdef __cplusplus
}
#endif
#endif
