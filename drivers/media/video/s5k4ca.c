/*
 * Driver for S5K4CA (QXGA camera) from Samsung Electronics
 *
 * 1/4" 3.2Mp CMOS Image Sensor SoC with an Embedded Image Processor
 *
 * Original driver for Samsung Galaxy GT-i5800:
 * Copyright (C) 2009, Jinsung Yang <jsgood.yang@samsung.com>
 *
 * Complete rewrite:
 * Copyright 2012, Tomasz Figa <tomasz.figa at gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-ctrls.h>
#include <media/videodev2_samsung.h>
#include <media/s5k4ca_platform.h>

#include "s5k4ca.h"

#define S5K4CA_DRIVER_NAME	"s5k4ca"

#define VIEW_FUNCTION_CALL

#ifdef VIEW_FUNCTION_CALL
#define TRACE_CALL	\
	printk("[S5k4CA] function %s line %d executed\n", __func__, __LINE__);
#else
#define TRACE_CALL
#endif

#define S5K4CA_WIN_WIDTH_MAX		2048
#define S5K4CA_WIN_HEIGHT_MAX		1536
#define S5K4CA_WIN_WIDTH_MIN		8
#define S5K4CA_WIN_HEIGHT_MIN		8

struct s5k4ca_ctrls {
	struct v4l2_ctrl_handler handler;
	/* TODO */
};

struct s5k4ca_preset {
	/* output pixel format and resolution */
	struct v4l2_mbus_framefmt mbus_fmt;
	u8 clk_id;
	u8 index;
};

struct s5k4ca_interval {
	u16 reg_fr_time;
	struct v4l2_fract interval;
	/* Maximum rectangle for the interval */
	struct v4l2_frmsize_discrete size;
};

struct s5k4ca_format {
	unsigned int width;
	unsigned int height;
	struct s5k4ca_request *table;
	unsigned int table_length;
	unsigned int preview;
};

struct s5k4ca_state {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct i2c_client *client;
	struct s5k4ca_platform_data *pdata;

	int frame_rate;
	int focus_mode;
	int auto_focus_result;
	int color_effect;
	int scene_mode;
	int brightness;
	int contrast;
	int saturation;
	int sharpness;
	int iso;
	int photometry;
	int white_balance;
	int capture;
	int ae_awb_lock;

	struct mutex lock;

	struct s5k4ca_ctrls ctrls;
	struct s5k4ca_preset preset;

	unsigned int powered:1;
	unsigned int streaming:1;
	unsigned int apply_cfg:1;

	u8 burst_buffer[2500];
};

#define S5K4CA_FORMAT_PREVIEW(w, h, table) \
	{ (w), (h), (table), ARRAY_SIZE((table)), 1 }
#define S5K4CA_FORMAT(w, h, table) \
	{ (w), (h), (table), ARRAY_SIZE((table)), 0 }

static struct s5k4ca_format s5k4ca_formats[] = {
	/* Formats supported by both preview and capture */
	S5K4CA_FORMAT_PREVIEW( 640,  480, s5k4ca_res_vga),
	S5K4CA_FORMAT_PREVIEW(1024,  768, s5k4ca_res_xga),
	/* Formats supported only in capture mode */
	S5K4CA_FORMAT(1280,  960, s5k4ca_res_sxga),
	S5K4CA_FORMAT(1600, 1200, s5k4ca_res_uxga),
	S5K4CA_FORMAT(2048, 1536, s5k4ca_res_qxga),
};



static const struct s5k4ca_interval s5k4ca_intervals[] = {
	{ 1401, {140100, 1000000}, {2048, 1536} }, /*  7.138 fps */
	{ 666,  { 66600, 1000000}, {2048, 1536} }, /* 15.015 fps */
	{ 334,  { 33400, 1000000}, {640,  480 } }, /* 29.940 fps */
};

/*
 * Utility functions
 */

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler,
				struct s5k4ca_state, ctrls.handler)->sd;
}

static inline struct s5k4ca_state *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k4ca_state, sd);
}

/*
 * Register access
 */

static int s5k4ca_sensor_write(struct s5k4ca_state *state,
				unsigned short subaddr, unsigned short val)
{
	struct i2c_client *client = state->client;
	unsigned char buf[] = {
		subaddr >> 8, subaddr & 0xff, val >> 8, val & 0xff
	};
	return i2c_master_send(client, buf, sizeof(buf));
}

static int s5k4ca_write_regs(struct s5k4ca_state *state,
					struct s5k4ca_request table[], int size)
{
	struct i2c_client *client = state->client;
	u8 *buffer = state->burst_buffer;
	u8 *ptr = &buffer[2];
	int err = 0;
	int i = 0;

	buffer[0] = S5K4CA_DATA_MAGIC >> 8;
	buffer[1] = S5K4CA_DATA_MAGIC & 0xff;

