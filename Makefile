# Makefile with convenience development targets.

MODULE_DIR=	sys/modules/intel/lpss
LINUX_SRC_DIR=	$(HOME)/Projects/linux-4.19.6

TAGS_SEARCH_DIRS=	. $(LINUX_SRC_DIR)

# Are we running as root?
ID!=	id -u
.if ${ID} != 0
SUDO=	sudo
.endif

CTAGS=		exctags
TAGSFILE=	tags

all: module

module:
	$(MAKE) -C $(MODULE_DIR)

clean:
	$(MAKE) -C $(MODULE_DIR) clean
	rm -f $(MODULE_DIR)/.depend*

install: module
	${SUDO} $(MAKE) -C $(MODULE_DIR) install

uninstall:
	${SUDO} rm -f /boot/modules/lpss.ko

tags:
	rm -f "$(TAGSFILE)"
	find $(TAGS_SEARCH_DIRS) -type f -name '*.[ch]' -print0 | xargs -0 $(CTAGS) -a -f "$(TAGSFILE)"

.PHONY: all module clean install uninstall tags
