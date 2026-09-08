/**
 * ff_build_info.h — DIAGNOSTICS: the firmware's own git SHA / build date,
 * as compile-time defines.
 *
 * No such identifier existed anywhere in this codebase before this file
 * (grepped: GIT_SHA/BUILD_DATE/__DATE__/FF_VERSION/FF_BUILD_ all came up
 * empty outside `ff_version.h`'s own unrelated S13/S14 placeholder
 * string) — the DIAGNOSTICS page's own spec calls for "firmware git
 * SHA/build date (a compile-time define; add one if none exists)", so
 * this is that define.
 *
 * Both macros are overridable via `-D` at configure time (see
 * `firmware/app/CMakeLists.txt`'s ff-shell target, and the esp32s3
 * target's mirror in `targets/esp32s3/components/ff_app/CMakeLists.txt`
 * — both run `git rev-parse`/`git show` against the commit HEAD points
 * at when a working `git` + repo is available) — the `#ifndef` guards
 * below are the honest fallback for a build with no git metadata
 * available (a source tarball, a CI checkout with history stripped, or
 * simply CMake not having been re-run since this header landed):
 * "unknown" rather than a fabricated SHA, and the C
 * preprocessor's own `__DATE__` (compile time, not commit time — an
 * honest "unknown" would be more correct here too, but `__DATE__`
 * remains directly useful for a from-source build with no git metadata
 * at all, and is the same tradeoff `ff_wall.h`'s own FF_WALL_EPOCH_FLOOR
 * comment discusses for why a build-date-shaped fact is being used at
 * all here — see that header's MAINTENANCE note for the general
 * caution against date-driven values; this one is DISPLAY ONLY, no
 * gate/test logic depends on it, so the reproducible-build concern that
 * comment raises does not apply).
 *
 * DIAGNOSTICS never computes these live from a fixture — a fixture sets
 * `ff_app_diag_t.fw_git_sha`/`fw_build_date` directly (see
 * `firmware/targets/sim/fixture.c`), so goldens are unaffected by
 * whatever these macros happen to expand to on the machine that renders
 * them.
 */
#ifndef FF_BUILD_INFO_H
#define FF_BUILD_INFO_H

#ifndef FF_BUILD_GIT_SHA
#define FF_BUILD_GIT_SHA "unknown"
#endif

#ifndef FF_BUILD_DATE
#define FF_BUILD_DATE __DATE__
#endif

#endif /* FF_BUILD_INFO_H */
