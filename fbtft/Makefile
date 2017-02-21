ccflags-y += -I$(src)/../include

# Core module
fbtft-y                                  += fbtft-core.o fbtft-bus.o fbtft-io.o
fbtft-y += tinydrm-helpers2.o tinydrm-panel.o tinydrm-regmap.o tinydrm-fbtft.o tinydrm-ili9325.o
obj-$(CONFIG_TINYDRM_FB_TFT)             += fbtft.o

# converted
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9325)     += fb_ili9325.o

# drivers
obj-$(CONFIG_TINYDRM_FB_TFT_BD663474)    += fb_bd663474.o
obj-$(CONFIG_TINYDRM_FB_TFT_HX8340BN)    += fb_hx8340bn.o
obj-$(CONFIG_TINYDRM_FB_TFT_HX8347D)     += fb_hx8347d.o
obj-$(CONFIG_TINYDRM_FB_TFT_HX8353D)     += fb_hx8353d.o
obj-$(CONFIG_TINYDRM_FB_TFT_HX8357D)     += fb_hx8357d.o
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9163)     += fb_ili9163.o
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9340)     += fb_ili9340.o
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9341)     += fb_ili9341.o
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9481)     += fb_ili9481.o
obj-$(CONFIG_TINYDRM_FB_TFT_ILI9486)     += fb_ili9486.o
obj-$(CONFIG_TINYDRM_FB_TFT_PCD8544)     += fb_pcd8544.o
obj-$(CONFIG_TINYDRM_FB_TFT_RA8875)      += fb_ra8875.o
obj-$(CONFIG_TINYDRM_FB_TFT_S6D02A1)     += fb_s6d02a1.o
obj-$(CONFIG_TINYDRM_FB_TFT_S6D1121)     += fb_s6d1121.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1289)     += fb_ssd1289.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1305)     += fb_ssd1305.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1306)     += fb_ssd1306.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1305)     += fb_ssd1325.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1331)     += fb_ssd1331.o
obj-$(CONFIG_TINYDRM_FB_TFT_SSD1351)     += fb_ssd1351.o
obj-$(CONFIG_TINYDRM_FB_TFT_ST7735R)     += fb_st7735r.o
obj-$(CONFIG_TINYDRM_FB_TFT_ST7789V)     += fb_st7789v.o
obj-$(CONFIG_TINYDRM_FB_TFT_TINYLCD)     += fb_tinylcd.o
obj-$(CONFIG_TINYDRM_FB_TFT_TLS8204)     += fb_tls8204.o
obj-$(CONFIG_TINYDRM_FB_TFT_UC1701)      += fb_uc1701.o
obj-$(CONFIG_TINYDRM_FB_TFT_UPD161704)   += fb_upd161704.o
obj-$(CONFIG_TINYDRM_FB_TFT_WATTEROTT)   += fb_watterott.o