	for (i = 0; i < size && err >= 0; ++i) {
		switch (table[i].subaddr) {
		case S5K4CA_BANK_MAGIC:
		case S5K4CA_PAGE_MAGIC:
		case S5K4CA_REG_MAGIC:
			if (ptr != &buffer[2]) {
				/* write in burst mode */
				err = i2c_master_send(client,
							buffer, ptr - buffer);
				ptr = &buffer[2];
				if (err < 0)
					break;
			}
			/* Set Address */
			err = s5k4ca_sensor_write(state,
					table[i].subaddr, table[i].value);
			break;
		case S5K4CA_DATA_MAGIC:
			/* make and fill buffer for burst mode write */
			*ptr++ = table[i].value >> 8;
			*ptr++ = table[i].value & 0xff;
			break;
		case S5K4CA_MSLEEP_MAGIC:
			msleep(table[i].value);
			break;
		}
	}

	if (ptr != &buffer[2])
		/* write in burst mode */
		err = i2c_master_send(client, buffer, ptr - buffer);

	if (unlikely(err < 0)) {
		v4l_err(client, "%s: register set failed\n", __func__);
		return err;
	}

	return 0;
}

static int s5k4ca_sensor_read(struct s5k4ca_state *state,
				unsigned short subaddr, unsigned short *data)
{
	struct i2c_client *client = state->client;
	unsigned char buf[] = { 0x0F, 0x12 };
	int ret;

	TRACE_CALL;

	s5k4ca_sensor_write(state, 0xFCFC, 0xD000);
	s5k4ca_sensor_write(state, 0x002C, 0x7000);
	s5k4ca_sensor_write(state, 0x002E, subaddr);

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		goto error;

	ret = i2c_master_recv(client, buf, sizeof(buf));
	if (ret < 0)
		goto error;

	*data = ((buf[0] << 8) | buf[1]);

error:
	return ret;
}

/*
 * Control handlers
 */

static int s5k4ca_set_wb(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	switch (type) {
	case WHITE_BALANCE_AUTO:
		state->white_balance = 0;
		printk("-> WB auto mode\n");
		ret = s5k4ca_write_regs(state, s5k4ca_wb_auto,
					ARRAY_SIZE(s5k4ca_wb_auto));
		break;
	case WHITE_BALANCE_SUNNY:
		state->white_balance = 1;
		printk("-> WB Sunny mode\n");
		ret = s5k4ca_write_regs(state, s5k4ca_wb_sunny,
					ARRAY_SIZE(s5k4ca_wb_sunny));
		break;
	case WHITE_BALANCE_CLOUDY:
		state->white_balance = 2;
		printk("-> WB Cloudy mode\n");
		ret = s5k4ca_write_regs(state, s5k4ca_wb_cloudy,
					ARRAY_SIZE(s5k4ca_wb_cloudy));
		break;
	case WHITE_BALANCE_TUNGSTEN:
		state->white_balance = 3;
		printk("-> WB Tungsten mode\n");
		ret = s5k4ca_write_regs(state, s5k4ca_wb_tungsten,
					ARRAY_SIZE(s5k4ca_wb_tungsten));
		break;
	case WHITE_BALANCE_FLUORESCENT:
		state->white_balance = 4;
		printk("-> WB Flourescent mode\n");
		ret = s5k4ca_write_regs(state, s5k4ca_wb_fluorescent,
					ARRAY_SIZE(s5k4ca_wb_fluorescent));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->white_balance = type;
	return 0;
}

static int s5k4ca_set_effect(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Effects Mode %d", type);

	switch (type) {
	case IMAGE_EFFECT_NONE:
		printk("-> Mode None\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_off,
					ARRAY_SIZE(s5k4ca_effect_off));
		break;
	case IMAGE_EFFECT_BNW:
		printk("-> Mode Gray\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_gray,
					ARRAY_SIZE(s5k4ca_effect_gray));
		break;
	case IMAGE_EFFECT_NEGATIVE:
		printk("-> Mode Negative\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_negative,
					ARRAY_SIZE(s5k4ca_effect_negative));
		break;
	case IMAGE_EFFECT_SEPIA:
		printk("-> Mode Sepia\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_sepia,
					ARRAY_SIZE(s5k4ca_effect_sepia));
		break;
	case IMAGE_EFFECT_AQUA:
		printk("-> Mode Aqua\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_aqua,
					ARRAY_SIZE(s5k4ca_effect_aqua));
		break;
	case IMAGE_EFFECT_ANTIQUE:
		printk("-> Mode Sketch\n");
		ret = s5k4ca_write_regs(state, s5k4ca_effect_sketch,
					ARRAY_SIZE(s5k4ca_effect_sketch));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->color_effect = type;
	return 0;
}

static int s5k4ca_set_scene_mode(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("\n[S5k4ca] scene mode type is %d\n", type);

	ret = s5k4ca_write_regs(state, s5k4ca_scene_auto,
						ARRAY_SIZE(s5k4ca_scene_auto));
	if (ret < 0)
		return ret;

	switch (type) {
	case SCENE_MODE_NONE:
		break;
	case SCENE_MODE_PORTRAIT:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_portrait,
					ARRAY_SIZE(s5k4ca_scene_portrait));
		break;
	case SCENE_MODE_LANDSCAPE:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_landscape,
					ARRAY_SIZE(s5k4ca_scene_landscape));
		break;
	case SCENE_MODE_SPORTS:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_sport,
					ARRAY_SIZE(s5k4ca_scene_sport));
		break;
	case SCENE_MODE_SUNSET:
	case SCENE_MODE_CANDLE_LIGHT:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_sunset_candlelight,
				ARRAY_SIZE(s5k4ca_scene_sunset_candlelight));
		break;
	case SCENE_MODE_FIREWORKS:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_fireworks,
					ARRAY_SIZE(s5k4ca_scene_fireworks));
		break;
	case SCENE_MODE_TEXT:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_text,
					ARRAY_SIZE(s5k4ca_scene_text));
		break;
	case SCENE_MODE_NIGHTSHOT:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_night,
					ARRAY_SIZE(s5k4ca_scene_night));
		break;
	case SCENE_MODE_BEACH_SNOW:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_beach,
					ARRAY_SIZE(s5k4ca_scene_beach));
		break;
	case SCENE_MODE_PARTY_INDOOR:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_party,
					ARRAY_SIZE(s5k4ca_scene_party));
		break;
	case SCENE_MODE_BACK_LIGHT:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_backlight,
					ARRAY_SIZE(s5k4ca_scene_backlight));
		break;
	case SCENE_MODE_DUST_DAWN:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_duskdawn,
					ARRAY_SIZE(s5k4ca_scene_duskdawn));
		break;
	case SCENE_MODE_FALL_COLOR:
		ret = s5k4ca_write_regs(state, s5k4ca_scene_fallcolor,
					ARRAY_SIZE(s5k4ca_scene_fallcolor));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->scene_mode = type;
	return 0;
}

