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

#define LPSS_DEV_OFFSET		0x000
#define LPSS_DEV_SIZE		0x200
#define LPSS_PRIV_OFFSET	0x200
#define LPSS_PRIV_SIZE		0x100
#define LPSS_PRIV_REG_COUNT	(LPSS_PRIV_SIZE / 4)
#define LPSS_IDMA64_OFFSET	0x800
#define LPSS_IDMA64_SIZE	0x800

/* Offsets from lpss->sc_map_priv */
#define LPSS_PRIV_RESETS		0x04
#define LPSS_PRIV_RESETS_IDMA		BIT(2)
#define LPSS_PRIV_RESETS_FUNC		0x3

#define LPSS_PRIV_ACTIVELTR		0x10
#define LPSS_PRIV_IDLELTR		0x14

#define LPSS_PRIV_LTR_REQ		BIT(15)
#define LPSS_PRIV_LTR_SCALE_MASK	0xc00
#define LPSS_PRIV_LTR_SCALE_1US		0x800
#define LPSS_PRIV_LTR_SCALE_32US	0xc00
#define LPSS_PRIV_LTR_VALUE_MASK	0x3ff

#define LPSS_PRIV_SSP_REG		0x20
#define LPSS_PRIV_REMAP_ADDR		0x40

#define LPSS_PRIV_CAPS		0xfc
#define LPSS_PRIV_CAPS_TYPE_SHIFT	4
#define LPSS_PRIV_CAPS_TYPE_MASK	(0xf << LPSS_PRIV_CAPS_TYPE_SHIFT)
#define LPSS_PRIV_CAPS_NO_IDMA		BIT(8)
#define LPSS_PRIV_SSP_REG_DIS_DMA_FIN	BIT(0)

#define LPSS_PRIV_READ_4(sc, offset) \
	bus_read_4(&(sc)->sc_map_priv, (offset))
#define LPSS_PRIV_WRITE_4(res, offset, value) \
	bus_write_4(&(sc)->sc_map_priv, (offset), (value))
#define LPSS_PRIV_WRITE_8(res, offset, value) \
	bus_write_8(&(sc)->sc_map_priv, (offset), (value))

/* This matches the type field in CAPS register */
enum intel_lpss_dev_type {
	LPSS_DEV_I2C = 0,
	LPSS_DEV_UART,
	LPSS_DEV_SPI,
};

static void lo_hi_writeq(const struct resource_map *map, unsigned long addr, uint64_t value)
{
	bus_write_4(map, addr, value & 0xffffffff);
	bus_write_4(map, addr + 4, value >> 32);
}

struct lpss_softc {
	device_t		sc_dev;
	int			sc_mem_rid;
	struct resource		*sc_mem_res;
	int			sc_irq_rid;
	struct resource		*sc_irq_res;
	void			*sc_irq_ih;
	struct resource_map	sc_map_dev;
	struct resource_map	sc_map_priv;
	unsigned long 		sc_clock_rate;
	uint32_t		sc_caps;
	int			sc_type;	// LPSS_PRIV_TYPE_*
#define LPSS_PRIV_TYPE_I2C	0
#define LPSS_PRIV_TYPE_UART	1
#define LPSS_PRIV_TYPE_SPI	2
#define LPSS_PRIV_TYPE_MAX	LPSS_PRIV_TYPE_SPI
	uint32_t                priv_ctx[LPSS_PRIV_REG_COUNT];
};

struct device;
struct resource;
struct property_entry
{
	const char *name;
	uint32_t value;
};

#define PROPERTY_ENTRY_U32(_name, _value) { .name = _name, .value = _value }
#define PROPERTY_ENTRY_BOOL(_name) PROPERTY_ENTRY_U32(_name, 0)

struct intel_lpss_platform_info {
	struct resource *mem;
	int irq;
	unsigned long clock_rate;
	const char *clock_con_id;
	struct property_entry *properties;
};

