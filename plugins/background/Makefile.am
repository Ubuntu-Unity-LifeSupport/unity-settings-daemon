NULL =

plugin_name = background

libexec_PROGRAMS = usd-test-background

usd_test_background_SOURCES =		\
	test-background.c		\
	gsd-background-manager.h	\
	gsd-background-manager.c	\
	$(NULL)

usd_test_background_CPPFLAGS =				\
	-I$(top_srcdir)/gnome-settings-daemon		\
	-I$(top_srcdir)/plugins/common			\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

usd_test_background_CFLAGS = \
	$(PLUGIN_CFLAGS)		\
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(BACKGROUND_CFLAGS)		\
	$(AM_CFLAGS)

usd_test_background_LDADD =				\
	$(top_builddir)/gnome-settings-daemon/libgsd.la	\
	$(top_builddir)/gnome-settings-daemon/libunity-settings-daemon.la	\
	$(SETTINGS_PLUGIN_LIBS)				\
	$(BACKGROUND_LIBS)				\
	$(NULL)

libexec_PROGRAMS += gnome-update-wallpaper-cache

gnome_update_wallpaper_cache_SOURCES =		\
	gnome-update-wallpaper-cache.c		\
	$(NULL)

gnome_update_wallpaper_cache_CPPFLAGS = \
	-I$(top_srcdir)/gnome-settings-daemon		\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

gnome_update_wallpaper_cache_CFLAGS = \
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(BACKGROUND_CFLAGS)		\
	$(AM_CFLAGS)

gnome_update_wallpaper_cache_LDADD =				\
	$(top_builddir)/gnome-settings-daemon/libgsd.la	\
	$(top_builddir)/gnome-settings-daemon/libunity-settings-daemon.la	\
	$(SETTINGS_PLUGIN_LIBS)				\
	$(BACKGROUND_LIBS)				\
	$(NULL)

plugin_LTLIBRARIES = \
	libbackground.la		\
	$(NULL)

libbackground_la_SOURCES =		\
	gsd-background-plugin.c		\
	gsd-background-manager.h	\
	gsd-background-manager.c	\
	$(NULL)

libbackground_la_CPPFLAGS = \
	-I$(top_srcdir)/gnome-settings-daemon		\
	-I$(top_srcdir)/plugins/background/libbackground   \
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

libbackground_la_CFLAGS = \
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(BACKGROUND_CFLAGS)		\
	$(AM_CFLAGS)

libbackground_la_LDFLAGS =		\
	$(GSD_PLUGIN_LDFLAGS)		\
	$(NULL)

libbackground_la_LIBADD  =		\
	$(top_builddir)/gnome-settings-daemon/libgsd.la	\
	$(top_builddir)/gnome-settings-daemon/libunity-settings-daemon.la	\
	$(SETTINGS_PLUGIN_LIBS)		\
	$(BACKGROUND_LIBS)		\
	$(NULL)

plugin_in_files =		\
	background.gnome-settings-plugin.in	\
	$(NULL)

plugin_DATA = $(plugin_in_files:.gnome-settings-plugin.in=.gnome-settings-plugin)

EXTRA_DIST = 			\
	$(plugin_in_files)	\
	$(NULL)

CLEANFILES = 			\
	$(plugin_DATA)		\
	$(NULL)

DISTCLEANFILES =		\
	$(plugin_DATA)		\
	$(NULL)

@GSD_INTLTOOL_PLUGIN_RULE@