static int s5k4ca_set_br(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Brightness Mode %d", type);

	switch (type) {
	case EV_MINUS_4:
		ret = s5k4ca_write_regs(state, s5k4ca_br_minus4,
						ARRAY_SIZE(s5k4ca_br_minus4));
		break;
	case EV_MINUS_3:
		ret = s5k4ca_write_regs(state, s5k4ca_br_minus3,
						ARRAY_SIZE(s5k4ca_br_minus3));
		break;
	case EV_MINUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_br_minus2,
						ARRAY_SIZE(s5k4ca_br_minus2));
		break;
	case EV_MINUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_br_minus1,
						ARRAY_SIZE(s5k4ca_br_minus1));
		break;
	case EV_DEFAULT:
		ret = s5k4ca_write_regs(state, s5k4ca_br_zero,
						ARRAY_SIZE(s5k4ca_br_zero));
		break;
	case EV_PLUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_br_plus1,
						ARRAY_SIZE(s5k4ca_br_plus1));
		break;
	case EV_PLUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_br_plus2,
						ARRAY_SIZE(s5k4ca_br_plus2));
		break;
	case EV_PLUS_3:
		ret = s5k4ca_write_regs(state, s5k4ca_br_plus3,
						ARRAY_SIZE(s5k4ca_br_plus3));
		break;
	case EV_PLUS_4:
		ret = s5k4ca_write_regs(state, s5k4ca_br_plus4,
						ARRAY_SIZE(s5k4ca_br_plus4));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->brightness = type;
	return 0;
}

static int s5k4ca_set_contrast(struct v4l2_subdev *sd, int type)
{
	int ret = 0;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Contras Mode %d",type);

	switch (type) {
	case CONTRAST_MINUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_contrast_m2,
					ARRAY_SIZE(s5k4ca_contrast_m2));
		break;
	case CONTRAST_MINUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_contrast_m1,
					ARRAY_SIZE(s5k4ca_contrast_m1));
		break;
	case CONTRAST_DEFAULT:
		ret = s5k4ca_write_regs(state, s5k4ca_contrast_0,
					ARRAY_SIZE(s5k4ca_contrast_0));
		break;
	case CONTRAST_PLUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_contrast_p1,
					ARRAY_SIZE(s5k4ca_contrast_p1));
		break;
	case CONTRAST_PLUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_contrast_p2,
					ARRAY_SIZE(s5k4ca_contrast_p2));
		break;
	}

	if (ret < 0)
		return ret;

	state->contrast = type;
	return 0;
}

