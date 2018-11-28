/*-
 * Copyright (c) 2018 Anthony Jenkins <Scoobi_doo@yahoo.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <sys/conf.h>
#include <sys/uio.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#define LPSS_PRIV_OFFSET	0x200
#define LPSS_PRIV_SIZE		0x100
#define LPSS_PRIV_CAPS		0xfc
#define LPSS_PRIV_CAPS_TYPE_SHIFT	4
#define LPSS_PRIV_CAPS_TYPE_MASK	(0xf << LPSS_PRIV_CAPS_TYPE_SHIFT)

#define LPSS_PRIV_READ_4(sc, offset) \
	bus_read_4((sc), LPSS_PRIV_OFFSET + (offset))
#define LPSS_PRIV_WRITE_4(sc, offset, value) \
	bus_write_4((sc), LPSS_PRIV_OFFSET + (offset), (value))

struct lpss_softc {
	device_t		sc_dev;
	int			sc_mem_rid;
	struct resource		*sc_mem_res;
	int			sc_irq_rid;
	struct resource		*sc_irq_res;
	void			*sc_irq_ih;
	unsigned long 		sc_clock_rate;
	uint32_t		sc_caps;
	int			sc_type;	// LPSS_PRIV_TYPE_*
#define LPSS_PRIV_TYPE_I2C	0
#define LPSS_PRIV_TYPE_UART	1
#define LPSS_PRIV_TYPE_SPI	2
#define LPSS_PRIV_TYPE_MAX	LPSS_PRIV_TYPE_SPI
};

static const struct {
	uint16_t vendor;
	uint16_t device;
	unsigned long clock_rate;
} lpss_pci_ids[] = {
	{ 0x8086, 0x9d61, 120000000 },
	{ 0x8086, 0xa368, 120000000 },
	{ 0x8086, 0xa369, 120000000 },
	{      0,      0,         0 }
};

static int
lpss_pci_probe(device_t dev)
{
	int i;

	for (i = 0; lpss_pci_ids[i].vendor != 0; ++i) {
		if (pci_get_vendor(dev) == lpss_pci_ids[i].vendor &&
		    pci_get_device(dev) == lpss_pci_ids[i].device)
		{
			struct lpss_softc *sc;

			device_printf(dev, "Found PCI device 0x%04x:0x%04x\n",
					lpss_pci_ids[i].vendor,
					lpss_pci_ids[i].device);
			sc = device_get_softc(dev);
			sc->sc_clock_rate = lpss_pci_ids[i].clock_rate;
			device_set_desc(dev, "Intel LPSS PCI Driver");
			return (BUS_PROBE_DEFAULT);
		}
	}
	return ENXIO;
}

static int
lpss_pci_attach(device_t dev)
{
	struct lpss_softc *sc;

	sc = device_get_softc(dev);

	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		goto error;
	}
	sc->sc_dev = dev;
	sc->sc_mem_rid = 0;
	sc->sc_mem_res = bus_alloc_resource_any(sc->sc_dev,
	    SYS_RES_MEMORY, &sc->sc_mem_rid, RF_ACTIVE);
	if (sc->sc_mem_res == NULL) {
		device_printf(dev, "Can't allocate memory resource\n");
		goto error;
	}

	sc->sc_irq_rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(sc->sc_dev,
	    SYS_RES_IRQ, &sc->sc_irq_rid, RF_ACTIVE);
	if (sc->sc_irq_res == NULL) {
		device_printf(dev, "Can't allocate IRQ resource\n");
		goto error;
	}
	device_printf(dev, "IRQ: %d\n", sc->sc_caps);
	sc->sc_caps = LPSS_PRIV_READ_4(sc->sc_mem_res, LPSS_PRIV_CAPS);
	device_printf(dev, "Capabilities: 0x%08x\n", sc->sc_caps);
	sc->sc_type = (sc->sc_caps & LPSS_PRIV_CAPS_TYPE_MASK) >> LPSS_PRIV_CAPS_TYPE_SHIFT;
	if (sc->sc_type > LPSS_PRIV_TYPE_MAX) {
		device_printf(dev, "No supported MFP device found (sc_type=0x%04x).\n", sc->sc_type);
		goto error;
	}
	device_printf(dev, "MFP device type: %s\n",
			sc->sc_type == LPSS_PRIV_TYPE_I2C ? "I2C" :
			sc->sc_type == LPSS_PRIV_TYPE_UART ? "UART" :
			sc->sc_type == LPSS_PRIV_TYPE_SPI ? "SPI" : "Unknown");

	return bus_generic_attach(dev);

error:
	if (sc->sc_mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    sc->sc_mem_rid, sc->sc_mem_res);

	if (sc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ,
		    sc->sc_irq_rid, sc->sc_irq_res);
	}

	return ENXIO;
}

static int
lpss_pci_detach(device_t dev)
{
	struct lpss_softc *sc;

	sc = device_get_softc(dev);

	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		return ENXIO;
	}
	if (sc->sc_mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    sc->sc_mem_rid, sc->sc_mem_res);

	if (sc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ,
		    sc->sc_irq_rid, sc->sc_irq_res);
	}
	return bus_generic_detach(dev);
}

static int
lpss_pci_shutdown(device_t dev)
{
	return (0);
}

static int
lpss_pci_suspend(device_t dev)
{
	return (0);
}

static int
lpss_pci_resume(device_t dev)
{
	return (0);
}

static device_method_t lpss_pci_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,		lpss_pci_probe),
    DEVMETHOD(device_attach,		lpss_pci_attach),
    DEVMETHOD(device_detach,		lpss_pci_detach),
    DEVMETHOD(device_shutdown,		lpss_pci_shutdown),
    DEVMETHOD(device_suspend,		lpss_pci_suspend),
    DEVMETHOD(device_resume,		lpss_pci_resume),

#if 0
    DEVMETHOD(bus_alloc_resource,	lpss_bus_alloc_resource),
    DEVMETHOD(bus_release_resource,	lpss_bus_release_resource),
    DEVMETHOD(bus_get_resource,		lpss_bus_get_resource),
    DEVMETHOD(bus_read_ivar,		lpss_bus_read_ivar),
    DEVMETHOD(bus_setup_intr,		lpss_bus_setup_intr),
    DEVMETHOD(bus_teardown_intr,	lpss_bus_teardown_intr),
    DEVMETHOD(bus_print_child,		lpss_bus_print_child),
    DEVMETHOD(bus_child_pnpinfo_str,	lpss_bus_child_pnpinfo_str),
    DEVMETHOD(bus_child_location_str,	lpss_bus_child_location_str),
#endif

    DEVMETHOD_END
};

static driver_t lpss_pci_driver = {
	"lpss",
	lpss_pci_methods,
	sizeof(struct lpss_softc)
};

static devclass_t lpss_devclass;

DRIVER_MODULE(lpss, pci, lpss_pci_driver, lpss_devclass, 0, 0);
