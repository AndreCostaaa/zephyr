/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_gfxss_dc

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/dt-bindings/display/panel.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cy_graphics.h>
#include <cy_sysclk.h>

LOG_MODULE_REGISTER(display_infineon_gfxss, CONFIG_DISPLAY_LOG_LEVEL);

/* The display controller fetches each framebuffer line in 128-byte bursts */
#define GFXSS_STRIDE_ALIGN 128U

struct ifx_gfxss_dc_config {
	GFXSS_Type *base;
	void (*irq_config_func)(const struct device *dev);
	uint8_t *fb;
	size_t fb_region_size;
	uint16_t width;
	uint16_t height;
	enum display_pixel_format pixel_format;
	uint32_t pixel_clock_khz;
	uint16_t hsync_len;
	uint16_t hfront_porch;
	uint16_t hback_porch;
	uint16_t vsync_len;
	uint16_t vfront_porch;
	uint16_t vback_porch;
	uint32_t polarity_flags;
	uint8_t data_lanes;
	uint32_t lane_rate_mbps;
};

struct ifx_gfxss_dc_data {
	cy_stc_gfx_context_t gfx_ctx;
	cy_stc_mipidsi_display_params_t display_params;
	cy_stc_mipidsi_config_t dsi_cfg;
	cy_stc_gfx_layer_config_t layer_cfg;
	cy_stc_gfx_dc_config_t dc_cfg;
	cy_stc_gfx_gpu_cfg_t gpu_cfg;
	cy_stc_gfx_config_t gfx_cfg;
	uint8_t bytes_per_pixel;
	/* Framebuffer line length in pixels, a multiple of GFXSS_STRIDE_ALIGN bytes */
	uint16_t pitch;
	bool started;
};

static void ifx_gfxss_dc_isr(const struct device *dev)
{
	const struct ifx_gfxss_dc_config *config = dev->config;
	struct ifx_gfxss_dc_data *data = dev->data;

	Cy_GFXSS_Clear_DC_Interrupt(config->base, &data->gfx_ctx);
}

static int ifx_gfxss_dc_write(const struct device *dev, const uint16_t x, const uint16_t y,
			      const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ifx_gfxss_dc_config *config = dev->config;
	struct ifx_gfxss_dc_data *data = dev->data;
	const uint8_t *src = buf;
	size_t line_bytes = (size_t)desc->width * data->bytes_per_pixel;
	size_t pitch_bytes = (size_t)data->pitch * data->bytes_per_pixel;
	uint8_t *dst;

	if (((uint32_t)x + desc->width > config->width) ||
	    ((uint32_t)y + desc->height > config->height) || (desc->pitch < desc->width)) {
		return -EINVAL;
	}

	dst = config->fb + ((size_t)y * pitch_bytes) + ((size_t)x * data->bytes_per_pixel);

	for (uint16_t row = 0U; row < desc->height; row++) {
		memcpy(dst, src, line_bytes);
		src += (size_t)desc->pitch * data->bytes_per_pixel;
		dst += pitch_bytes;
	}

	/* The display controller reads the framebuffer from memory, not from the cache */
	sys_cache_data_flush_range(config->fb + ((size_t)y * pitch_bytes),
				   (size_t)desc->height * pitch_bytes);

	return 0;
}

/*
 * The DSI link is started only once the panel is powered: a bridge powered after
 * the link entered high-speed video mode does not lock onto the stream.
 */
static int ifx_gfxss_dc_start(const struct device *dev)
{
	const struct ifx_gfxss_dc_config *config = dev->config;
	struct ifx_gfxss_dc_data *data = dev->data;
	cy_en_gfx_status_t status;

	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_GPU_PERI_NR, CY_MMIO_GFXSS_GPU_GROUP_NR,
				     CY_MMIO_GFXSS_GPU_SLAVE_NR, CY_MMIO_GFXSS_GPU_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_DC_PERI_NR, CY_MMIO_GFXSS_DC_GROUP_NR,
				     CY_MMIO_GFXSS_DC_SLAVE_NR, CY_MMIO_GFXSS_DC_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_GFXSS_MIPIDSI_PERI_NR, CY_MMIO_GFXSS_MIPIDSI_GROUP_NR,
				     CY_MMIO_GFXSS_MIPIDSI_SLAVE_NR,
				     CY_MMIO_GFXSS_MIPIDSI_CLK_HF_NR);

	status = Cy_GFXSS_Init(config->base, &data->gfx_cfg, &data->gfx_ctx);
	if (status != CY_GFX_SUCCESS) {
		LOG_ERR("GFXSS init failed (%d)", status);
		return -EIO;
	}

	config->irq_config_func(dev);

	/* For DPI output, Cy_GFXSS_Init() only stages the configuration; this commits it */
	status = Cy_GFXSS_Set_FrameBuffer(config->base, (uint32_t *)config->fb, &data->gfx_ctx);
	if (status != CY_GFX_SUCCESS) {
		LOG_ERR("Setting the framebuffer failed (%d)", status);
		return -EIO;
	}

	data->started = true;

	LOG_DBG("%ux%u, pitch %u px, clk_hf %u Hz", config->width, config->height, data->pitch,
		data->gfx_cfg.clockHz);

	return 0;
}

