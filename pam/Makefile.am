if HAVE_PAM

pammod_LTLIBRARIES = pam_fprintd.la
pammoddir=$(libdir)/security

pam_fprintd_la_SOURCES = pam_fprintd.c
pam_fprintd_la_CFLAGS = -fPIC $(WARN_CFLAGS) $(PAM_MODULE_CFLAGS) -DLOCALEDIR="\"$(localedir)\""
pam_fprintd_la_LDFLAGS = -avoid-version -module -Wl,-z,nodelete
pam_fprintd_la_LIBADD = $(PAM_LIBS) $(PAM_MODULE_LIBS)

endif

EXTRA_DIST = pam_fprintd.c fingerprint-strings.h
