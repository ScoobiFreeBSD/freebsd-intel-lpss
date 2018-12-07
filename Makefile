# Makefile with convenience development targets.

MODULE_DIR=	sys/modules/intel/lpss

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
	find . -type f -name '*.[ch]' -print0 | xargs -0 $(CTAGS) -f "$(TAGSFILE)"

.PHONY: all module clean install uninstall tags