static int ifx_gfxss_dc_blanking_off(const struct device *dev)
{
	struct ifx_gfxss_dc_data *data = dev->data;

	if (data->started) {
		return 0;
	}

	return ifx_gfxss_dc_start(dev);
}

static void ifx_gfxss_dc_get_capabilities(const struct device *dev,
					  struct display_capabilities *capabilities)
{
	const struct ifx_gfxss_dc_config *config = dev->config;

	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->x_resolution = config->width;
	capabilities->y_resolution = config->height;
	capabilities->supported_pixel_formats = config->pixel_format;
	capabilities->current_pixel_format = config->pixel_format;
	capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static DEVICE_API(display, ifx_gfxss_dc_api) = {
	.blanking_off = ifx_gfxss_dc_blanking_off,
	.write = ifx_gfxss_dc_write,
	.get_capabilities = ifx_gfxss_dc_get_capabilities,
};

static int ifx_gfxss_dc_init(const struct device *dev)
{
	const struct ifx_gfxss_dc_config *config = dev->config;
	struct ifx_gfxss_dc_data *data = dev->data;
	viv_input_format_type input_format;
	size_t fb_size;

	switch (config->pixel_format) {
	case PIXEL_FORMAT_RGB_565:
		input_format = vivRGB565;
		data->bytes_per_pixel = 2U;
		break;
	case PIXEL_FORMAT_ARGB_8888:
		input_format = vivARGB8888;
		data->bytes_per_pixel = 4U;
		break;
	default:
		LOG_ERR("Unsupported pixel format 0x%x", config->pixel_format);
		return -ENOTSUP;
	}

	data->pitch = ROUND_UP(config->width, GFXSS_STRIDE_ALIGN / data->bytes_per_pixel);
	fb_size = (size_t)data->pitch * config->height * data->bytes_per_pixel;
	if (fb_size > config->fb_region_size) {
		LOG_ERR("Framebuffer needs %zu bytes, memory region has %zu", fb_size,
			config->fb_region_size);
		return -ENOMEM;
	}

	memset(config->fb, 0, fb_size);
	sys_cache_data_flush_range(config->fb, fb_size);

	/* The active width sent to the panel is the padded framebuffer line */
	data->display_params = (cy_stc_mipidsi_display_params_t){
		.pixel_clock = config->pixel_clock_khz,
		.hdisplay = data->pitch,
		.hsync_width = config->hsync_len,
		.hfp = config->hfront_porch,
		.hbp = config->hback_porch,
		.vdisplay = config->height,
		.vsync_width = config->vsync_len,
		.vfp = config->vfront_porch,
		.vbp = config->vback_porch,
		.polarity_flags = config->polarity_flags,
	};

	data->dsi_cfg = (cy_stc_mipidsi_config_t){
		.virtual_ch = 0U,
		.num_of_lanes = config->data_lanes,
		.per_lane_mbps = config->lane_rate_mbps,
		.dpi_fmt = CY_MIPIDSI_FMT_RGB888,
		/*
		 * Only used to derive the escape clock divider. The value matches the
		 * Infineon display libraries, which give an escape clock divider of 41.
		 */
		.max_phy_clk = 2500000000U,
		.dsi_mode = DSI_VIDEO_MODE,
		.mode_flags = VID_MODE_TYPE_BURST | ENABLE_LOW_POWER_CMD | ENABLE_LOW_POWER,
		.display_params = &data->display_params,
	};

	data->layer_cfg = (cy_stc_gfx_layer_config_t){
		.layer_type = GFX_LAYER_GRAPHICS,
		.buffer_address = (gctADDRESS *)config->fb,
		.input_format_type = input_format,
		.tiling_type = vivLINEAR,
		.width = data->pitch,
		.height = config->height,
		.layer_enable = true,
		.visibility = true,
	};

	data->dc_cfg = (cy_stc_gfx_dc_config_t){
		.gfx_layer_config = &data->layer_cfg,
		.display_type = GFX_DISP_TYPE_DSI_DPI,
		.display_format = vivD24,
		.display_size = vivDISPLAY_CUSTOMIZED,
		.display_width = data->pitch,
		.display_height = config->height,
	};

	data->gpu_cfg.enable = false;

	data->gfx_cfg = (cy_stc_gfx_config_t){
		.dc_cfg = &data->dc_cfg,
		.gpu_cfg = &data->gpu_cfg,
		.mipi_dsi_cfg = &data->dsi_cfg,
		.display_update_type = GFX_SINGLE_BUFFER,
		.clockHz = Cy_SysClk_ClkHfGetFrequency(CY_MMIO_GFXSS_DC_CLK_HF_NR),
	};

	return 0;
}

#define IFX_GFXSS_DSI_NODE(n)    DT_INST_CHILD(n, mipi_dsi)
#define IFX_GFXSS_TIMING_NODE(n) DT_INST_CHILD(n, display_timings)

#define IFX_GFXSS_POLARITY_FLAGS(n)                                                                \
	((DT_PROP(IFX_GFXSS_TIMING_NODE(n), hsync_active) ? DISPLAY_PARAMS_FLAG_PHSYNC             \
							   : DISPLAY_PARAMS_FLAG_NHSYNC) |         \
	 (DT_PROP(IFX_GFXSS_TIMING_NODE(n), vsync_active) ? DISPLAY_PARAMS_FLAG_PVSYNC             \
							   : DISPLAY_PARAMS_FLAG_NVSYNC))

#define IFX_GFXSS_DC_INIT(n)                                                                       \
	BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(IFX_GFXSS_DSI_NODE(n)),                               \
		     "GFXSS display controller requires an enabled mipi-dsi child node");          \
	BUILD_ASSERT(DT_PROP(IFX_GFXSS_DSI_NODE(n), pixel_format) == MIPI_DSI_PIXFMT_RGB888,      \
		     "GFXSS MIPI DSI host only supports RGB888");                                  \
                                                                                                   \
	static void ifx_gfxss_dc_irq_config_##n(const struct device *dev)                          \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), ifx_gfxss_dc_isr,           \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct ifx_gfxss_dc_config ifx_gfxss_dc_config_##n = {                        \
		.base = (GFXSS_Type *)DT_INST_REG_ADDR(n),                                         \
		.irq_config_func = ifx_gfxss_dc_irq_config_##n,                                    \
		.fb = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(n, memory_region)),                   \
		.fb_region_size = DT_REG_SIZE(DT_INST_PHANDLE(n, memory_region)),                  \
		.width = DT_INST_PROP(n, width),                                                   \
		.height = DT_INST_PROP(n, height),                                                 \
		.pixel_format = DT_INST_PROP(n, pixel_format),                                     \
		.pixel_clock_khz = DT_PROP(IFX_GFXSS_TIMING_NODE(n), clock_frequency) / 1000U,     \
		.hsync_len = DT_PROP(IFX_GFXSS_TIMING_NODE(n), hsync_len),                         \
		.hfront_porch = DT_PROP(IFX_GFXSS_TIMING_NODE(n), hfront_porch),                   \
		.hback_porch = DT_PROP(IFX_GFXSS_TIMING_NODE(n), hback_porch),                     \
		.vsync_len = DT_PROP(IFX_GFXSS_TIMING_NODE(n), vsync_len),                         \
		.vfront_porch = DT_PROP(IFX_GFXSS_TIMING_NODE(n), vfront_porch),                   \
		.vback_porch = DT_PROP(IFX_GFXSS_TIMING_NODE(n), vback_porch),                     \
		.polarity_flags = IFX_GFXSS_POLARITY_FLAGS(n),                                     \
		.data_lanes = DT_PROP(IFX_GFXSS_DSI_NODE(n), data_lanes),                          \
		.lane_rate_mbps = DT_PROP(IFX_GFXSS_DSI_NODE(n), phy_clock) / 1000000U,            \
	};                                                                                         \
                                                                                                   \
	static struct ifx_gfxss_dc_data ifx_gfxss_dc_data_##n;                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, ifx_gfxss_dc_init, NULL, &ifx_gfxss_dc_data_##n,                  \
			      &ifx_gfxss_dc_config_##n, POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, \
			      &ifx_gfxss_dc_api);

DT_INST_FOREACH_STATUS_OKAY(IFX_GFXSS_DC_INIT)
