NULL =

apidir = $(includedir)/unity-settings-daemon-$(GSD_API_VERSION)/unity-settings-daemon
api_DATA = gsd-enums.h

gsettings_ENUM_NAMESPACE = com.canonical.unity.settings-daemon
gsettings_ENUM_FILES = $(top_srcdir)/data/$(api_DATA)


gschema_in_files =							\
	com.canonical.unity.settings-daemon.peripherals.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.peripherals.wacom.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.color.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.datetime.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.housekeeping.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.keyboard.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.media-keys.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.orientation.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.power.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.print-notifications.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.sharing.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.xrandr.gschema.xml.in.in \
	com.canonical.unity.settings-daemon.plugins.xsettings.gschema.xml.in.in
	
	
gsettings_SCHEMAS = $(gschema_in_files:.xml.in.in=.xml)

gsettingsdir = $(datadir)/glib-2.0/schemas

%.gschema.xml.in: %.gschema.xml.in.in
	$(AM_V_GEN) sed -e "s|\@GETTEXT_PACKAGE\@|$(GETTEXT_PACKAGE)|g" $< > $@

@INTLTOOL_XML_NOMERGE_RULE@

@GSETTINGS_RULES@

@INTLTOOL_DESKTOP_RULE@
desktopdir = $(sysconfdir)/xdg/autostart
desktop_in_files = unity-settings-daemon.desktop.in.in
desktop_DATA = $(desktop_in_files:.desktop.in.in=.desktop)

unity-settings-daemon.desktop.in: unity-settings-daemon.desktop.in.in
	$(AM_V_GEN) sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = unity-settings-daemon.pc libunity-settings-daemon.pc

@INTLTOOL_XML_NOMERGE_RULE@

EXTRA_DIST = 					\
	$(convert_DATA)				\
	$(gschema_in_files)         \
	$(desktop_in_files)			\
	$(gsettings_ENUM_FILES)			\
	unity-settings-daemon.pc.in		\
	libunity-settings-daemon.pc.in		\
	$(api_DATA)				\
	$(NULL)

DISTCLEANFILES = 			\
	$(desktop_DATA)			\
	unity-settings-daemon.desktop.in \
	$(NULL)

MAINTAINERCLEANFILES =			\
	*~				\
	Makefile.in     \
	$(gsettings_SCHEMAS:.xml=.valid)

