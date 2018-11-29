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

#define BIT(nr) (1UL << (nr))

#define LPSS_PRIV_OFFSET	0x200
#define LPSS_PRIV_SIZE		0x100
#define LPSS_PRIV_CAPS		0xfc
#define LPSS_PRIV_CAPS_TYPE_SHIFT	4
#define LPSS_PRIV_CAPS_TYPE_MASK	(0xf << LPSS_PRIV_CAPS_TYPE_SHIFT)
#define LPSS_PRIV_CAPS_NO_IDMA		BIT(8)
#define LPSS_PRIV_REMAP_ADDR		0x40
#define LPSS_PRIV_RESETS		0x04
#define LPSS_PRIV_RESETS_FUNC		BIT(2)
#define LPSS_PRIV_RESETS_IDMA		0x3
#define LPSS_PRIV_SSP_REG		0x20
#define LPSS_PRIV_SSP_REG_DIS_DMA_FIN	BIT(0)

#define LPSS_PRIV_READ_4(sc, offset) \
	bus_read_4((sc), LPSS_PRIV_OFFSET + (offset))
#define LPSS_PRIV_WRITE_4(sc, offset, value) \
	bus_write_4((sc), LPSS_PRIV_OFFSET + (offset), (value))
#define LPSS_PRIV_WRITE_8(sc, offset, value) \
	bus_write_8((sc), LPSS_PRIV_OFFSET + (offset), (value))

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

static bool intel_lpss_has_idma(const struct lpss_softc *sc)
{
	return (sc->sc_caps & LPSS_PRIV_CAPS_NO_IDMA) == 0;
}

static void intel_lpss_set_remap_addr(const struct lpss_softc *sc)
{
	LPSS_PRIV_WRITE_8(sc->sc_mem_res, LPSS_PRIV_REMAP_ADDR, (uintptr_t)sc->sc_mem_res);
}

static void intel_lpss_deassert_reset(const struct lpss_softc *sc)
{
	uint32_t value = LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA;

	/* Bring out the device from reset */
	LPSS_PRIV_WRITE_4(sc->sc_mem_res, LPSS_PRIV_RESETS, value);
}

static void lpss_init_dev(const struct lpss_softc *sc)
{
	uint32_t value = LPSS_PRIV_SSP_REG_DIS_DMA_FIN;

	intel_lpss_deassert_reset(sc);

	if (!intel_lpss_has_idma(sc))
		return;

	intel_lpss_set_remap_addr(sc);

	/* Make sure that SPI multiblock DMA transfers are re-enabled */
	if (sc->sc_type == LPSS_PRIV_TYPE_SPI)
		LPSS_PRIV_WRITE_4(sc->sc_mem_res, LPSS_PRIV_SSP_REG, value);
}

static int intel_lpss_register_clock_divider(struct lpss_softc *sc)
{
// 	char name[32];
// 	struct clk *tmp = *clk;
//
// 	snprintf(name, sizeof(name), "%s-enable", devname);
// 	tmp = clk_register_gate(NULL, name, __clk_get_name(tmp), 0,
// 				lpss->priv, 0, 0, NULL);
// 	if (IS_ERR(tmp))
// 		return PTR_ERR(tmp);
//
// 	snprintf(name, sizeof(name), "%s-div", devname);
// 	tmp = clk_register_fractional_divider(NULL, name, __clk_get_name(tmp),
// 					      0, lpss->priv, 1, 15, 16, 15, 0,
// 					      NULL);
// 	if (IS_ERR(tmp))
// 		return PTR_ERR(tmp);
// 	*clk = tmp;
//
// 	snprintf(name, sizeof(name), "%s-update", devname);
// 	tmp = clk_register_gate(NULL, name, __clk_get_name(tmp),
// 				CLK_SET_RATE_PARENT, lpss->priv, 31, 0, NULL);
// 	if (IS_ERR(tmp))
// 		return PTR_ERR(tmp);
// 	*clk = tmp;

	return 0;
}

static int intel_lpss_register_clock(struct lpss_softc *sc)
{
// 	const struct mfd_cell *cell = lpss->cell;
// 	struct clk *clk;
// 	char devname[24];
	int ret;

	if (!sc->sc_clock_rate)
		return 0;

// 	/* Root clock */
// 	clk = clk_register_fixed_rate(NULL, dev_name(lpss->dev), NULL,
// 				      CLK_IS_ROOT, sc->sc_clock_rate);
// 	if (IS_ERR(clk))
// 		return PTR_ERR(clk);
//
// 	snprintf(devname, sizeof(devname), "%s.%d", cell->name, lpss->devid);

	/*
	 * Support for clock divider only if it has some preset value.
	 * Otherwise we assume that the divider is not used.
	 */
	if (sc->sc_type != LPSS_PRIV_TYPE_I2C) {
		ret = intel_lpss_register_clock_divider(sc);
		if (ret)
			goto err_clk_register;
	}

// 	ret = -ENOMEM;
//
// 	/* Clock for the host controller */
// 	lpss->clock = clkdev_create(clk, lpss->info->clk_con_id, "%s", devname);
// 	if (!lpss->clock)
// 		goto err_clk_register;
//
// 	lpss->clk = clk;

	return 0;

err_clk_register:
// 	intel_lpss_unregister_clock_tree(clk);

	return ret;
}

static int
lpss_pci_attach(device_t dev)
{
	struct lpss_softc *sc;
    int ret;

	sc = device_get_softc(dev);

	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		goto error;
	}
	sc->sc_dev = dev;
	sc->sc_mem_rid = 0x10;
	sc->sc_mem_res = bus_alloc_resource_any(sc->sc_dev,
	    SYS_RES_MEMORY, &sc->sc_mem_rid, RF_ACTIVE);
	if (sc->sc_mem_res == NULL) {
		device_printf(dev, "Can't allocate memory resource\n");
		goto error;
	}

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

	lpss_init_dev(sc);

	ret = intel_lpss_register_clock(sc);
	if (ret)
		goto error;

// 	intel_lpss_ltr_expose(sc);

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
