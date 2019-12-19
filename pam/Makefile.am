if HAVE_PAM

pammod_LTLIBRARIES = pam_fprintd.la
pammoddir=$(libdir)/security

pam_fprintd_la_SOURCES = pam_fprintd.c $(MARSHALFILES)
pam_fprintd_la_CFLAGS = -fPIC $(WARN_CFLAGS) $(PAM_MODULE_CFLAGS) -DLOCALEDIR="\"$(localedir)\""
pam_fprintd_la_LDFLAGS = -avoid-version -module -Wl,-z,nodelete
pam_fprintd_la_LIBADD = $(PAM_LIBS) $(PAM_MODULE_LIBS)

MARSHALFILES = marshal.c marshal.h
GLIB_GENMARSHAL=`pkg-config --variable=glib_genmarshal glib-2.0`
BUILT_SOURCES = $(MARSHALFILES)

marshal.h: $(top_srcdir)/src/fprintd-marshal.list
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --header > marshal.h )
marshal.c: marshal.h
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --body --header > marshal.c )

endif

EXTRA_DIST = pam_fprintd.c fingerprint-strings.h
