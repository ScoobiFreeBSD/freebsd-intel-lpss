# Makefile with convenience development targets.

MODULE_DIR_LPSS=	sys/modules/intel/lpss
MODULE_DIR_IG4=		sys/modules/i2c/controllers/ichiic

LINUX_SRC_DIR=	$(HOME)/Projects/linux-4.19.6

TAGS_SEARCH_DIRS=	. $(LINUX_SRC_DIR)

# Are we running as root?
ID!=	id -u
SUDO_DEPS=
.if ${ID} != 0
SUDO=	sudo
SUDO_DEPS=	has-sudo
.endif

CTAGS=		exctags
TAGSFILE=	tags

DEFAULT_TARGET=	all
ALL_TARGET=	modules

default: $(DEFAULT_TARGET)

all: $(ALL_TARGET)

modules: module-lpss module-ig4

module-lpss:
	$(MAKE) -C $(MODULE_DIR_LPSS) SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g

module-ig4:
	$(MAKE) -C $(MODULE_DIR_IG4) SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g

clean:
	$(MAKE) -C $(MODULE_DIR_LPSS) clean SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g
	$(MAKE) -C $(MODULE_DIR_IG4) clean SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g
	rm -f $(MODULE_DIR_LPSS)/.depend* $(MODULE_DIR_IG4)/.depend*

distclean: clean

install: modules $(SUDO_DEPS)
	${SUDO} $(MAKE) -C $(MODULE_DIR_LPSS) install SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g
	${SUDO} $(MAKE) -C $(MODULE_DIR_IG4) install SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g

uninstall: unload $(SUDO_DEPS)
	${SUDO} rm -f /boot/modules/ig4.ko
	${SUDO} rm -f /boot/modules/lpss.ko

load: install $(SUDO_DEPS)
	$(SUDO) kldload /boot/modules/ig4.ko

unload: $(SUDO_DEPS)
	-$(SUDO) kldunload ig4
	-$(SUDO) kldunload lpss

tags:
	rm -f "$(TAGSFILE)"
	find $(TAGS_SEARCH_DIRS) -type f -name '*.[ch]' -print0 | xargs -0 $(CTAGS) -a -f "$(TAGSFILE)"

has-sudo:
	@sudo -V 1>/dev/null 2>&1 || { echo "*** Please install security/sudo port."; false; }

help:
	@echo "Build  Targets:"
	@echo
	@echo "        all : Alias for '$(ALL_TARGET)' (default)."
	@echo "    modules : Build ig4.ko and lpss.ko modules."
	@echo "module-lpss : Build lpss.ko module."
	@echo " module-ig4 : Build ig4.ko module."
	@echo "      clean : Remove all build files."
	@echo "  distclean : Alias for 'clean'."
	@echo "    install : Install ig4.ko and lpss.ko to /boot/modules."
	@echo "  uninstall : Remove ig4.ko and lpss.ko from /boot/modules."
	@echo "       load : Load ig4.ko into kernel."
	@echo "     unload : Unload ig4.ko and lpss.ko from kernel."
	@echo "       tags : Generate $(TAGSFILE) file (requires $(CTAGS))."
	@echo "       help : Print this message."
.PHONY: all modules module-ig4 module-lpss clean distclean install uninstall load unload tags has-sudo help
