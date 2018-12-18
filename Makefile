# Makefile with convenience development targets.

MODULE_DIR_LPSS=	sys/modules/intel/lpss
MODULE_DIR_IG4=		sys/modules/i2c/controllers/ichiic

LINUX_SRC_DIR=	$(HOME)/Projects/linux-4.19.6

TAGS_SEARCH_DIRS=	. $(LINUX_SRC_DIR)

# Are we running as root?
ID!=	id -u
.if ${ID} != 0
SUDO=	sudo
.endif

CTAGS=		exctags
TAGSFILE=	tags

all: modules

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

install: modules
	${SUDO} $(MAKE) -C $(MODULE_DIR_LPSS) install SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g
	${SUDO} $(MAKE) -C $(MODULE_DIR_IG4) install SRCTOP=$(.CURDIR) DEBUG=YES DEBUG_FLAGS=-g

uninstall: unload
	${SUDO} rm -f /boot/modules/lpss.ko
	${SUDO} rm -f /boot/modules/ig4.ko

load: install
	$(SUDO) kldload /boot/modules/lpss.ko
	$(SUDO) kldload /boot/modules/ig4.ko

unload:
	-$(SUDO) kldunload ig4
	-$(SUDO) kldunload lpss

tags:
	rm -f "$(TAGSFILE)"
	find $(TAGS_SEARCH_DIRS) -type f -name '*.[ch]' -print0 | xargs -0 $(CTAGS) -a -f "$(TAGSFILE)"

.PHONY: all modules module-ig4 module-lpss clean distclean install uninstall tags