static const struct intel_lpss_platform_info spt_info = {
	.clock_rate = 120000000,
};

static struct property_entry spt_i2c_properties[] = {
	PROPERTY_ENTRY_U32("i2c-sda-hold-time-ns", 230),
	{ },
};

static const struct intel_lpss_platform_info spt_i2c_info = {
	.clock_rate = 120000000,
	.properties = spt_i2c_properties,
};

static struct property_entry uart_properties[] = {
	PROPERTY_ENTRY_U32("reg-io-width", 4),
	PROPERTY_ENTRY_U32("reg-shift", 2),
	PROPERTY_ENTRY_BOOL("snps,uart-16550-compatible"),
	{ },
};

static const struct intel_lpss_platform_info spt_uart_info = {
	.clock_rate = 120000000,
	.clock_con_id = "baudclk",
	.properties = uart_properties,
};

static const struct intel_lpss_platform_info bxt_info = {
	.clock_rate = 100000000,
};

static const struct intel_lpss_platform_info bxt_uart_info = {
	.clock_rate = 100000000,
	.clock_con_id = "baudclk",
	.properties = uart_properties,
};

static struct property_entry bxt_i2c_properties[] = {
	PROPERTY_ENTRY_U32("i2c-sda-hold-time-ns", 42),
	PROPERTY_ENTRY_U32("i2c-sda-falling-time-ns", 171),
	PROPERTY_ENTRY_U32("i2c-scl-falling-time-ns", 208),
	{ },
};

static const struct intel_lpss_platform_info bxt_i2c_info = {
	.clock_rate = 133000000,
	.properties = bxt_i2c_properties,
};

static struct property_entry apl_i2c_properties[] = {
	PROPERTY_ENTRY_U32("i2c-sda-hold-time-ns", 207),
	PROPERTY_ENTRY_U32("i2c-sda-falling-time-ns", 171),
	PROPERTY_ENTRY_U32("i2c-scl-falling-time-ns", 208),
	{ },
};

static const struct intel_lpss_platform_info apl_i2c_info = {
	.clock_rate = 133000000,
	.properties = apl_i2c_properties,
};

