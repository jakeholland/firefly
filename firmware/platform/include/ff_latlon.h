/**
 * ff_latlon.h — shared WGS84 geodetic coordinate type.
 *
 * Lives under firmware/platform/ rather than firmware/core/ for the same
 * reason ff_clock_t does (see ff_clock.h): extraction-grade libraries
 * (meshclient) can't include core/ (docs/specs/S03-meshclient.md AC7), but
 * still need the exact {lat, lon} shape core/geo (S01) and festpack (S05)
 * use. A tiny platform/ header is the one place both extraction-grade and
 * core-linked modules can depend on it without either depending on the
 * other — see docs/ARCHITECTURE.md's shared-seam note.
 *
 * Wave 1 (S01/S03/S05) shipped this same struct defined independently
 * three times, deliberately, to avoid coupling parallel work; this header
 * is the reconciliation.
 */
#ifndef FF_LATLON_H
#define FF_LATLON_H

#ifdef __cplusplus
extern "C" {
#endif

/** A WGS84 geodetic coordinate, degrees. */
typedef struct {
    double lat;
    double lon;
} ff_latlon_t;

#ifdef __cplusplus
}
#endif

#endif /* FF_LATLON_H */
