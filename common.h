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

// HM_ERROR_TOO_MANY_POPULAR_DOMAINS is returned by static domain set if there
// are too many popular domains in the list.
#define HM_ERROR_TOO_MANY_POPULAR_DOMAINS (7)

// HM_ERROR_FAILED_TO_CALIBRATE is returned by static domain set if failed to
// calibrate the hash table after all the iterations.
#define HM_ERROR_FAILED_TO_CALIBRATE (8)

// HM_ERROR_TOP_LEVEL_DOMAIN is returned by static domain set compile when
// an input pattern is a top-level domain (contains no dot). Such patterns
// are not supported by the fast lookup path.
#define HM_ERROR_TOP_LEVEL_DOMAIN (9)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // HM_COMMON_H