static const struct intel_lpss_platform_info cnl_i2c_info = {
	.clock_rate = 216000000,
	.properties = spt_i2c_properties,
};
static const struct
{
	uint16_t vendor;
	uint16_t device;
	const struct intel_lpss_platform_info *info;
} intel_lpss_pci_ids[] = {
	/* BXT A-Step */
	{ 0x8086, 0x0aac, &bxt_i2c_info },
	{ 0x8086, 0x0aae, &bxt_i2c_info },
	{ 0x8086, 0x0ab0, &bxt_i2c_info },
	{ 0x8086, 0x0ab2, &bxt_i2c_info },
	{ 0x8086, 0x0ab4, &bxt_i2c_info },
	{ 0x8086, 0x0ab6, &bxt_i2c_info },
	{ 0x8086, 0x0ab8, &bxt_i2c_info },
	{ 0x8086, 0x0aba, &bxt_i2c_info },
	{ 0x8086, 0x0abc, &bxt_uart_info },
	{ 0x8086, 0x0abe, &bxt_uart_info },
	{ 0x8086, 0x0ac0, &bxt_uart_info },
	{ 0x8086, 0x0ac2, &bxt_info },
	{ 0x8086, 0x0ac4, &bxt_info },
	{ 0x8086, 0x0ac6, &bxt_info },
	{ 0x8086, 0x0aee, &bxt_uart_info },
	/* BXT B-Step */
	{ 0x8086, 0x1aac, &bxt_i2c_info },
	{ 0x8086, 0x1aae, &bxt_i2c_info },
	{ 0x8086, 0x1ab0, &bxt_i2c_info },
	{ 0x8086, 0x1ab2, &bxt_i2c_info },
	{ 0x8086, 0x1ab4, &bxt_i2c_info },
	{ 0x8086, 0x1ab6, &bxt_i2c_info },
	{ 0x8086, 0x1ab8, &bxt_i2c_info },
	{ 0x8086, 0x1aba, &bxt_i2c_info },
	{ 0x8086, 0x1abc, &bxt_uart_info },
	{ 0x8086, 0x1abe, &bxt_uart_info },
	{ 0x8086, 0x1ac0, &bxt_uart_info },
	{ 0x8086, 0x1ac2, &bxt_info },
	{ 0x8086, 0x1ac4, &bxt_info },
	{ 0x8086, 0x1ac6, &bxt_info },
	{ 0x8086, 0x1aee, &bxt_uart_info },
	/* GLK */
	{ 0x8086, 0x31ac, &bxt_i2c_info },
	{ 0x8086, 0x31ae, &bxt_i2c_info },
	{ 0x8086, 0x31b0, &bxt_i2c_info },
	{ 0x8086, 0x31b2, &bxt_i2c_info },
	{ 0x8086, 0x31b4, &bxt_i2c_info },
	{ 0x8086, 0x31b6, &bxt_i2c_info },
	{ 0x8086, 0x31b8, &bxt_i2c_info },
	{ 0x8086, 0x31ba, &bxt_i2c_info },
	{ 0x8086, 0x31bc, &bxt_uart_info },
	{ 0x8086, 0x31be, &bxt_uart_info },
	{ 0x8086, 0x31c0, &bxt_uart_info },
	{ 0x8086, 0x31ee, &bxt_uart_info },
	{ 0x8086, 0x31c2, &bxt_info },
	{ 0x8086, 0x31c4, &bxt_info },
	{ 0x8086, 0x31c6, &bxt_info },
	/* ICL-LP */
	{ 0x8086, 0x34a8, &spt_uart_info },
	{ 0x8086, 0x34a9, &spt_uart_info },
	{ 0x8086, 0x34aa, &spt_info },
	{ 0x8086, 0x34ab, &spt_info },
	{ 0x8086, 0x34c5, &bxt_i2c_info },
	{ 0x8086, 0x34c6, &bxt_i2c_info },
	{ 0x8086, 0x34c7, &spt_uart_info },
	{ 0x8086, 0x34e8, &bxt_i2c_info },
	{ 0x8086, 0x34e9, &bxt_i2c_info },
	{ 0x8086, 0x34ea, &bxt_i2c_info },
	{ 0x8086, 0x34eb, &bxt_i2c_info },
	{ 0x8086, 0x34fb, &spt_info },
	/* APL */
	{ 0x8086, 0x5aac, &apl_i2c_info },
	{ 0x8086, 0x5aae, &apl_i2c_info },
	{ 0x8086, 0x5ab0, &apl_i2c_info },
	{ 0x8086, 0x5ab2, &apl_i2c_info },
	{ 0x8086, 0x5ab4, &apl_i2c_info },
	{ 0x8086, 0x5ab6, &apl_i2c_info },
	{ 0x8086, 0x5ab8, &apl_i2c_info },
	{ 0x8086, 0x5aba, &apl_i2c_info },
	{ 0x8086, 0x5abc, &bxt_uart_info },
	{ 0x8086, 0x5abe, &bxt_uart_info },
	{ 0x8086, 0x5ac0, &bxt_uart_info },
	{ 0x8086, 0x5ac2, &bxt_info },
	{ 0x8086, 0x5ac4, &bxt_info },
	{ 0x8086, 0x5ac6, &bxt_info },
	{ 0x8086, 0x5aee, &bxt_uart_info },
	/* SPT-LP */
	{ 0x8086, 0x9d27, &spt_uart_info },
	{ 0x8086, 0x9d28, &spt_uart_info },
	{ 0x8086, 0x9d29, &spt_info },
	{ 0x8086, 0x9d2a, &spt_info },
	{ 0x8086, 0x9d60, &spt_i2c_info },
	{ 0x8086, 0x9d61, &spt_i2c_info },
	{ 0x8086, 0x9d62, &spt_i2c_info },
	{ 0x8086, 0x9d63, &spt_i2c_info },
	{ 0x8086, 0x9d64, &spt_i2c_info },
	{ 0x8086, 0x9d65, &spt_i2c_info },
	{ 0x8086, 0x9d66, &spt_uart_info },
	/* CNL-LP */
	{ 0x8086, 0x9da8, &spt_uart_info },
	{ 0x8086, 0x9da9, &spt_uart_info },
	{ 0x8086, 0x9daa, &spt_info },
	{ 0x8086, 0x9dab, &spt_info },
	{ 0x8086, 0x9dfb, &spt_info },
	{ 0x8086, 0x9dc5, &cnl_i2c_info },
	{ 0x8086, 0x9dc6, &cnl_i2c_info },
	{ 0x8086, 0x9dc7, &spt_uart_info },
	{ 0x8086, 0x9de8, &cnl_i2c_info },
	{ 0x8086, 0x9de9, &cnl_i2c_info },
	{ 0x8086, 0x9dea, &cnl_i2c_info },
	{ 0x8086, 0x9deb, &cnl_i2c_info },
	/* SPT-H */
	{ 0x8086, 0xa127, &spt_uart_info },
	{ 0x8086, 0xa128, &spt_uart_info },
	{ 0x8086, 0xa129, &spt_info },
	{ 0x8086, 0xa12a, &spt_info },
	{ 0x8086, 0xa160, &spt_i2c_info },
	{ 0x8086, 0xa161, &spt_i2c_info },
	{ 0x8086, 0xa162, &spt_i2c_info },
	{ 0x8086, 0xa166, &spt_uart_info },
	/* KBL-H */
	{ 0x8086, 0xa2a7, &spt_uart_info },
	{ 0x8086, 0xa2a8, &spt_uart_info },
	{ 0x8086, 0xa2a9, &spt_info },
	{ 0x8086, 0xa2aa, &spt_info },
	{ 0x8086, 0xa2e0, &spt_i2c_info },
	{ 0x8086, 0xa2e1, &spt_i2c_info },
	{ 0x8086, 0xa2e2, &spt_i2c_info },
	{ 0x8086, 0xa2e3, &spt_i2c_info },
	{ 0x8086, 0xa2e6, &spt_uart_info },
	/* CNL-H */
	{ 0x8086, 0xa328, &spt_uart_info },
	{ 0x8086, 0xa329, &spt_uart_info },
	{ 0x8086, 0xa32a, &spt_info },
	{ 0x8086, 0xa32b, &spt_info },
	{ 0x8086, 0xa37b, &spt_info },
	{ 0x8086, 0xa347, &spt_uart_info },
	{ 0x8086, 0xa368, &cnl_i2c_info },
	{ 0x8086, 0xa369, &cnl_i2c_info },
	{ 0x8086, 0xa36a, &cnl_i2c_info },
	{ 0x8086, 0xa36b, &cnl_i2c_info },
	{ }
};

