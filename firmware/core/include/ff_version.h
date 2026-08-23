/**
 * ff_version.h — firefly-core version string.
 *
 * Placeholder unit for the S13/S14 walking skeleton: proves the core
 * library builds, links, and has a passing unit test wired into ctest.
 * Pure C11, no I/O, no allocation.
 */
#ifndef FF_VERSION_H
#define FF_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_version_string — return firefly-core's version identifier.
 *
 * Returns a pointer to a static, NUL-terminated string owned by the
 * library. The caller must not free or modify it.
 */
const char *ff_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* FF_VERSION_H */
