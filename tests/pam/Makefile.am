SUBDIRS = services

TESTS_ENVIRONMENT = export TOPSRCDIR=$(abs_top_srcdir); export LD_PRELOAD=libpam_wrapper.so; export PAM_WRAPPER_SERVICE_DIR=$(abs_top_builddir)/tests/pam/services; export PAM_WRAPPER=1; export PYTHON=@PYTHON@;
TESTS = test_pam_fprintd.py

EXTRA_DIST = $(TESTS)