static int
lpss_pci_probe(device_t dev)
{
	int i;

	for (i = 0; intel_lpss_pci_ids[i].vendor != 0; ++i) {
		if (pci_get_vendor(dev) == intel_lpss_pci_ids[i].vendor &&
		    pci_get_device(dev) == intel_lpss_pci_ids[i].device)
		{
			struct lpss_softc *sc;

#if 0
			device_printf(dev, "Found PCI device 0x%04x:0x%04x\n",
					intel_lpss_pci_ids[i].vendor,
					intel_lpss_pci_ids[i].device);
#endif
			sc = device_get_softc(dev);
			sc->sc_clock_rate = intel_lpss_pci_ids[i].info->clock_rate;
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
	lo_hi_writeq(&sc->sc_map_priv, LPSS_PRIV_REMAP_ADDR,
			(uintptr_t)sc->sc_map_priv.r_vaddr);
}

static void intel_lpss_deassert_reset(const struct lpss_softc *sc)
{
	/* Bring out the device from reset */
	LPSS_PRIV_WRITE_4(sc, LPSS_PRIV_RESETS,
			LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA);
}

static void intel_lpss_init_dev(const struct lpss_softc *sc)
{
	intel_lpss_deassert_reset(sc);

	if (intel_lpss_has_idma(sc)) {
		intel_lpss_set_remap_addr(sc);

		/* Make sure that SPI multiblock DMA transfers are re-enabled */
		if (sc->sc_type == LPSS_PRIV_TYPE_SPI) {
			LPSS_PRIV_WRITE_4(sc, LPSS_PRIV_SSP_REG,
					LPSS_PRIV_SSP_REG_DIS_DMA_FIN);
		}
	}
}

static int
lpss_pci_attach(device_t dev)
{
	struct lpss_softc *sc;
	struct resource_map_request map_req;
	int count;

	sc = device_get_softc(dev);

	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		goto error;
	}
	sc->sc_dev = dev;
	sc->sc_mem_rid = PCIR_BAR(0);
	sc->sc_mem_res = bus_alloc_resource_any(sc->sc_dev,
	    SYS_RES_MEMORY, &sc->sc_mem_rid, RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_mem_res == NULL) {
		device_printf(dev, "Can't allocate memory resource\n");
		goto error;
	}

	if (pci_alloc_msi(dev, &count) == 0) {
		device_printf(dev, "Using MSI\n");
		sc->sc_irq_rid = 1;
	} else {
		sc->sc_irq_rid = 0;
	}
	sc->sc_irq_res = bus_alloc_resource_any(sc->sc_dev,
	    SYS_RES_IRQ, &sc->sc_irq_rid, RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_irq_res == NULL) {
		device_printf(dev, "Can't allocate IRQ resource\n");
		goto error;
	}
	device_printf(dev, "IRQ: %d\n", sc->sc_irq_rid);

	/* Set up DEV memory region */
	resource_init_map_request(&map_req);
	map_req.offset = LPSS_DEV_OFFSET;
	map_req.length = LPSS_DEV_SIZE;
	if (bus_map_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res,
				&map_req, &sc->sc_map_dev) != 0)
	{
		device_printf(dev, "Can't map DEV memory resource\n");
		goto error;
	}

	/* Set up PRIV memory region */
	resource_init_map_request(&map_req);
	map_req.offset = LPSS_PRIV_OFFSET;
	map_req.length = LPSS_PRIV_SIZE;
	if (bus_map_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res,
				&map_req, &sc->sc_map_priv) != 0)
	{
		device_printf(dev, "Can't map PRIV memory resource\n");
		goto error;
	}

	/* Read device capabilities */
	sc->sc_caps = LPSS_PRIV_READ_4(sc, LPSS_PRIV_CAPS);
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

	/* Finish initialization */
	intel_lpss_init_dev(sc);

