#ifndef HM_COMMON_H
#define HM_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define HM_CDECL __cdecl
#else
#define HM_CDECL
#endif

#if !defined(_WIN32)
#define HM_PUBLIC_API __attribute__((visibility("default")))
#else
// TODO: dllexport defines for windows
#define HM_PUBLIC_API
#endif

// hm_error_t is error type returned by some functions of hipermap.
typedef int hm_error_t;

// HM_SUCCESS is returned in case of success.
#define HM_SUCCESS (0)

// HM_ERROR_BAD_ALIGNMENT is returned if db_place is not 8 byte aligned.
#define HM_ERROR_BAD_ALIGNMENT (1)

// HM_ERROR_SMALL_PLACE is returned if db_place is too small.
#define HM_ERROR_SMALL_PLACE (2)

// HM_ERROR_NO_MASKS is returned if the number of masks is 0.
#define HM_ERROR_NO_MASKS (3)

// HM_ERROR_BAD_VALUE is returned if the value is invalid.
#define HM_ERROR_BAD_VALUE (4)

// HM_ERROR_BAD_RANGE is returned if IP range has non-zero bits after prefix.
#define HM_ERROR_BAD_RANGE (5)

// HM_ERROR_BAD_SIZE is returned if size value is incorrect.
#define HM_ERROR_BAD_SIZE (6)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // HM_COMMON_H
