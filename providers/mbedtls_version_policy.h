#ifndef MAELYS_HTTP_MBEDTLS_VERSION_POLICY_H
#define MAELYS_HTTP_MBEDTLS_VERSION_POLICY_H

#if !defined(MBEDTLS_VERSION_MAJOR) || !defined(MBEDTLS_VERSION_MINOR) || \
    !defined(MBEDTLS_VERSION_PATCH)
#error "Mbed TLS version macros are required"
#endif

#if !defined(MBEDTLS_VERSION_C)
#error "MBEDTLS_VERSION_C is required for runtime downgrade protection"
#endif

/* The floor of each maintained line is its release carrying the fixes of the
 * 2026-07 security advisories: 3.6.7 on the 3.6 LTS line, 4.1.1 on the 4.1
 * LTS line, and 4.2.0, the first release of the 4.2 line. Later minor
 * releases of a maintained major line are accepted. */
#define MAELYS_HTTP_MBEDTLS_SECURE_VERSION(major, minor, patch) \
    (((major) == 3 && ((minor) > 6 || \
      ((minor) == 6 && (patch) >= 7))) || \
     ((major) == 4 && ((minor) >= 2 || \
      ((minor) == 1 && (patch) >= 1))))

#define MAELYS_HTTP_MBEDTLS_BACKPORTED_VERSION(major, minor, patch) \
    (((major) == 3 && (minor) >= 6) || \
     ((major) == 4 && (minor) >= 1))

#if defined(MAELYS_HTTP_MBEDTLS_ALLOW_BACKPORTED_SECURITY_FIXES)
#define MAELYS_HTTP_MBEDTLS_ACCEPTED_VERSION(major, minor, patch) \
    MAELYS_HTTP_MBEDTLS_BACKPORTED_VERSION(major, minor, patch)
#else
#define MAELYS_HTTP_MBEDTLS_ACCEPTED_VERSION(major, minor, patch) \
    MAELYS_HTTP_MBEDTLS_SECURE_VERSION(major, minor, patch)
#endif

/* Only upstream-maintained major lines are accepted. An explicit override is
 * limited to maintained minor lines whose distribution package backports all
 * relevant security fixes without changing the upstream version number. */
#if !MAELYS_HTTP_MBEDTLS_ACCEPTED_VERSION( \
        MBEDTLS_VERSION_MAJOR, MBEDTLS_VERSION_MINOR, MBEDTLS_VERSION_PATCH)
#error "Mbed TLS is unsupported or below the required security patch level"
#endif

#endif
