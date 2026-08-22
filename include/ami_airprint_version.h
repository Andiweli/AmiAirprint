#ifndef AMI_AIRPRINT_VERSION_H
#define AMI_AIRPRINT_VERSION_H

/*
 * AmiAirPrint package / preferences version.
 *
 * Version 1.1 adds mDNS/DNS-SD printer discovery in the preferences tools.
 * The classic printer-driver segment and airprint.device did not change for
 * this release, so their independently visible versions stay frozen at the
 * tested 1.0 values below.
 */
#define AMIAIRPRINT_VERSION_MAJOR 1
#define AMIAIRPRINT_VERSION_MINOR 1
#define AMIAIRPRINT_VERSION_TEXT "1.1"
#define AMIAIRPRINT_VERSION_DATE "22.8.2026"
#define AMIAIRPRINT_VERSION_DATE_LONG "22 Aug 2026"

/* Explicit preferences aliases: these track the package release. */
#define AMIAIRPRINT_PREFS_VERSION_MAJOR AMIAIRPRINT_VERSION_MAJOR
#define AMIAIRPRINT_PREFS_VERSION_MINOR AMIAIRPRINT_VERSION_MINOR
#define AMIAIRPRINT_PREFS_VERSION_TEXT AMIAIRPRINT_VERSION_TEXT
#define AMIAIRPRINT_PREFS_VERSION_DATE AMIAIRPRINT_VERSION_DATE
#define AMIAIRPRINT_PREFS_VERSION_DATE_LONG AMIAIRPRINT_VERSION_DATE_LONG

/* Unchanged core / diagnostic components carried over from AmiAirPrint 1.0. */
#define AMIAIRPRINT_CORE_VERSION_MAJOR 1
#define AMIAIRPRINT_CORE_VERSION_MINOR 0
#define AMIAIRPRINT_CORE_VERSION_TEXT "1.0"
#define AMIAIRPRINT_CORE_VERSION_DATE "22.8.2026"
#define AMIAIRPRINT_CORE_VERSION_DATE_LONG "22 Aug 2026"

#endif
