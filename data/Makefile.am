
dbus_servicesdir = $(datadir)/dbus-1/system-services
dbus_services_in_files = net.reactivated.Fprint.service.in
dbus_services_DATA = $(dbus_services_in_files:.service.in=.service)

$(dbus_services_DATA): $(dbus_services_in_files)
	sed -e "s|\@LIBEXECDIR\@|$(libexecdir)|" $< > $@

dbus_confdir = $(sysconfdir)/dbus-1/system.d
dbus_conf_DATA = net.reactivated.Fprint.conf

systemdservice_in_files = fprintd.service.in

if HAVE_SYSTEMD
systemdservicedir       = $(systemdsystemunitdir)
systemdservice_DATA     = $(systemdservice_in_files:.service.in=.service)
$(systemdservice_DATA): $(systemdservice_in_files) Makefile
	@sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@
endif

polkitdir = $(datadir)/polkit-1/actions
polkit_in_files = net.reactivated.fprint.device.policy.in

@INTLTOOL_POLICY_RULE@
polkit_DATA = $(polkit_in_files:.policy.in=.policy)

confdir = $(sysconfdir)/
conf_DATA = fprintd.conf

man_MANS =

if BUILD_MAN
man_MANS += fprintd.1
fprintd.1: fprintd.pod
	$(AM_V_GEN) pod2man -c "" -s 1 -q none -n fprintd -r freedesktop $< > $@
endif

EXTRA_DIST = $(dbus_services_in_files) $(dbus_conf_DATA) $(polkit_in_files) $(conf_DATA) $(systemdservice_in_files) fprintd.pod
CLEANFILES = $(polkit_DATA) $(dbus_services_DATA) $(systemdservice_DATA) fprintd.1

check:
	@$(XMLLINT) --noout $(polkit_DATA)


