all-am: fprintd-pam-test

fprintd-pam-test: fprintd-pam-test.in Makefile
	sed -e "s|\@FPRINTDPAMPATH\@|$(abs_top_builddir)/pam/.libs/pam_fprintd.so|" $< > $@

EXTRA_DIST = fprintd-pam-test.in
CLEANFILES = fprintd-pam-test
