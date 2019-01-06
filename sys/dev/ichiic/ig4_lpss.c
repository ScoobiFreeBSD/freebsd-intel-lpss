/*
 * Copyright (c) 2014 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com> and was subsequently ported
 * to FreeBSD by Michael Gmelin <freebsd@grem.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Intel fourth generation mobile cpus integrated I2C device.
 *
 * See ig4_reg.h for datasheet reference and notes.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/syslog.h>
#include <sys/bus.h>

#include <machine/bus.h>
#include <sys/rman.h>
#include <machine/resource.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iiconf.h>

#include <dev/ichiic/ig4_reg.h>
#include <dev/ichiic/ig4_var.h>

#define USE_DEV_IDENTIFY 1

static int ig4iic_lpss_detach(device_t dev);

static int
ig4iic_lpss_probe(device_t dev)
{
#if 0
//	ig4iic_softc_t *sc = device_get_softc(dev);
	devclass_t dc;
	device_t *devices = NULL;
	int num_devices = 0;

	dc = devclass_find("lpss");
	if (dc != NULL) {
		if (devclass_get_devices(dc, &devices, &num_devices) == 0 && num_devices > 0) {
			int i;

			for (i = 0; i < num_devices; ++i) {
				device_printf(dev, "HOORAY! Found lpss class device unit %d.\n", device_get_unit(devices[i]));
			}
			free(devices, M_TEMP);
			return (BUS_PROBE_DEFAULT);
		} else {
			device_printf(dev, "No lpss class devices found.\n");
		}
	} else {
		device_printf(dev, "lpss class not found.\n");
	}
#else
	device_printf(dev, "%s: Returning BUS_PROBE_NOWILDCARD.\n", __func__);
	return BUS_PROBE_NOWILDCARD;
#endif
}

#if defined(USE_DEV_IDENTIFY)
static void
ig4iic_lpss_identify(driver_t *driver, device_t parent)
{
	/* Add only a single device instance. */
	device_printf(parent, "%s: Entered.\n", __func__);
	if (device_find_child(parent, "ig4iic_lpss", -1) == NULL) {
		if (BUS_ADD_CHILD(parent, 0, "ig4iic_lpss", -1) == NULL) {
			device_printf(parent, "add ig4iic_lpss child failed\n");
		}
	}
}
#endif

static int
ig4iic_lpss_attach(device_t dev)
{
	int error = ENXIO;

	device_printf(dev, "%s: Entered.\n", __func__);
	ig4iic_softc_t *sc = device_get_softc(dev);
	int count = 1;

	sc->dev = dev;
	sc->regs_rid = PCIR_BAR(0);
	sc->regs_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
					  &sc->regs_rid, RF_SHAREABLE | RF_ACTIVE);
	if (sc->regs_res == NULL) {
		device_printf(dev, "%s: Unable to map registers\n", __func__);
		ig4iic_lpss_detach(dev);
		return (ENXIO);
	} else {
		device_printf(dev, "%s: Got memory resource.\n", __func__);
	}
	if (pci_alloc_msi(dev, &count) == 0) {
		device_printf(dev, "Using MSI\n");
		sc->intr_type = INTR_TYPE_MSI;
		sc->intr_rid = 1;
	}
	else
	{
		sc->intr_type = INTR_TYPE_PCI;
		sc->intr_rid = 0;
	}
	sc->intr_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
					  &sc->intr_rid, RF_SHAREABLE | RF_ACTIVE);
	if (sc->intr_res == NULL) {
		device_printf(dev, "Unable to map interrupt\n");
		ig4iic_lpss_detach(dev);
		return (ENXIO);
	} else {
		device_printf(dev, "%s: Got interrupt resource.\n", __func__);
	}
	sc->platform_attached = 1;

	error = ig4iic_attach(sc);
	if (error)
		ig4iic_lpss_detach(dev);

	device_printf(dev, "%s: Returning %d.\n", __func__, error);
	return (error);
}

static int
ig4iic_lpss_detach(device_t dev)
{
	int error = 0;
	ig4iic_softc_t *sc = device_get_softc(dev);

	device_printf(dev, "%s: Entered.\n", __func__);
	if (sc->platform_attached) {
		error = ig4iic_detach(sc);
		if (error)
			return (error);
		sc->platform_attached = 0;
	}

	if (sc->intr_res) {
		bus_release_resource(dev, SYS_RES_IRQ,
				     sc->intr_rid, sc->intr_res);
		sc->intr_res = NULL;
	}
	if (sc->intr_type == INTR_TYPE_MSI) {
		pci_release_msi(dev);
	}
	if (sc->regs_res) {
		bus_release_resource(dev, SYS_RES_MEMORY,
				     sc->regs_rid, sc->regs_res);
		sc->regs_res = NULL;
	}
	return (error);
}

static device_method_t ig4iic_lpss_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, ig4iic_lpss_probe),
#if defined(USE_DEV_IDENTIFY)
	DEVMETHOD(device_identify,ig4iic_lpss_identify),
#endif
	DEVMETHOD(device_attach, ig4iic_lpss_attach),
	DEVMETHOD(device_detach, ig4iic_lpss_detach),

	DEVMETHOD(iicbus_transfer, ig4iic_transfer),
	DEVMETHOD(iicbus_reset, ig4iic_reset),
	DEVMETHOD(iicbus_callback, iicbus_null_callback),

	DEVMETHOD_END
};

static driver_t ig4iic_lpss_driver = {
	"ig4iic_lpss",
	ig4iic_lpss_methods,
	sizeof(struct ig4iic_softc)
};

static devclass_t ig4iic_lpss_devclass;

DRIVER_MODULE_ORDERED(ig4iic_lpss, lpss, ig4iic_lpss_driver, ig4iic_lpss_devclass, 0, 0,
    SI_ORDER_ANY);
DRIVER_MODULE(iicbus, ig4iic_lpss, iicbus_driver, iicbus_devclass, NULL, NULL);
MODULE_DEPEND(ig4iic_lpss, lpss, 1, 1, 1);
MODULE_DEPEND(ig4iic_lpss, iicbus, IICBUS_MINVER, IICBUS_PREFVER, IICBUS_MAXVER);
MODULE_VERSION(ig4iic_lpss, 1);
/*
 * Loading this module breaks suspend/resume on laptops
 * Do not add MODULE_PNP_INFO until it's impleneted
 */ 