static int s5k4ca_set_saturation(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Saturation Mode %d",type);

	switch (type) {
	case SATURATION_MINUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_Saturation_m2,
					ARRAY_SIZE(s5k4ca_Saturation_m2));
		break;
	case SATURATION_MINUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_Saturation_m1,
					ARRAY_SIZE(s5k4ca_Saturation_m1));
		break;
	case SATURATION_DEFAULT:
		ret = s5k4ca_write_regs(state, s5k4ca_Saturation_0,
					ARRAY_SIZE(s5k4ca_Saturation_0));
		break;
	case SATURATION_PLUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_Saturation_p1,
					ARRAY_SIZE(s5k4ca_Saturation_p1));
		break;
	case SATURATION_PLUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_Saturation_p2,
					ARRAY_SIZE(s5k4ca_Saturation_p2));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->saturation = type;
	return 0;
}

static int s5k4ca_set_sharpness(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Sharpness Mode %d",type);

	switch (type) {
	case SHARPNESS_MINUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_Sharpness_m2,
					ARRAY_SIZE(s5k4ca_Sharpness_m2));
		break;
	case SHARPNESS_MINUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_Sharpness_m1,
					ARRAY_SIZE(s5k4ca_Sharpness_m1));
		break;
	case SHARPNESS_DEFAULT:
		ret = s5k4ca_write_regs(state, s5k4ca_Sharpness_0,
					ARRAY_SIZE(s5k4ca_Sharpness_0));
		break;
	case SHARPNESS_PLUS_1:
		ret = s5k4ca_write_regs(state, s5k4ca_Sharpness_p1,
					ARRAY_SIZE(s5k4ca_Sharpness_p1));
		break;
	case SHARPNESS_PLUS_2:
		ret = s5k4ca_write_regs(state, s5k4ca_Sharpness_p2,
					ARRAY_SIZE(s5k4ca_Sharpness_p2));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->sharpness = type;
	return 0;
}

