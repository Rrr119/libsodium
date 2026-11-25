ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = \
	autogen.sh \
	build.zig \
	ChangeLog \
	CITATION.cff \
	LICENSE \
	README.markdown \
	THANKS

SUBDIRS = \
	builds \
	dist-build \
	src \
	test

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = @PACKAGE_NAME@.pc

DISTCLEANFILES = $(pkgconfig_DATA)

.PHONY: consttime-check
consttime-check:
	LIBSODIUM_BUILD="$(abs_builddir)" $(srcdir)/test/consttime/run.sh

