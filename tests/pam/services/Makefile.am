all-am: fprintd-pam-test

fprintd-pam-test: fprintd-pam-test.in Makefile
	sed -e "s|\@TOPBUILDDIR\@|$(abs_top_builddir)|" $< > $@

EXTRA_DIST = fprintd-pam-test.in
CLEANFILES = fprintd-pam-test