#if 0
	if (sc->sc_type == LPSS_PRIV_TYPE_I2C) {
		device_add_child(dev, "ig4iic_lpss", -1);
	}
#endif

	return bus_generic_attach(dev);

error:
	bus_unmap_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res, &sc->sc_map_priv);
	bus_unmap_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res, &sc->sc_map_dev);
	if (sc->sc_mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_mem_rid, sc->sc_mem_res);
	}
	if (sc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid, sc->sc_irq_res);
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
	bus_unmap_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res, &sc->sc_map_priv);
	bus_unmap_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_res, &sc->sc_map_dev);
	if (sc->sc_mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_mem_rid, sc->sc_mem_res);
	}
	if (sc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid, sc->sc_irq_res);
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
	struct lpss_softc *sc;
	unsigned int i;

	sc = device_get_softc(dev);
	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		return ENXIO;
	}

	/* Save device context */
	for (i = 0; i < LPSS_PRIV_REG_COUNT; i++) {
		sc->priv_ctx[i] = LPSS_PRIV_READ_4(sc, i * 4);
	}

	/*
	 * If the device type is not UART, then put the controller into
	 * reset. UART cannot be put into reset since S3/S0ix fail when
	 * no_console_suspend flag is enabled.
	 */
	if (sc->sc_type != LPSS_DEV_UART) {
		LPSS_PRIV_WRITE_4(sc, LPSS_PRIV_RESETS, 0);
	}

	return 0;
}