static int s5k4ca_set_iso(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Iso Mode %d",type);

	switch (type) {
	case ISO_AUTO:
		printk("-> ISO AUTO\n");
		ret = s5k4ca_write_regs(state, s5k4ca_iso_auto,
					ARRAY_SIZE(s5k4ca_iso_auto));
		break;
	case ISO_50:
		printk("-> ISO 50\n");
		ret = s5k4ca_write_regs(state, s5k4ca_iso50,
					ARRAY_SIZE(s5k4ca_iso50));
		break;
	case ISO_100:
		printk("-> ISO 100\n");
		ret = s5k4ca_write_regs(state, s5k4ca_iso100,
					ARRAY_SIZE(s5k4ca_iso100));
		break;
	case ISO_200:
		printk("-> ISO 200\n");
		ret = s5k4ca_write_regs(state, s5k4ca_iso200,
					ARRAY_SIZE(s5k4ca_iso200));
		break;
	case ISO_400:
		printk("-> ISO 400\n");
		ret = s5k4ca_write_regs(state, s5k4ca_iso400,
					ARRAY_SIZE(s5k4ca_iso400));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->iso = type;
	return 0;
}

static int s5k4ca_set_photometry(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =Photometry Mode %d", type);

	switch (type) {
	case METERING_SPOT:
		ret = s5k4ca_write_regs(state, s5k4ca_photometry_spot,
					ARRAY_SIZE(s5k4ca_photometry_spot));
		break;
	case METERING_MATRIX:
		ret = s5k4ca_write_regs(state, s5k4ca_photometry_matrix,
					ARRAY_SIZE(s5k4ca_photometry_matrix));
		break;
	case METERING_CENTER:
		ret = s5k4ca_write_regs(state, s5k4ca_photometry_center,
					ARRAY_SIZE(s5k4ca_photometry_center));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->photometry = type;
	return 0;
}

static int s5k4ca_set_ae_awb_lock(struct v4l2_subdev *sd, int type)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =AE AWB Lock Mode %d", type);

	switch (type) {
	case AE_UNLOCK_AWB_UNLOCK:
		ret = s5k4ca_write_regs(state, s5k4ca_awb_ae_unlock,
					ARRAY_SIZE(s5k4ca_awb_ae_unlock));
		break;
	case AE_LOCK_AWB_UNLOCK:
		ret = s5k4ca_write_regs(state, s5k4ca_awb_ae_lock,
					ARRAY_SIZE(s5k4ca_awb_ae_lock));
		break;
	case AE_UNLOCK_AWB_LOCK:
		ret = s5k4ca_write_regs(state, s5k4ca_mwb_ae_unlock,
					ARRAY_SIZE(s5k4ca_mwb_ae_unlock));
		break;
	case AE_LOCK_AWB_LOCK:
		ret = s5k4ca_write_regs(state, s5k4ca_mwb_ae_lock,
					ARRAY_SIZE(s5k4ca_mwb_ae_lock));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->ae_awb_lock = type;
	return 0;
}

static int s5k4ca_framerate_set(struct v4l2_subdev *sd, int rate)
{
	int ret;
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	printk("[CAM-SENSOR] =frame rate = %d\n", rate);

	switch (rate) {
	case FRAME_RATE_AUTO:
		ret = s5k4ca_write_regs(state, s5k4ca_fps_auto,
						ARRAY_SIZE(s5k4ca_fps_auto));
		break;
	case FRAME_RATE_7:
		ret = s5k4ca_write_regs(state, s5k4ca_fps_7,
						ARRAY_SIZE(s5k4ca_fps_7));
		break;
	case FRAME_RATE_15:
		ret = s5k4ca_write_regs(state, s5k4ca_fps_15,
						ARRAY_SIZE(s5k4ca_fps_15));
		break;
	case FRAME_RATE_30:
		ret = s5k4ca_write_regs(state, s5k4ca_fps_30,
						ARRAY_SIZE(s5k4ca_fps_30));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	msleep(300);
	state->frame_rate = rate;
	return 0;
}

static int s5k4ca_set_focus_mode(struct v4l2_subdev *sd, int mode)
{
	struct s5k4ca_state *state = to_state(sd);
	int ret;

	TRACE_CALL;

	switch(mode) {
	case FOCUS_MODE_AUTO:
		ret = s5k4ca_write_regs(state, s5k4ca_focus_mode_normal,
					ARRAY_SIZE(s5k4ca_focus_mode_normal));
		break;
	case FOCUS_MODE_MACRO:
		ret = s5k4ca_write_regs(state, s5k4ca_focus_mode_macro,
					ARRAY_SIZE(s5k4ca_focus_mode_macro));
		break;
	case FOCUS_MODE_INFINITY:
		ret = s5k4ca_write_regs(state, s5k4ca_focus_mode_infinity,
					ARRAY_SIZE(s5k4ca_focus_mode_infinity));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	state->focus_mode = mode;
	return ret;
}

static int s5k4ca_set_capture(struct v4l2_subdev *sd, int mode)
{
	struct s5k4ca_state *state = to_state(sd);
	int ret;

	TRACE_CALL;

	if (mode)
		ret = s5k4ca_write_regs(state, s5k4ca_snapshot_enable,
					ARRAY_SIZE(s5k4ca_snapshot_enable));
	else
		ret = s5k4ca_write_regs(state, s5k4ca_snapshot_disable,
					ARRAY_SIZE(s5k4ca_snapshot_disable));

	if (ret < 0)
		return ret;

	state->capture = mode;
	return ret;
}

static int s5k4ca_set_auto_focus(struct v4l2_subdev *sd, int val)
{
	struct s5k4ca_state *state = to_state(sd);
	int count = 50;
	u16 stat = 0;
	u16 lux_value = 0;
	int ret;

	TRACE_CALL;

	if (state->focus_mode == FOCUS_MODE_INFINITY)
		return 0;

	if (val == AUTO_FOCUS_OFF)
		return s5k4ca_set_focus_mode(sd, state->focus_mode);

	/* Get lux_value. */
	ret = s5k4ca_sensor_read(state, 0x12FE, &lux_value);
	if (ret < 0)
		return ret;

	if (lux_value < 128)  /* Low light AF */
		ret = s5k4ca_write_regs(state, s5k4ca_af_low_lux_val,
					ARRAY_SIZE(s5k4ca_af_low_lux_val));
	else
		ret = s5k4ca_write_regs(state, s5k4ca_af_normal_lux_val,
					ARRAY_SIZE(s5k4ca_af_normal_lux_val));

	if (ret < 0)
		return ret;

	if (state->focus_mode == FOCUS_MODE_MACRO)
		ret = s5k4ca_write_regs(state, s5k4ca_af_start_macro,
					ARRAY_SIZE(s5k4ca_af_start_macro));
	else
		ret = s5k4ca_write_regs(state, s5k4ca_af_start_normal,
					ARRAY_SIZE(s5k4ca_af_start_normal));

	if (ret < 0)
		return ret;

	do {
		if (lux_value < 128)
			msleep(250);
		else
			msleep(100);

		ret = s5k4ca_sensor_read(state, 0x130E, &stat);
		if (ret < 0)
			return ret;
	} while (--count && (stat & 3) < 2);

	if (!count || (stat & 3) == 2) {
		if (state->focus_mode == FOCUS_MODE_MACRO)
			ret = s5k4ca_write_regs(state, s5k4ca_af_stop_macro,
					ARRAY_SIZE(s5k4ca_af_stop_macro));
		else
			ret = s5k4ca_write_regs(state, s5k4ca_af_stop_normal,
					ARRAY_SIZE(s5k4ca_af_stop_normal));

		if (ret < 0)
			return ret;

		printk("[CAM-SENSOR] =Auto focus failed\n");

		state->auto_focus_result = 0;
		return 0;
	}

	printk("[CAM-SENSOR] =Auto focus successful\n");
	state->auto_focus_result = 1;
	return 0;
}

/*
 * V4L2 ctrl ops
 */

static int s5k4ca_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct s5k4ca_state *state = to_state(sd);
	int err = 0;

	TRACE_CALL;

	mutex_lock(&state->lock);

	if (!state->powered)
		goto unlock;

	printk("[S5k4CA] %s function ctrl->id : %d \n", __func__, ctrl->id);

	switch (ctrl->id) {
	case V4L2_CID_CAMERA_FRAME_RATE:
		err = s5k4ca_framerate_set(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_FOCUS_MODE:
		err = s5k4ca_set_focus_mode(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_SET_AUTO_FOCUS:
		err = s5k4ca_set_auto_focus(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_WHITE_BALANCE:
		err = s5k4ca_set_wb(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_EFFECT:
		err = s5k4ca_set_effect(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_SCENE_MODE:
		err = s5k4ca_set_scene_mode(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_BRIGHTNESS:
		err = s5k4ca_set_br(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_CONTRAST:
		err = s5k4ca_set_contrast(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_SATURATION:
		err = s5k4ca_set_saturation(sd, ctrl->val);
		break;
	case V4L2_CID_SHARPNESS:
		err = s5k4ca_set_sharpness(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_SHARPNESS:
		err = s5k4ca_set_iso(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_METERING:
		err = s5k4ca_set_photometry(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_AEAWB_LOCK_UNLOCK:
		err = s5k4ca_set_ae_awb_lock(sd, ctrl->val);
		break;
	case V4L2_CID_CAMERA_CAPTURE:
		err = s5k4ca_set_capture(sd, ctrl->val);
		break;
	}

unlock:
	mutex_unlock(&state->lock);
	return err;
}

static const struct v4l2_ctrl_ops s5k4ca_ctrl_ops = {
	.s_ctrl	= s5k4ca_s_ctrl,
};

/*
 * V4L2 subdev pad ops
 */

static void s5k4ca_bound_image(struct s5k4ca_state *state, u32 *w, u32 *h)
{
	int i;

	TRACE_CALL;

	for (i = 0; i < ARRAY_SIZE(s5k4ca_formats); ++i) {
		if (!((!state->capture) & s5k4ca_formats[i].preview)) {
			--i;
			break;
		}
		if (*w <= s5k4ca_formats[i].width
		    && *h <= s5k4ca_formats[i].height)
			break;
	}

	if (i >= ARRAY_SIZE(s5k4ca_formats))
		i = ARRAY_SIZE(s5k4ca_formats) - 1;

	*w = s5k4ca_formats[i].width;
	*h = s5k4ca_formats[i].height;
}

static int s5k4ca_enum_frame_interval(struct v4l2_subdev *sd,
			      struct v4l2_subdev_fh *fh,
			      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct s5k4ca_state *state = to_state(sd);
	const struct s5k4ca_interval *fi;
	int ret = 0;

	TRACE_CALL;

	if (fie->index > ARRAY_SIZE(s5k4ca_intervals))
		return -EINVAL;

	s5k4ca_bound_image(state, &fie->width, &fie->height);

	mutex_lock(&state->lock);
	fi = &s5k4ca_intervals[fie->index];
	if (fie->width > fi->size.width || fie->height > fi->size.height)
		ret = -EINVAL;
	else
		fie->interval = fi->interval;
	mutex_unlock(&state->lock);

	return ret;
}

static int s5k4ca_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_fh *fh,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	TRACE_CALL;

	if (code->index > 0)
		return -EINVAL;

	code->code = V4L2_MBUS_FMT_VYUY8_2X8;
	return 0;
}

static int s5k4ca_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_fh *fh,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k4ca_state *state = to_state(sd);

	TRACE_CALL;

	if (fse->index >= ARRAY_SIZE(s5k4ca_formats))
		return -EINVAL;

	if (!((!state->capture) & s5k4ca_formats[fse->index].preview))
		return -EINVAL;

	fse->code	= V4L2_MBUS_FMT_VYUY8_2X8;
	fse->min_width	= s5k4ca_formats[fse->index].width;
	fse->max_width	= s5k4ca_formats[fse->index].width;
	fse->max_height	= s5k4ca_formats[fse->index].height;
	fse->min_height	= s5k4ca_formats[fse->index].height;

	return 0;
}

static void s5k4ca_try_format(struct s5k4ca_state *state,
						struct v4l2_mbus_framefmt *mf)
{
	TRACE_CALL;

	s5k4ca_bound_image(state, &mf->width, &mf->height);

	mf->colorspace	= V4L2_COLORSPACE_JPEG;
	mf->code	= V4L2_MBUS_FMT_VYUY8_2X8;
	mf->field	= V4L2_FIELD_NONE;
}

static int s5k4ca_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
			  struct v4l2_subdev_format *fmt)
{
	struct s5k4ca_state *s5k4ca = to_state(sd);
	struct v4l2_mbus_framefmt *mf;

	TRACE_CALL;

	memset(fmt->reserved, 0, sizeof(fmt->reserved));

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		mf = v4l2_subdev_get_try_format(fh, 0);
		fmt->format = *mf;
		return 0;
	}

	mutex_lock(&s5k4ca->lock);
	fmt->format = s5k4ca->preset.mbus_fmt;
	mutex_unlock(&s5k4ca->lock);

	return 0;
}

static int s5k4ca_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
			  struct v4l2_subdev_format *fmt)
{
	struct s5k4ca_state *s5k4ca = to_state(sd);
	struct s5k4ca_preset *preset = &s5k4ca->preset;
	struct v4l2_mbus_framefmt *mf;
	int ret = 0;

	TRACE_CALL;

	mutex_lock(&s5k4ca->lock);
	s5k4ca_try_format(s5k4ca, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		mf = v4l2_subdev_get_try_format(fh, fmt->pad);
	} else {
		if (s5k4ca->streaming) {
			ret = -EBUSY;
		} else {
			mf = &preset->mbus_fmt;
			s5k4ca->apply_cfg = 1;
		}
	}

	if (ret == 0)
		*mf = fmt->format;

	mutex_unlock(&s5k4ca->lock);

	return ret;
}

static const struct v4l2_subdev_pad_ops s5k4ca_pad_ops = {
	.enum_mbus_code		= s5k4ca_enum_mbus_code,
	.enum_frame_size	= s5k4ca_enum_frame_size,
	.enum_frame_interval	= s5k4ca_enum_frame_interval,
	.get_fmt		= s5k4ca_get_fmt,
	.set_fmt		= s5k4ca_set_fmt,
};

/*
 * V4L2 subdev core ops
 */

static int s5k4ca_s_power(struct v4l2_subdev *sd, int on)
{
	struct s5k4ca_state *state = to_state(sd);
	int ret = 0;
	u16 stat = 0;

	TRACE_CALL;

	mutex_lock(&state->lock);

	if (!!on == state->powered)
		goto unlock;

	if (!on) {
		state->streaming = 0;
		state->powered = 0;
		if (state->pdata->set_power)
			state->pdata->set_power(0);
		goto unlock;
	}

	v4l_info(state->client, "%s: camera initialization start\n", __func__);

	if (state->pdata->set_power)
		state->pdata->set_power(1);

	TRACE_CALL;

	ret = s5k4ca_write_regs(state, s5k4ca_init, ARRAY_SIZE(s5k4ca_init));
	if (ret < 0)
		goto unlock;

	TRACE_CALL;

	ret = s5k4ca_sensor_read(state, 0x02e8, &stat);
	if (ret < 0)
		goto unlock;

	v4l2_info(sd, "Camera preview status = %d\n", stat);

	state->powered = 1;

	ret = v4l2_ctrl_handler_setup(sd->ctrl_handler);

unlock:
	mutex_unlock(&state->lock);
	return ret;
}

static int s5k4ca_log_status(struct v4l2_subdev *sd)
{
	TRACE_CALL;
	v4l2_ctrl_handler_log_status(sd->ctrl_handler, sd->name);
	return 0;
}

static const struct v4l2_subdev_core_ops s5k4ca_core_ops = {
	.s_power		= s5k4ca_s_power,
	.log_status		= s5k4ca_log_status,
};

/*
 * V4L2 subdev video ops
 */

static int s5k4ca_apply_cfg(struct s5k4ca_state *state)
{
	struct v4l2_mbus_framefmt *fmt = &state->preset.mbus_fmt;
	int ret;
	int i;

	TRACE_CALL;

	for (i = 0; i < ARRAY_SIZE(s5k4ca_formats); ++i)
		if (fmt->width <= s5k4ca_formats[i].width
		    && fmt->height <= s5k4ca_formats[i].height)
			break;

	if (i >= ARRAY_SIZE(s5k4ca_formats))
		return -EINVAL;

	ret = s5k4ca_write_regs(state, s5k4ca_formats[i].table,
						s5k4ca_formats[i].table_length);
	if (ret < 0)
		return ret;

	state->apply_cfg = 0;
	msleep(300);
	return 0;
}

static int s5k4ca_stream(struct s5k4ca_state *s5k4ca, int enable)
{
	int ret = 0;
	u16 stat = 0;

	TRACE_CALL;

	if (enable)
		ret = s5k4ca_write_regs(s5k4ca, s5k4ca_preview_enable,
					ARRAY_SIZE(s5k4ca_preview_enable));
	else
		ret = s5k4ca_write_regs(s5k4ca, s5k4ca_preview_disable,
					ARRAY_SIZE(s5k4ca_preview_disable));

	if (ret < 0)
		return ret;

	if (enable) {
		ret = s5k4ca_sensor_read(s5k4ca, 0x02e8, &stat);
		if (ret < 0)
			return ret;
		if (stat) {
			v4l2_err(&s5k4ca->sd,
				"Failed to enable streaming (stat=%d)\n", stat);
			return -EFAULT;
		}
	}

	s5k4ca->streaming = enable;
	return 0;
}

static int s5k4ca_s_stream(struct v4l2_subdev *sd, int on)
{
	struct s5k4ca_state *s5k4ca = to_state(sd);
	int ret = 0;

	TRACE_CALL;

	mutex_lock(&s5k4ca->lock);

	if (s5k4ca->streaming == !on) {
		if (!ret && s5k4ca->apply_cfg)
			ret = s5k4ca_apply_cfg(s5k4ca);
		if (!ret)
			ret = s5k4ca_stream(s5k4ca, !!on);
	}
	mutex_unlock(&s5k4ca->lock);

	return ret;
}

static const struct v4l2_subdev_video_ops s5k4ca_video_ops = {
	.s_stream		= s5k4ca_s_stream,
};

/*
 * V4L2 subdev internal ops
 */

static int s5k4ca_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format = v4l2_subdev_get_try_format(fh, 0);
	struct v4l2_rect *crop = v4l2_subdev_get_try_crop(fh, 0);

	TRACE_CALL;

	format->colorspace = V4L2_COLORSPACE_JPEG;
	format->code = V4L2_MBUS_FMT_VYUY8_2X8;
	format->width = S5K4CA_WIN_WIDTH_MAX;
	format->height = S5K4CA_WIN_HEIGHT_MAX;
	format->field = V4L2_FIELD_NONE;

	crop->width = S5K4CA_WIN_WIDTH_MAX;
	crop->height = S5K4CA_WIN_HEIGHT_MAX;
	crop->left = 0;
	crop->top = 0;

	return 0;
}

static const struct v4l2_subdev_internal_ops s5k4ca_subdev_internal_ops = {
	.open = s5k4ca_open,
};

/*
 * V4L2 I2C driver
 */

static int s5k4ca_initialize_ctrls(struct s5k4ca_state *state)
{
	const struct v4l2_ctrl_ops *ops = &s5k4ca_ctrl_ops;
	struct s5k4ca_ctrls *ctrls = &state->ctrls;
	struct v4l2_ctrl_handler *hdl = &ctrls->handler;
	int ret;

	TRACE_CALL;

	ret = v4l2_ctrl_handler_init(hdl, 16);
	if (ret)
		return ret;

	/* TODO: Setup controls here */

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	state->sd.ctrl_handler = hdl;
	return 0;
}

static const struct v4l2_subdev_ops s5k4ca_ops = {
	.core = &s5k4ca_core_ops,
	.pad = &s5k4ca_pad_ops,
	.video = &s5k4ca_video_ops,
};

static int s5k4ca_probe(struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct s5k4ca_platform_data *pdata;
	struct s5k4ca_state *state;
	struct v4l2_subdev *sd;
	int ret;

	TRACE_CALL;

	pdata = client->dev.platform_data;
	if (!pdata) {
		dev_err(&client->dev, "%s: no platform data\n", __func__);
		return -ENODEV;
	}

	state = kzalloc(sizeof(struct s5k4ca_state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);

	state->client = client;
	state->pdata = pdata;

	sd = &state->sd;
	strcpy(sd->name, S5K4CA_DRIVER_NAME);

	v4l2_i2c_subdev_init(sd, client, &s5k4ca_ops);

	sd->internal_ops = &s5k4ca_subdev_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	state->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	ret = media_entity_init(&sd->entity, 1, &state->pad, 0);
	if (ret)
		goto err_free_mem;

	ret = s5k4ca_initialize_ctrls(state);
	if (ret)
		goto err_media_entity_cleanup;

	dev_info(&client->dev, "s5k4ca has been probed\n");

	return 0;

err_media_entity_cleanup:
	media_entity_cleanup(&sd->entity);
err_free_mem:
	kfree(state);

	return ret;
}

static int s5k4ca_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	TRACE_CALL;

	v4l2_device_unregister_subdev(sd);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	media_entity_cleanup(&sd->entity);
	kfree(to_state(sd));

	return 0;
}

static const struct i2c_device_id s5k4ca_id[] = {
	{ S5K4CA_DRIVER_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, s5k4ca_id);

static struct i2c_driver s5k4ca_i2c_driver = {
	.driver = {
		.name = S5K4CA_DRIVER_NAME,
	},
	.probe		= s5k4ca_probe,
	.remove		= s5k4ca_remove,
	.id_table	= s5k4ca_id,
};

static int __init s5k4ca_mod_init(void)
{
	return i2c_add_driver(&s5k4ca_i2c_driver);
}

static void __exit s5k4ca_mod_exit(void)
{
	i2c_del_driver(&s5k4ca_i2c_driver);
}

module_init(s5k4ca_mod_init);
module_exit(s5k4ca_mod_exit);

MODULE_DESCRIPTION("Samsung Electronics S5K4CA QXGA camera driver");
MODULE_AUTHOR("Tomasz Figa <tomasz.figa at gmail.com>");
MODULE_LICENSE("GPL");
