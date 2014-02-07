NULL =

INCLUDES = \
	-DDATADIR=\""$(datadir)"\"				\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\"	\
	-DLIBEXECDIR=\""$(libexecdir)"\" 			\
	-DGNOME_SETTINGS_PLUGINDIR=\""$(plugindir)"\"		\
	$(WARN_CFLAGS)						\
	$(DISABLE_DEPRECATED_CFLAGS)				\
	$(SETTINGS_DAEMON_CFLAGS)				\
	$(LIBNOTIFY_CFLAGS)					\
	$(GNOME_DESKTOP_CFLAGS)					\
	$(NULL)

privlibdir = $(pkglibdir)-$(GSD_API_VERSION)

privlib_LTLIBRARIES =		\
	libgsd.la		\
	$(NULL)

libgsd_la_SOURCES =		\
	gnome-settings-profile.c	\
	gnome-settings-profile.h	\
	gnome-settings-session.c	\
	gnome-settings-session.h	\
	$(NULL)

libgsd_la_CPPFLAGS = 		\
	$(DISABLE_DEPRECATED_CFLAGS)	\
	$(NULL)

libgsd_la_CFLAGS =		\
	$(NULL)

libgsd_la_LIBADD =		\
	$(SETTINGS_DAEMON_LIBS)		\
	$(GIOUNIX_LIBS)		\
	$(NULL)

libgsd_la_LDFLAGS =		\
	-export-dynamic		\
	-avoid-version		\
	-no-undefined		\
	$(NULL)

gsddir = $(libexecdir)

gsd_PROGRAMS = \
	unity-settings-daemon

gsd_SCRIPTS = \
	unity-settings-daemon-localeexec

EXTRA_DIST = \
	unity-settings-daemon-localeexec.in

unity-settings-daemon-localeexec: unity-settings-daemon-localeexec.in
	$(AM_V_GEN) sed -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@prefix\@|$(prefix)|"  $< > $@.tmp && mv $@.tmp $@

apidir   = $(includedir)/unity-settings-daemon-$(GSD_API_VERSION)/unity-settings-daemon
api_DATA = 				\
	gnome-settings-plugin.h

unity_settings_daemon_SOURCES =		\
	main.c				\
	gnome-settings-manager.c	\
	gnome-settings-manager.h	\
	gnome-settings-plugin.c		\
	gnome-settings-plugin.h		\
	gnome-settings-plugin-info.c	\
	gnome-settings-plugin-info.h	\
	gnome-settings-module.c		\
	gnome-settings-module.h		\
	$(NULL)

unity_settings_daemon_CPPFLAGS = \
	$(AM_CPPFLAGS)

unity_settings_daemon_CFLAGS = 		\
	$(AM_CFLAGS)

unity_settings_daemon_LDFLAGS = 	\
	$(AM_LDFLAGS)

unity_settings_daemon_LDADD = 		\
	libgsd.la		\
	$(SETTINGS_DAEMON_LIBS)		\
	$(LIBNOTIFY_LIBS)		\
	$(GNOME_DESKTOP_LIBS)		\
	$(NULL)

CLEANFILES = $(gsd_SCRIPTS)

# vim: ts=8