static int
lpss_pci_resume(device_t dev)
{
	struct lpss_softc *sc;
	unsigned int i;

	sc = device_get_softc(dev);
	if (!sc) {
		device_printf(dev, "Error getting softc from device.");
		return ENXIO;
	}

	intel_lpss_deassert_reset(sc);

	/* Restore device context */
	for (i = 0; i < LPSS_PRIV_REG_COUNT; i++) {
		LPSS_PRIV_WRITE_4(sc, sc->priv_ctx[i], i * 4);
	}

	return 0;
}

static device_t
lpss_add_child(device_t dev, u_int order, const char *name, int unit)
{
	return device_add_child_ordered(dev, order, name, unit);
}

#if 0
static int
lpss_child_present(device_t dev, device_t child)
{
	return (bus_child_present(dev));
}

static int
lpss_read_ivar(device_t dev, device_t child, int which, uintptr_t *result)
{
	return(ENOENT);
}

static int
lpss_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{
	return(ENOENT);
}

static struct resource *
lpss_alloc_resource(device_t dev, device_t child, int type, int *rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	return bus_generic_alloc_resource(dev, child, type, rid, start, end, count, flags);
}

static int
lpss_release_resource(device_t dev, device_t child, int type, int rid,
    struct resource *r)
{
	return bus_generic_release_resource(dev, child, type, rid, r);
}

static int
lpss_adjust_resource(device_t bus, device_t child, int type, struct resource *r,
    rman_res_t start, rman_res_t end)
{
	return bus_generic_adjust_resource(bus, child, type, r, start, end);
}

static int
lpss_print_child(device_t bus, device_t child)
{
	return BUS_PRINT_CHILD(bus, child);
}
#endif

static device_method_t lpss_pci_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,		lpss_pci_probe),
    DEVMETHOD(device_attach,		lpss_pci_attach),
    DEVMETHOD(device_detach,		lpss_pci_detach),
    DEVMETHOD(device_shutdown,		lpss_pci_shutdown),
    DEVMETHOD(device_suspend,		lpss_pci_suspend),
    DEVMETHOD(device_resume,		lpss_pci_resume),

    /* Bus interface */
    DEVMETHOD(bus_add_child,		lpss_add_child),
#if 0
    DEVMETHOD(bus_child_present,	lpss_child_present),		/* pcib_child_present */
    DEVMETHOD(bus_read_ivar,		lpss_read_ivar),		/* pcib_read_ivar */
    DEVMETHOD(bus_write_ivar,		lpss_write_ivar),		/* pcib_write_ivar */
    DEVMETHOD(bus_alloc_resource,	lpss_alloc_resource),		/* bus_generic_alloc_resource */
    DEVMETHOD(bus_release_resource,	lpss_release_resource),		/* bus_generic_release_resource */
    DEVMETHOD(bus_adjust_resource,	lpss_adjust_resource),		/* bus_generic_adjust_resource */
    DEVMETHOD(bus_print_child,		lpss_print_child),		/* lpss_bus_print_child */
    DEVMETHOD(bus_activate_resource,	bus_generic_activate_resource),
    DEVMETHOD(bus_deactivate_resource,	bus_generic_deactivate_resource),
    DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
    DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),
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
MODULE_DEPEND(lpss, pci, 1, 1, 1);
MODULE_VERSION(lpss, 1);
