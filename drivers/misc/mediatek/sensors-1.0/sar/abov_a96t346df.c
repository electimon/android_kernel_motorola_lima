/*
 * file abov_sar.c
 * brief abov Driver for two channel SAP using
 *
 * Driver for the ABOV
 * Copyright (c) 2015-2016 ABOV Corp
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/device.h>
//#include <linux/sensors.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>
#include <linux/usb.h>
#include <linux/power_supply.h>
#include <linux/version.h>
#if defined(CONFIG_FB)
#include <linux/fb.h>
#endif
#include "abov_a96t346df.h" /* main struct, interrupt,init,pointers */

#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/async.h>
#include <linux/firmware.h>
#include "../situation/situation.h"
#define SLEEP(x)	mdelay(x)

#define C_I2C_FIFO_SIZE 8

#define I2C_MASK_FLAG	(0x00ff)

static u8 checksum_h;
static u8 checksum_h_bin;
static u8 checksum_l;
static u8 checksum_l_bin;

#define IDLE 0
#define ACTIVE 1
#define S_PROX   1
#define S_BODY   2

#define DRIVER_NAME "abov_sar"

#define ABOV_DEBUG 1
#define LOG_TAG "ABOV"

#define USE_KERNEL_SUSPEND

#if ABOV_DEBUG
#define LOG_INFO(fmt, args...)    pr_info(LOG_TAG fmt, ##args)
#else
#define LOG_INFO(fmt, args...)
#endif

#define LOG_DBG(fmt, args...)	pr_info(LOG_TAG fmt, ##args)
#define LOG_ERR(fmt, args...)   pr_err(LOG_TAG fmt, ##args)

static int last_val;
int mEnabled = 0;
static int fw_dl_status;
static int programming_done;
static bool user_debug = false;
static bool i2c_added = false;
pabovXX_t abov_sar_ptr;

/**
 * struct abov
 * Specialized struct containing input event data, platform data, and
 * last cap state read if needed.
 */
typedef struct abov {
	pbuttonInformation_t pbuttonInformation;
	pabov_platform_data_t hw; /* specific platform data settings */
} abov_t, *pabov_t;

static void ForcetoTouched(pabovXX_t this)
{
	pabov_t pDevice = NULL;

	struct _buttonInfo *pCurrentButton  = NULL;

	pDevice = this->pDevice;
	if (this && pDevice) {
		LOG_INFO("ForcetoTouched()\n");
		pCurrentButton = pDevice->pbuttonInformation->buttons;
		pCurrentButton->state = ACTIVE;
		last_val = 1;
		if (mEnabled) {
            this->report_data = 1;
			abovXX_sar_data_report(this,CHANNEL_TOP);
            this->report_data = 1;
			abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
            this->report_data = 1;
			abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
		}
		LOG_INFO("Leaving ForcetoTouched()\n");
	}
}

/**
 * fn static int write_register(pabovXX_t this, u8 address, u8 value)
 * brief Sends a write register to the device
 * param this Pointer to main parent struct
 * param address 8-bit register address
 * param value   8-bit register value to write to address
 * return Value from i2c_master_send
 */
static int write_register(pabovXX_t this, u8 address, u8 value)
{
	struct i2c_client *i2c = 0;
	char buffer[2];
	int returnValue = 0;

	buffer[0] = address;
	buffer[1] = value;
	returnValue = -ENOMEM;
	if (this && this->bus) {
		i2c = this->bus;

		returnValue = i2c_master_send(i2c, buffer, 2);
		LOG_INFO("write_register Addr: 0x%x Val: 0x%x Return: %d\n",
				address, value, returnValue);
	}
	if (returnValue < 0) {
		ForcetoTouched(this);
		LOG_DBG("Write_register-ForcetoTouched()\n");
	}
	return returnValue;
}

/**
 * fn static int read_register(pabovXX_t this, u8 address, u8 *value)
 * brief Reads a register's value from the device
 * param this Pointer to main parent struct
 * param address 8-Bit address to read from
 * param value Pointer to 8-bit value to save register value to
 * return Value from i2c_smbus_read_byte_data if < 0. else 0
 */
static int read_register(pabovXX_t this, u8 address, u8 *value)
{
	struct i2c_client *i2c = 0;
	s32 returnValue = 0;

	if (this && value && this->bus) {
		i2c = this->bus;
		returnValue = i2c_smbus_read_byte_data(i2c, address);
		LOG_INFO("read_register Addr: 0x%x Return: 0x%x\n",
				address, returnValue);
		if (returnValue >= 0) {
			*value = returnValue;
			return 0;
		} else {
			return returnValue;
		}
	}
	ForcetoTouched(this);
	LOG_INFO("read_register-ForcetoTouched()\n");
	return -ENOMEM;
}

/**
 * detect if abov exist or not
 * return 1 if chip exist else return 0
 */
static int abov_detect(struct i2c_client *client)
{
	s32 returnValue = 0, i;
	u8 address = ABOV_ABOV_WHOAMI_REG;
	u8 value = 0xAB;

	if (client) {
		for (i = 0; i < 3; i++) {
			returnValue = i2c_smbus_read_byte_data(client, address);
			LOG_INFO("abov read_register for %d time Addr: 0x%02x Return: 0x%02x\n",
					i, address, returnValue);
			if (returnValue >= 0) {
				if (value == returnValue) {
					LOG_INFO("abov detect success!\n");
					return 1;
				}
			}
		}

		for (i = 0; i < 3; i++) {
				if(abov_tk_fw_mode_enter(client) == 0) {
					LOG_INFO("abov boot detect success!\n");
					return 2;
				}
		}
	}
	LOG_INFO("abov detect failed!!!\n");
	return 0;
}


/**
 * brief Perform a manual offset calibration
 * param this Pointer to main parent struct
 * return Value return value from the write register
 */
static int manual_offset_calibration(pabovXX_t this)
{
	s32 returnValue = 0;

	returnValue = write_register(this, ABOV_RECALI_REG, 0x01);
	return returnValue;
}

/**
 * brief sysfs show function for manual calibration which currently just
 * returns register value.
 */
static ssize_t manual_offset_calibration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u8 reg_value = 0;
	pabovXX_t this = dev_get_drvdata(dev);

	LOG_INFO("Reading IRQSTAT_REG\n");
	read_register(this, ABOV_IRQSTAT_REG, &reg_value);
	return scnprintf(buf, PAGE_SIZE, "%d\n", reg_value);
}

/* brief sysfs store function for manual calibration */
static ssize_t manual_offset_calibration_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = dev_get_drvdata(dev);
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;
	if (val) {
		LOG_INFO("Performing manual_offset_calibration()\n");
		manual_offset_calibration(this);
	}
	return count;
}

static DEVICE_ATTR(calibrate, 0644, manual_offset_calibration_show,
		manual_offset_calibration_store);
static struct attribute *abov_attributes[] = {
	&dev_attr_calibrate.attr,
	NULL,
};
static struct attribute_group abov_attr_group = {
	.attrs = abov_attributes,
};

/**
 * fn static int read_regStat(pabovXX_t this)
 * brief Shortcut to read what caused interrupt.
 * details This is to keep the drivers a unified
 * function that will read whatever register(s)
 * provide information on why the interrupt was caused.
 * param this Pointer to main parent struct
 * return If successful, Value of bit(s) that cause interrupt, else 0
 */
static int read_regStat(pabovXX_t this)
{
	u8 data = 0;

	if (this) {
		if (read_register(this, ABOV_IRQSTAT_REG, &data) == 0)
			return (data & 0x00FF);
	}
	return 0;
}

/**
 * brief  Initialize I2C config from platform data
 * param this Pointer to main parent struct
 *
 */
static void hw_init(pabovXX_t this)
{
	pabov_t pDevice = 0;
	pabov_platform_data_t pdata = 0;
	int i = 0;
	/* configure device */
	LOG_INFO("Going to Setup I2C Registers\n");
	pDevice = this->pDevice;
	pdata = pDevice->hw;
	if (this && pDevice && pdata) {
		while (i < pdata->i2c_reg_num) {
			/* Write all registers/values contained in i2c_reg */
			LOG_INFO("Going to Write Reg: 0x%x Value: 0x%x\n",
					pdata->pi2c_reg[i].reg, pdata->pi2c_reg[i].val);
			write_register(this, pdata->pi2c_reg[i].reg,
					pdata->pi2c_reg[i].val);
			i++;
		}
	} else {
		LOG_DBG("ERROR! platform data 0x%p\n", pDevice->hw);
		/* Force to touched if error */
		ForcetoTouched(this);
		LOG_INFO("Hardware_init-ForcetoTouched()\n");
	}
}

/**
 * fn static int initialize(pabovXX_t this)
 * brief Performs all initialization needed to configure the device
 * param this Pointer to main parent struct
 * return Last used command's return value (negative if error)
 */
static int initialize(pabovXX_t this)
{
	int ret;

	if (this) {
		/* prepare reset by disabling any irq handling */
		this->irq_disabled = 1;
		disable_irq(this->irq);
		/* perform a reset */
		ret = write_register(this, ABOV_SOFTRESET_REG,
				0x10);
		if (ret < 0)
			goto error_exit;
		/* wait until the reset has finished by monitoring NIRQ */
		LOG_INFO("Software Reset. Waiting device back to continue.\n");
		/* just sleep for awhile instead of using a loop with reading irq status */
		msleep(300);
		/*
		 *    while(this->get_nirq_low && this->get_nirq_low()) { read_regStat(this); }
		 */
		LOG_INFO("Device back from the reset, continuing. NIRQ = %d\n",
				this->get_nirq_low(this->board->irq_gpio));
		hw_init(this);
		msleep(100); /* make sure everything is running */
		ret = manual_offset_calibration(this);
		if (ret < 0)
			goto error_exit;
		/* re-enable interrupt handling */
		enable_irq(this->irq);
		this->irq_disabled = 0;

		/* make sure no interrupts are pending since enabling irq will only
		 * work on next falling edge */
		read_regStat(this);
		LOG_INFO("Exiting initialize(). NIRQ = %d\n",
				this->get_nirq_low(this->board->irq_gpio));
		programming_done = ACTIVE;
		return 0;
	}
	return -ENOMEM;

error_exit:
	programming_done = IDLE;
	return ret;
}

/**
 * brief Handle what to do when a touch occurs
 * param this Pointer to main parent struct
 */
static void touchProcess(pabovXX_t this)
{
	int counter = 0;
	u8 i = 0;
	int numberOfButtons = 0;
	pabov_t pDevice = NULL;
	struct _buttonInfo *buttons = NULL;
	struct _buttonInfo *pCurrentButton  = NULL;
	struct abov_platform_data *board;

	pDevice = this->pDevice;
	board = this->board;
	if (this && pDevice) {
		LOG_INFO("Inside touchProcess()\n");
		read_register(this, ABOV_IRQSTAT_REG, &i);

		buttons = pDevice->pbuttonInformation->buttons;
		numberOfButtons = pDevice->pbuttonInformation->buttonSize;

		if (unlikely(buttons == NULL)) {
			LOG_DBG("ERROR!! buttons or input NULL!!!\n");
			return;
		}

		for (counter = 0; counter < numberOfButtons; counter++) {
			pCurrentButton = &buttons[counter];
			if (pCurrentButton == NULL) {
				LOG_DBG("ERR!current button index: %d NULL!\n",
						counter);
				return; /* ERRORR!!!! */
			}
			switch (pCurrentButton->state) {
			case IDLE: /* Button is being in far state! */
				if ((i & pCurrentButton->mask) == pCurrentButton->mask) {
					LOG_INFO("CS %d State=BODY.\n",
							counter);
					if (board->cap_channel_top == counter) {
                        this->report_data = S_BODY;
                        abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = S_BODY;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					} else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = S_BODY;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}

					pCurrentButton->state = S_BODY;
					last_val = 2;
				} else if ((i & pCurrentButton->mask) == (pCurrentButton->mask & 0x15)) {
					LOG_INFO("CS %d State=PROX.\n",
							counter);
					if (board->cap_channel_top == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					}
					else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}
					pCurrentButton->state = S_PROX;
					last_val = 0;
				} else {
					LOG_INFO("CS %d still in IDLE State.\n",
							counter);
				}
				break;
			case S_PROX: /* Button is being in proximity! */
				if ((i & pCurrentButton->mask) == pCurrentButton->mask) {
					LOG_INFO("CS %d State=BODY.\n",
							counter);
					if (board->cap_channel_top == counter) {
                        this->report_data = S_BODY;
                        abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = S_BODY;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					} else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = S_BODY;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}
					pCurrentButton->state = S_BODY;
					last_val = 2;
				} else if ((i & pCurrentButton->mask) == (pCurrentButton->mask & 0x15)) {
					LOG_INFO("CS %d still in PROX State.\n",
							counter);
				} else{
					LOG_INFO("CS %d State=IDLE.\n",
							counter);
					if (board->cap_channel_top == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					} else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}
					pCurrentButton->state = IDLE;
					last_val = 0;
				}
				break;
			case S_BODY: /* Button is being in 0mm! */
				if ((i & pCurrentButton->mask) == pCurrentButton->mask) {
					LOG_INFO("CS %d still in BODY State.\n",
							counter);
				} else if ((i & pCurrentButton->mask) == (pCurrentButton->mask & 0x15)) {
					LOG_INFO("CS %d State=PROX.\n",
							counter);
					if (board->cap_channel_top == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					} else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = S_PROX;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}

					pCurrentButton->state = S_PROX;
					last_val = 1;
				} else{
					LOG_INFO("CS %d State=IDLE.\n",
							counter);
					if (board->cap_channel_top == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_TOP);
					} else if (board->cap_channel_bottom_lmb == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
					} else if (board->cap_channel_bottom_hb == counter) {
						this->report_data = IDLE;
						abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
					}
					pCurrentButton->state = IDLE;
					last_val = 0;
				}
				break;
			default: /* Shouldn't be here, device only allowed ACTIVE or IDLE */
				break;
			};
		}
		LOG_INFO("Leaving touchProcess()\n");
	}
}

static int abov_get_nirq_state(unsigned irq_gpio)
{
	if (irq_gpio) {
		return !gpio_get_value(irq_gpio);
	} else {
		LOG_INFO("abov irq_gpio is not set.");
		return -EINVAL;
	}
}

static struct _totalButtonInformation smtcButtonInformation = {
	.buttons = psmtcButtons,
	.buttonSize = ARRAY_SIZE(psmtcButtons),
};

/**
 *fn static void abov_reg_setup_init(struct i2c_client *client)
 *brief read reg val form dts
 *      reg_array_len for regs needed change num
 *      data_array_val's format <reg val ...>
 */
static void abov_reg_setup_init(struct i2c_client *client)
{
	u32 data_array_len = 0;
	u32 *data_array;
	int ret, i, j;
	struct device_node *np = client->dev.of_node;

	ret = of_property_read_u32(np, "reg_array_len", &data_array_len);
	if (ret < 0) {
		LOG_DBG("data_array_len read error");
		return;
	}
	data_array = kmalloc(data_array_len * 2 * sizeof(u32), GFP_KERNEL);
	ret = of_property_read_u32_array(np, "reg_array_val",
			data_array,
			data_array_len*2);
	if (ret < 0) {
		LOG_DBG("data_array_val read error");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(abov_i2c_reg_setup); i++) {
		for (j = 0; j < data_array_len*2; j += 2) {
			if (data_array[j] == abov_i2c_reg_setup[i].reg) {
				abov_i2c_reg_setup[i].val = data_array[j+1];
				LOG_DBG("read dtsi 0x%02x:0x%02x set reg\n",
					data_array[j], data_array[j+1]);
			}
		}
	}
	kfree(data_array);
}

static void abov_platform_data_of_init(struct i2c_client *client,
		pabov_platform_data_t pplatData)
{
	struct device_node *np = client->dev.of_node;
	u32 cap_channel_top, cap_channel_bottom_lmb, cap_channel_bottom_hb;
	int ret;
	struct device_node *device_node;

	device_node = of_find_compatible_node(NULL, NULL, "mediatek,sar_irq");
	if (device_node) {
		client->irq = irq_of_parse_and_map(device_node, 0);
		LOG_INFO("func = %s, irq = %d\n", __func__, client->irq);
	} else {
		LOG_ERR("func = %s, can't find compatible node.\n", __func__);
	}
	pplatData->irq_gpio = client->irq;
	LOG_INFO("2 func = %s, irq = %d\n", __func__, client->irq);
	//client->irq = of_get_gpio(np, 0);
	//pplatData->irq_gpio = client->irq;
	ret = of_property_read_u32(np, "cap,use_channel_top", &cap_channel_top);
	ret = of_property_read_u32(np, "cap,use_channel_bottom_lmb", &cap_channel_bottom_lmb);
	ret = of_property_read_u32(np, "cap,use_channel_bottom_hb", &cap_channel_bottom_hb);
	pplatData->cap_channel_top = (int)cap_channel_top;
	pplatData->cap_channel_bottom_lmb = (int)cap_channel_bottom_lmb;
	pplatData->cap_channel_bottom_hb = (int)cap_channel_bottom_hb;

	pplatData->get_is_nirq_low = abov_get_nirq_state;
	pplatData->init_platform_hw = NULL;
	/*  pointer to an exit function. Here in case needed in the future */
	/*
	 *.exit_platform_hw = abov_exit_ts,
	 */
	pplatData->exit_platform_hw = NULL;
	abov_reg_setup_init(client);
	pplatData->pi2c_reg = abov_i2c_reg_setup;
	pplatData->i2c_reg_num = ARRAY_SIZE(abov_i2c_reg_setup);

	pplatData->pbuttonInformation = &smtcButtonInformation;

	ret = of_property_read_string(np, "label", &pplatData->fw_name);
	if (ret < 0) {
		LOG_DBG("firmware name read error!\n");
		return;
	}
}

static ssize_t capsense_reset_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	return snprintf(buf, 8, "%d\n", programming_done);
}

static ssize_t capsense_reset_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = abov_sar_ptr;

	if (!count || (this == NULL))
		return -EINVAL;

	if (!strncmp(buf, "reset", 5) || !strncmp(buf, "1", 1))
		write_register(this, ABOV_SOFTRESET_REG, 0x10);

	this->report_data = IDLE;
	abovXX_sar_data_report(this,CHANNEL_TOP);
	abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
	abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);		

	return count;
}


static ssize_t capsense_enable_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	return snprintf(buf, 8, "%d\n", mEnabled);
}

static ssize_t capsense_enable_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = abov_sar_ptr;

	if (!count || (this == NULL))
		return -EINVAL;

	if (!strncmp(buf, "1", 1)) {
		LOG_DBG("enable cap sensor\n");
		initialize(this);

		this->report_data = IDLE;
		abovXX_sar_data_report(this,CHANNEL_TOP);
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);

	} else if (!strncmp(buf, "0", 1)) {
		LOG_DBG("disable cap sensor\n");

		write_register(this, ABOV_CTRL_MODE_REG, 0x02);

		this->report_data = IDLE;
		abovXX_sar_data_report(this,CHANNEL_TOP);
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);

	} else {
		LOG_DBG("unknown enable symbol\n");
	}

	return count;
}


static ssize_t reg_dump_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	if(!user_debug) {
		u8 reg_value = 0, i;
		pabovXX_t this = abov_sar_ptr;
		if(this->read_flag){
			this->read_flag = 0;
			for(i = 0 ; i < this->read_len ; i++){
				read_register(this,(this->read_reg+i),&reg_value);
				buf[i] = reg_value;
				LOG_INFO("%s : buf[%d] = 0x%x\n",__func__,i,buf[i]);
			}
			return i;
		}
		return -1;
	} else {
		u8 reg_value = 0, i;
		u16 offset_value = 0, diff_value = 0;
		pabovXX_t this = abov_sar_ptr;
		char *p = buf;

		if (this->read_flag) {
			this->read_flag = 0;
			read_register(this, this->read_reg, &reg_value);
			p += snprintf(p, PAGE_SIZE,  "(0x%02x)=0x%02x\n", this->read_reg, reg_value);
			return (p-buf);
		}

		for (i = 0; i < 0x40; i++) {
			read_register(this, i, &reg_value);
			p += snprintf(p, PAGE_SIZE, "(0x%02x)=0x%02x\n",
					i, reg_value);
		}

		for (i = 0x80; i < 0x8C; i++) {
			read_register(this, i, &reg_value);
			p += snprintf(p, PAGE_SIZE, "(0x%02x)=0x%02x\n",
					i, reg_value);
		}
		if ((this->get_ch0_flag) == 1) {
			/* diff */
			read_register(this, ABOV_CH0_DIFF_MSB_REG, &reg_value);
			diff_value = reg_value;
			diff_value <<= 8;
			read_register(this, ABOV_CH0_DIFF_LSB_REG, &reg_value);
			diff_value += reg_value;

			/* offset */
			read_register(this, ABOV_CH0_CAP_MSB_REG, &reg_value);
			offset_value = reg_value;
			offset_value <<= 8;
			read_register(this, ABOV_CH0_CAP_LSB_REG, &reg_value);
			offset_value += reg_value;
		} else if ((this->get_ch0_flag) == 2) {

			/* diff */
			read_register(this, ABOV_CH1_DIFF_MSB_REG, &reg_value);
			diff_value = reg_value;
			diff_value <<= 8;
			read_register(this, ABOV_CH1_DIFF_LSB_REG, &reg_value);
			diff_value += reg_value;

			/* offset */
			read_register(this, ABOV_CH1_CAP_MSB_REG, &reg_value);
			offset_value = reg_value;
			offset_value <<= 8;
			read_register(this, ABOV_CH1_CAP_LSB_REG, &reg_value);
			offset_value += reg_value;
		} else {

			/* diff */
			read_register(this, ABOV_CH2_DIFF_MSB_REG, &reg_value);
			diff_value = reg_value;
			diff_value <<= 8;
			read_register(this, ABOV_CH2_DIFF_LSB_REG, &reg_value);
			diff_value += reg_value;

			/* offset */
			read_register(this, ABOV_CH2_CAP_MSB_REG, &reg_value);
			offset_value = reg_value;
			offset_value <<= 8;
			read_register(this, ABOV_CH2_CAP_LSB_REG, &reg_value);
			offset_value += reg_value;
		}

		reg_value = gpio_get_value(this->board->irq_gpio);
		p += snprintf(p, PAGE_SIZE, "NIRQ=%d\n", reg_value);

		p += snprintf(p, PAGE_SIZE, "diff_value=0x%04x\n",
			diff_value);
		p += snprintf(p, PAGE_SIZE, "offset_value=0x%04x\n",
			offset_value);

		return (p-buf);
	}
}

static ssize_t reg_dump_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = abov_sar_ptr;
	if(!user_debug) {
		u8 regaddr,val;
		int i = 0;

		if( count != 3){
			LOG_ERR("%s :params error[ count == %d !=2]\n",__func__,count);
			return -1;
		}
		for(i = 0 ; i < count ; i++)
			LOG_INFO("%s : buf[%d] = 0x%x\n",__func__,i,buf[i]);
		if(buf[2] == 0){
			regaddr = buf[0];
			val= buf[1];
			write_register(this,regaddr,val);
	} else if(buf[2] == 1) {
			this->read_reg = buf[0];
			this->read_len = buf[1];
			this->read_flag = 1;
			LOG_ERR("-----------%d\n",this->read_len);
		}
	} else {
		unsigned int val, reg, opt;
		if (strcmp("select_ch0\n", buf) == 0) {
			this->get_ch0_flag = 1;
		} else if (strcmp("select_ch1\n", buf) == 0) {
			this->get_ch0_flag = 2;
		} else if (strcmp("select_ch2\n", buf) == 0) {
			this->get_ch0_flag = 0;
		} else if (strcmp("calibrate\n", buf) == 0) {
			write_register(this, ABOV_RECALI_REG, 0x01);
		} else if (sscanf(buf, "%x,%x,%x", &reg, &val, &opt) == 3) {
			LOG_INFO("%s, read reg = 0x%02x\n", __func__, *(u8 *)&reg);
			this->read_reg = *((u8 *)&reg);
			this->read_flag = 1;
		} else if (sscanf(buf, "%x,%x", &reg, &val) == 2) {
			LOG_INFO("%s,reg = 0x%02x, val = 0x%02x\n",
				__func__, *(u8 *)&reg, *(u8 *)&val);
			write_register(this, *((u8 *)&reg), *((u8 *)&val));
		}
	}
	return count;
}




static void ps_notify_callback_work(struct work_struct *work)
{
	pabovXX_t this = container_of(work, abovXX_t, ps_notify_work);
	int ret = 0;

	LOG_INFO("Usb insert,going to force calibrate\n");
	ret = write_register(this, ABOV_RECALI_REG, 0x01);
	if (ret < 0)
		LOG_ERR(" Usb insert,calibrate cap sensor failed\n");

	this->report_data = IDLE;
	abovXX_sar_data_report(this,CHANNEL_TOP);
	abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
	abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
}
static int ps_get_state(struct power_supply *psy, bool *present)
{
	union power_supply_propval pval = { 0 };
	int retval;

	retval = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE,/*POWER_SUPPLY_PROP_PRESENT,*/
			&pval);
	if (retval) {
		LOG_ERR("%s psy get property failed\n", psy->desc->name);
		return retval;
	}
	*present = (pval.intval) ? true : false;
	LOG_INFO("%s is %s\n", psy->desc->name,
			(*present) ? "present" : "not present");
	return 0;
}
static int ps_notify_callback(struct notifier_block *self,
		unsigned long event, void *p)
{
	pabovXX_t this = container_of(self, abovXX_t, ps_notif);
	struct power_supply *psy = p;
	bool present;
	int retval;

	if(!psy)
		return -ENODEV;
	if(!psy->desc)
		return -ENODEV;
	if ((event == PSY_EVENT_PROP_CHANGED)
			&& psy && psy->desc->get_property && psy->desc->name &&
			!strncmp(psy->desc->name, "charger", sizeof("charger"))) {
		LOG_INFO("ps notification: event = %lu\n", event);
		retval = ps_get_state(psy, &present);
		if (retval) {
			LOG_ERR("psy get property failed\n");
			return retval;
		}

		if (event == PSY_EVENT_PROP_CHANGED) {
			if (this->ps_is_present == present) {
				LOG_INFO("ps present state not change\n");
				return 0;
			}
		}
		this->ps_is_present = present;
		schedule_work(&this->ps_notify_work);
	}

	return 0;
}

static int _i2c_adapter_block_write(struct i2c_client *client, u8 *data, u8 len)
{
	u8 buffer[C_I2C_FIFO_SIZE];
	u8 left = len;
	u8 offset = 0;
	u8 retry = 0;

	struct i2c_msg msg = {
		.addr = client->addr & I2C_MASK_FLAG,
		.flags = 0,
		.buf = buffer,
	};

	if (data == NULL || len < 1) {
		LOG_ERR("Invalid : data is null or len=%d\n", len);
		return -EINVAL;
	}

	while (left > 0) {
		retry = 0;
		if (left >= C_I2C_FIFO_SIZE) {
			msg.buf = &data[offset];
			msg.len = C_I2C_FIFO_SIZE;
			left -= C_I2C_FIFO_SIZE;
			offset += C_I2C_FIFO_SIZE;
		} else {
			msg.buf = &data[offset];
			msg.len = left;
			left = 0;
		}

		while (i2c_transfer(client->adapter, &msg, 1) != 1) {
			retry++;
			if (retry > 10) {
			    LOG_ERR("OUT : fail - addr:%#x len:%d \n", client->addr, msg.len);
				return -EIO;
			}
		}
	}
	return 0;
}

static int i2c_adapter_block_write_nodatalog(struct i2c_client *client, u8 *data, u8 len)
{
	return _i2c_adapter_block_write(client, data, len);
}

static int abov_tk_check_busy(struct i2c_client *client)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(client, &val, sizeof(val));
		if (val & 0x01) {
			count++;
			if (count > 1000) {
				LOG_INFO("%s: val = 0x%x\r\n", __func__, val);
				return ret;
			}
		} else {
			break;
		}
	} while (1);

	return ret;
}

static int abov_tk_fw_write(struct i2c_client *client, unsigned char *addrH,
						unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char buf[40] = {0, };

	buf[0] = 0xAC;
	buf[1] = 0x7A;
	memcpy(&buf[2], addrH, 1);
	memcpy(&buf[3], addrL, 1);
	memcpy(&buf[4], val, 32);
	ret = i2c_adapter_block_write_nodatalog(client, buf, length);
	if (ret < 0) {
		LOG_ERR("Firmware write fail ...\n");
		return ret;
	}

	SLEEP(3);
	abov_tk_check_busy(client);

	return 0;
}

static int abov_tk_reset_for_bootmode(struct i2c_client *client)
{
	int ret, retry_count = 10;
	unsigned char buf[16] = {0, };

retry:
	buf[0] = 0xF0;
	buf[1] = 0xAA;
	ret = i2c_master_send(client, buf, 2);
	if (ret < 0) {
		LOG_INFO("write fail(retry:%d)\n", retry_count);
		if (retry_count-- > 0) {
			goto retry;
		}
		return -EIO;
	} else {
		LOG_INFO("success reset & boot mode\n");
		return 0;
	}
}

static int abov_tk_fw_mode_enter(struct i2c_client *client)
{
	int ret = 0;
	unsigned char buf[40] = {0, };

	buf[0] = 0xAC;
	buf[1] = 0x5B;
	ret = i2c_master_send(client, buf, 2);
	if (ret != 2) {
		LOG_ERR("SEND : fail - addr:%#x data:%#x %#x... ret:%d\n", client->addr, buf[0], buf[1], ret);
		return -EIO;
	}
	LOG_INFO("SEND : succ - addr:%#x data:%#x %#x... ret:%d\n", client->addr, buf[0], buf[1], ret);
	SLEEP(5);

	ret = i2c_master_recv(client, buf, 1);
	if (ret < 0) {
		LOG_ERR("Enter fw mode fail ... device id:%#x\n",buf[0]);
		return -EIO;
	}

	LOG_INFO("succ ... device id:%#x\n", buf[0]);

	return 0;
}

static int abov_tk_fw_mode_exit(struct i2c_client *client)
{
	int ret = 0;
	unsigned char buf[40] = {0, };

	buf[0] = 0xAC;
	buf[1] = 0xE1;
	ret = i2c_master_send(client, buf, 2);
	if (ret != 2) {
		LOG_ERR("SEND : fail - addr:%#x data:%#x %#x ... ret:%d\n", client->addr, buf[0], buf[1], ret);
		return -EIO;
	}
	LOG_INFO("SEND : succ - addr:%#x data:%#x %#x ... ret:%d\n", client->addr, buf[0], buf[1], ret);

	return 0;
}

static int abov_tk_flash_erase(struct i2c_client *client)
{
	int ret = 0;
	unsigned char buf[16] = {0, };

	buf[0] = 0xAC;
	buf[1] = 0x2E;

	ret = i2c_master_send(client, buf, 2);
	if (ret != 2) {
		LOG_ERR("SEND : fail - addr:%#x data:%#x %#x ... ret:%d\n", client->addr, buf[0], buf[1], ret);
		return -EIO;
	}

	LOG_INFO("SEND : succ - addr:%#x data:%#x %#x ... ret:%d\n", client->addr, buf[0], buf[1], ret);

	return 0;
}

static int abov_tk_i2c_read_checksum(struct i2c_client *client)
{
	unsigned char checksum[6] = {0, };
	unsigned char buf[16] = {0, };
	int ret;

	checksum_h = 0;
	checksum_l = 0;

	buf[0] = 0xAC;
	buf[1] = 0x9E;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = checksum_h_bin;
	buf[5] = checksum_l_bin;
	ret = i2c_master_send(client, buf, 6);

	if (ret != 6) {
		LOG_ERR("SEND : fail - addr:%#x len:%d ... ret:%d\n", client->addr, 6, ret);
		return -EIO;
	}
	SLEEP(5);

	buf[0] = 0x00;
	ret = i2c_master_send(client, buf, 1);
	if (ret != 1) {
		LOG_ERR("SEND : fail - addr:%#x data:%#x ... ret:%d\n", client->addr, buf[0], ret);
		return -EIO;
	}
	SLEEP(5);

	ret = i2c_master_recv(client, checksum, 6);
	if (ret < 0) {
		LOG_ERR("Verify checksum fail ... \n");
		return -EIO;
	}

	//LOG_INFO("Verify checksum  succ - addr:%#x checksum:%#x,%#x,%#x,%#,%#x,%# \n", client->addr, checksum[0], checksum[1],
	//	    checksum[2], checksum[3], checksum[4], checksum[5]);
	checksum_h = checksum[4];
	checksum_l = checksum[5];

	return 0;
}

static int _abov_fw_update(struct i2c_client *client, const u8 *image, u32 size)
{
	int ret, ii = 0;
	int count;
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char data[32] = {0, };

	LOG_INFO("%s: call in\r\n", __func__);

	if (abov_tk_reset_for_bootmode(client) < 0) {
		LOG_ERR("don't reset(enter boot mode)!");
		return -EIO;
	}

	SLEEP(45);

	for (ii = 0; ii < 10; ii++) {
		if (abov_tk_fw_mode_enter(client) < 0) {
			LOG_ERR("don't enter the download mode! %d", ii);
			SLEEP(40);
			continue;
		}
		break;
	}

	if (10 <= ii) {
		return -EAGAIN;
	}

	if (abov_tk_flash_erase(client) < 0) {
		LOG_ERR("don't erase flash data!");
		return -EIO;
	}

	SLEEP(1400);

	address = 0x800;
	count = size / 32;

	for (ii = 0; ii < count; ii++) {
		/* first 32byte is header */
		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		memcpy(data, &image[ii * 32], 32);
		ret = abov_tk_fw_write(client, &addrH, &addrL, data);
		if (ret < 0) {
			LOG_INFO("fw_write.. ii = 0x%x err\r\n", ii);
			return ret;
		}

		address += 0x20;
		memset(data, 0, 32);
	}

	ret = abov_tk_i2c_read_checksum(client);
	ret = abov_tk_fw_mode_exit(client);
	if ((checksum_h == checksum_h_bin) && (checksum_l == checksum_l_bin)) {
		LOG_INFO("update successful. checksum_h:%x=checksum_h_bin:%x && checksum_l:%x=checksum_l_bin:%x\n",
			checksum_h, checksum_h_bin, checksum_l, checksum_l_bin);
	} else {
		LOG_INFO("update error. checksum_h:%x!=checksum_h_bin:%x && checksum_l:%x!=checksum_l_bin:%x\n",
			checksum_h, checksum_h_bin, checksum_l, checksum_l_bin);
		ret = -1;
	}
	SLEEP(100);

	return ret;
}

static int abov_fw_update(bool force)
{
	int update_loop;
	pabovXX_t this = abov_sar_ptr;
	struct i2c_client *client = this->bus;
	int rc;
	bool fw_upgrade = false;
	u8 fw_version = 0, fw_file_version = 0;
	u8 fw_modelno = 0, fw_file_modeno = 0;
	const struct firmware *fw = NULL;
	char fw_name[32] = {0};

	strlcpy(fw_name, this->board->fw_name, NAME_MAX);
	strlcat(fw_name, ".BIN", NAME_MAX);
	rc = request_firmware(&fw, fw_name, this->pdev);
	if (rc < 0) {
		LOG_INFO("Request firmware failed - %s (%d)\n",
				this->board->fw_name, rc);
		return rc;
	}
	if (force == false) {
	    read_register(this, ABOV_VERSION_REG, &fw_version);
	    read_register(this, ABOV_MODELNO_REG, &fw_modelno);
	}

	fw_file_modeno = fw->data[1];
	fw_file_version = fw->data[5];
	checksum_h_bin = fw->data[8];
	checksum_l_bin = fw->data[9];

	if ((force) || (fw_version != fw_file_version) || (fw_modelno != fw_file_modeno))
		fw_upgrade = true;
	else {
		LOG_INFO("Exiting fw upgrade...\n");
		fw_upgrade = false;
		fw_dl_status = 0;
		rc = -EIO;
		goto rel_fw;
	}

	if (fw_upgrade) {
		fw_dl_status = 2;
		for (update_loop = 0; update_loop < 10; update_loop++) {
			rc = _abov_fw_update(client, &fw->data[32], fw->size-32);
			if (rc < 0)
				LOG_INFO("retry : %d times!\n", update_loop);
			else {
				initialize(this);
				break;
			}
			SLEEP(400);
		}
		if (update_loop >= 10) {
			fw_dl_status = 1;
			rc = -EIO;
		}
		fw_dl_status = 0;
	}
rel_fw:
	release_firmware(fw);
	return rc;
}

static ssize_t capsense_fw_ver_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	u8 fw_version = 0;
	pabovXX_t this = abov_sar_ptr;

	read_register(this, ABOV_VERSION_REG, &fw_version);

	return snprintf(buf, 16, "ABOV FW Version:0x%02x\n", fw_version);
}

static ssize_t capsense_update_fw_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = abov_sar_ptr;
	unsigned long val;
	int rc;

	if (count > 2)
		return -EINVAL;

	rc = kstrtoul(buf, 10, &val);
	if (rc != 0)
		return rc;

	this->irq_disabled = 1;
	disable_irq(this->irq);

	mutex_lock(&this->mutex);
	if (!this->loading_fw  && val) {
		this->loading_fw = true;
		abov_fw_update(false);
		this->loading_fw = false;
	}
	mutex_unlock(&this->mutex);

	enable_irq(this->irq);
	this->irq_disabled = 0;

	return count;
}

static ssize_t capsense_force_update_fw_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	pabovXX_t this = abov_sar_ptr;
	unsigned long val;
	int rc;

	if (count > 2)
		return -EINVAL;

	rc = kstrtoul(buf, 10, &val);
	if (rc != 0)
		return rc;

	this->irq_disabled = 1;
	disable_irq(this->irq);

	mutex_lock(&this->mutex);
	if (!this->loading_fw  && val) {
		this->loading_fw = true;
		abov_fw_update(true);
		this->loading_fw = false;
	}
	mutex_unlock(&this->mutex);

	enable_irq(this->irq);
	this->irq_disabled = 0;

	return count;
}

static ssize_t capsense_user_debug_status_store(struct class *class,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	if (!count )
		return -EINVAL;

	if (!strncmp(buf, "1", 1)) {
		LOG_DBG("enable cap user debug\n");
		user_debug = true;
	} else if (!strncmp(buf, "0", 1)) {
		LOG_DBG("disable cap user debug\n");
		user_debug = false;
	} else {
		LOG_DBG("unknown enable symbol\n");
	}

	return count;
}

static ssize_t capsense_user_debug_status_show(struct class *class,
		struct class_attribute *attr,
		char *buf)
{
	return snprintf(buf,8,"%d\n",user_debug);
}

static void capsense_update_work(struct work_struct *work)
{
	pabovXX_t this = container_of(work, abovXX_t, fw_update_work);

	LOG_INFO("%s: start update firmware\n", __func__);
	mutex_lock(&this->mutex);
	this->loading_fw = true;
	abov_fw_update(false);
	this->loading_fw = false;
	mutex_unlock(&this->mutex);
	LOG_INFO("%s: update firmware end\n", __func__);
}
static void capsense_fore_update_work(struct work_struct *work)
{
	pabovXX_t this = container_of(work, abovXX_t, fw_update_work);

	LOG_INFO("%s: start force update firmware\n", __func__);
	mutex_lock(&this->mutex);
	this->loading_fw = true;
	abov_fw_update(true);
	this->loading_fw = false;
	mutex_unlock(&this->mutex);
	LOG_INFO("%s: force update firmware end\n", __func__);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)

static struct class_attribute class_attr_reset =
	__ATTR(reset, 0660, capsense_reset_show, capsense_reset_store);

static struct class_attribute class_attr_enable =
	__ATTR(enable, 0660, capsense_enable_show, capsense_enable_store);

static struct class_attribute class_attr_reg =
	__ATTR(reg, 0660, reg_dump_show, reg_dump_store);

static struct class_attribute class_attr_update_fw =
	__ATTR(update_fw, 0660, capsense_fw_ver_show, capsense_update_fw_store);

static struct class_attribute class_attr_force_update_fw =
	__ATTR(force_update_fw, 0660, capsense_fw_ver_show, capsense_force_update_fw_store);

static struct class_attribute class_attr_user_debug_status =
	__ATTR(user_debug_status, 0660, capsense_user_debug_status_show, capsense_user_debug_status_store);

static struct attribute *capsense_class_attrs[] = {
	&class_attr_reset.attr,
	&class_attr_enable.attr,
	&class_attr_reg.attr,
	&class_attr_update_fw.attr,
	&class_attr_force_update_fw.attr,
	&class_attr_user_debug_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(capsense_class);
#else
static struct class_attribute capsense_class_attributes[] = {
	__ATTR(reset, 0660, capsense_reset_show, capsense_reset_store),
	__ATTR(enable, 0660, capsense_enable_show, capsense_enable_store),
	__ATTR(reg, 0660, reg_dump_show, reg_dump_store),
	__ATTR(update_fw, 0660, capsense_fw_ver_show, capsense_update_fw_store),
	__ATTR(force_update_fw, 0660, capsense_fw_ver_show, capsense_force_update_fw_store),
	__ATTR(user_debug_status, 0660, capsense_user_debug_status_show, capsense_user_debug_status_store),
	__ATTR_NULL,
};
#endif

static struct class capsense_class = {
	.name			= "capsense",
	.owner			= THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	.class_groups	= capsense_class_groups,
#else
	.class_attrs		= capsense_class_attributes,
#endif	
};

/**
 * fn static int abov_probe(struct i2c_client *client, const struct i2c_device_id *id)
 * brief Probe function
 * param client pointer to i2c_client
 * param id pointer to i2c_device_id
 * return Whether probe was successful
 */
static int abov_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	pabovXX_t this = 0;
	pabov_t pDevice = 0;
	pabov_platform_data_t pplatData = 0;
	int ret;
	bool isForceUpdate = false;

	struct power_supply *psy = NULL;

	LOG_INFO("abov_probe start()\n");
	pplatData = kzalloc(sizeof(struct abov_platform_data), GFP_KERNEL);

	if (!pplatData) {
		LOG_ERR("platform data is required!\n");
		return -EINVAL;
	}
	printk("func = %s, line = %d\n", __func__, __LINE__);
	mdelay(100);


	/* detect if abov exist or not */
	ret = abov_detect(client);
	if (ret == 0) {
		ret = -ENODEV;
		goto err_this_device;
	} else if (ret == 2) {
		LOG_INFO("abov enter boot mode\n");
		isForceUpdate = true;
	}

	printk("func = %s, line = %d\n", __func__, __LINE__);
	abov_platform_data_of_init(client, pplatData);
	client->dev.platform_data = pplatData;

	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_READ_WORD_DATA)) {
		ret = -EIO;
		goto err_this_device;
	}

	printk("func = %s, line = %d\n", __func__, __LINE__);
	this = kzalloc(sizeof(abovXX_t), GFP_KERNEL); /* create memory for main struct */
	LOG_INFO("\t Initialized Main Memory: 0x%p\n", this);

	if (this) {
		printk("func = %s, line = %d\n", __func__, __LINE__);
		/* In case we need to reinitialize data
		 * (e.q. if suspend reset device) */
		this->init = initialize;
		/* shortcut to read status of interrupt */
		this->refreshStatus = read_regStat;
		/* pointer to function from platform data to get pendown
		 * (1->NIRQ=0, 0->NIRQ=1) */
		this->get_nirq_low = pplatData->get_is_nirq_low;
		/* save irq in case we need to reference it */
		printk("func = %s, line = %d, client->irq:%d, this->irq :%d \n", __func__, __LINE__, client->irq, this->irq  );
		// this->irq = gpio_to_irq(client->irq);
        this->irq = client->irq;
		printk("func = %s, line = %d, client->irq:%d, this->irq :%d \n", __func__, __LINE__, client->irq, this->irq  );
		/* do we need to create an irq timer after interrupt ? */
		this->useIrqTimer = 0;
		this->board = pplatData;
		/* Setup function to call on corresponding reg irq source bit */
		if (MAX_NUM_STATUS_BITS >= 8) {
			this->statusFunc[0] = 0; /* TXEN_STAT */
			this->statusFunc[1] = 0; /* UNUSED */
			this->statusFunc[2] = 0; /* UNUSED */
			this->statusFunc[3] = 0;/*read_rawData;  CONV_STAT */
			this->statusFunc[4] = 0; /* COMP_STAT */
			this->statusFunc[5] = touchProcess; /* RELEASE_STAT */
			this->statusFunc[6] = touchProcess; /* TOUCH_STAT  */
			this->statusFunc[7] = 0; /* RESET_STAT */
		}

		/* setup i2c communication */
		this->bus = client;
		i2c_set_clientdata(client, this);

		/* record device struct */
		this->pdev = &client->dev;

		/* create memory for device specific struct */
		this->pDevice = pDevice = kzalloc(sizeof(abov_t), GFP_KERNEL);
		LOG_INFO("\t Initialized Device Specific Memory: 0x%p\n",
				pDevice);
		abov_sar_ptr = this;
		if (pDevice) {
			/* for accessing items in user data (e.g. calibrate) */
			ret = sysfs_create_group(&client->dev.kobj, &abov_attr_group);


			/* Check if we hava a platform initialization function to call*/
			if (pplatData->init_platform_hw)
				pplatData->init_platform_hw();

			/* Add Pointer to main platform data struct */
			pDevice->hw = pplatData;

			/* Initialize the button information initialized with keycodes */
			pDevice->pbuttonInformation = pplatData->pbuttonInformation;
		}

		ret = class_register(&capsense_class);
		if (ret < 0) {
			LOG_ERR("Create fsys class failed (%d)\n", ret);
			goto err_class_creat;
		}


		abovXX_sar_init(this);
	    this->report_data = IDLE;

		write_register(this, ABOV_CTRL_MODE_REG, 0x02);
		mEnabled = 0;

		INIT_WORK(&this->ps_notify_work, ps_notify_callback_work);
		this->ps_notif.notifier_call = ps_notify_callback;
		ret = power_supply_reg_notifier(&this->ps_notif);
		if (ret) {
			LOG_ERR(
				"Unable to register ps_notifier: %d\n", ret);
			goto free_ps_notifier;
		}

		psy = power_supply_get_by_name("charger");
		if (psy) {
			ret = ps_get_state(psy, &this->ps_is_present);
			if (ret) {
				LOG_ERR(
					"psy get property failed rc=%d\n",
					ret);
				goto free_ps_notifier;
			}
		}

		this->loading_fw = false;
		fw_dl_status = 1;
		if (isForceUpdate == true) {
		    INIT_WORK(&this->fw_update_work, capsense_fore_update_work);
		} else {
			INIT_WORK(&this->fw_update_work, capsense_update_work);
		}
		schedule_work(&this->fw_update_work);

	printk("func = %s, line = %d\n", __func__, __LINE__);
        LOG_INFO("abov_probe end()\n");
		return  0;
	}
	ret =  -ENOMEM;
	goto err_this_device;

free_ps_notifier:
	LOG_ERR("%s free ps notifier:.\n", __func__);
	power_supply_unreg_notifier(&this->ps_notif);

err_class_creat:
	LOG_ERR("%s unregister capsense class.\n", __func__);
	class_unregister(&capsense_class);

err_this_device:
	LOG_ERR("%s device this defer.\n", __func__);
#if 0
	regulator_disable(pplatData->cap_svdd);
	regulator_put(pplatData->cap_svdd);

err_svdd_error:
	LOG_DBG("%s svdd defer.\n", __func__);
	regulator_disable(pplatData->cap_vdd);
	regulator_put(pplatData->cap_vdd);


err_vdd_defer:
	LOG_ERR("%s free pplatData.\n", __func__);
#endif

	kfree(pplatData);
	return ret;
}

/**
 * fn static int abov_remove(struct i2c_client *client)
 * brief Called when device is to be removed
 * param client Pointer to i2c_client struct
 * return Value from abovXX_sar_remove()
 */
static int abov_remove(struct i2c_client *client)
{
	pabov_platform_data_t pplatData = 0;
	pabov_t pDevice = 0;
	pabovXX_t this = i2c_get_clientdata(client);

	pDevice = this->pDevice;
	if (this && pDevice) {
		sysfs_remove_group(&client->dev.kobj, &abov_attr_group);
		pplatData = client->dev.platform_data;
		if (pplatData && pplatData->exit_platform_hw)
			pplatData->exit_platform_hw();
		kfree(this->pDevice);
	}
	return abovXX_sar_remove(this);
}


#if defined(USE_KERNEL_SUSPEND)

/*====================================================*/
/***** Kernel Suspend *****/
static int abov_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
	pabovXX_t this = i2c_get_clientdata(client);
	abovXX_suspend(this);
	return 0;
}
/***** Kernel Resume *****/
static int abov_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	pabovXX_t this = i2c_get_clientdata(client);
	abovXX_resume(this);
	return 0;
}
/*====================================================*/
#endif

#ifdef CONFIG_OF
static const struct of_device_id abov_match_tbl[] = {
	{ .compatible = "mediatek,sar" },
	{ },
};
MODULE_DEVICE_TABLE(of, abov_match_tbl);
#endif

static struct i2c_device_id abov_idtable[] = {
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, abov_idtable);


static SIMPLE_DEV_PM_OPS(abov_pm_ops, abov_suspend, abov_resume);

static struct i2c_driver abov_driver = {
	.driver = {
		.owner  = THIS_MODULE,
		.of_match_table = abov_match_tbl,
		.name   = DRIVER_NAME,
		.pm = &abov_pm_ops,
	},
	.id_table = abov_idtable,
	.probe	  = abov_probe,
	.remove	  = abov_remove,
};
/*---------------------add to situation----------------*/
static int sar_top_open_report_data(int enable)
{
	pabovXX_t this = abov_sar_ptr;

	if (this == NULL)
		return -EINVAL;

	if (enable == 1) {
		LOG_DBG("enable cap sensor mEnabled=%d\n",mEnabled);
		if (mEnabled == 0)
		   initialize(this);

		this->report_data = 0;
		abovXX_sar_data_report(this,CHANNEL_TOP);
		mEnabled = mEnabled | 0x01;
	} else if (enable == 0) {
		LOG_DBG("disable cap sensor mEnabled=%d\n",mEnabled);
		mEnabled = mEnabled & 0xFE;
		if ((mEnabled & 0x07) == 0)
		   write_register(this, ABOV_CTRL_MODE_REG, 0x02);
	}

	return 0;
}

static int sar_bottom_lmb_open_report_data(int enable)
{
	pabovXX_t this = abov_sar_ptr;

	if (this == NULL)
		return -EINVAL;

	if (enable == 1) {
		LOG_DBG("enable cap sensor mEnabled=%d\n",mEnabled);
		if (mEnabled == 0)
		   initialize(this);
		this->report_data = 0;
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_LMB);
		mEnabled = mEnabled | 0x02;
	} else if (enable == 0) {
		LOG_DBG("disable cap sensor mEnabled=%d\n",mEnabled);

		mEnabled = mEnabled & 0xFD;
		if ((mEnabled & 0x07) == 0)
		   write_register(this, ABOV_CTRL_MODE_REG, 0x02);
	}
	return 0;
}

static int sar_bottom_nb_open_report_data(int enable)
{
	pabovXX_t this = abov_sar_ptr;

	if (this == NULL)
		return -EINVAL;

	if (enable == 1) {
		LOG_DBG("enable cap sensor mEnabled=%d\n",mEnabled);
		if (mEnabled == 0)
		   initialize(this);
		this->report_data = 0;
		abovXX_sar_data_report(this,CHANNEL_BOTTOM_NB);
		mEnabled = mEnabled | 0x04;
	} else if (enable == 0) {
	    LOG_DBG("disable cap sensor mEnabled=%d\n",mEnabled);
		mEnabled = mEnabled & 0XFB;
		if ((mEnabled & 0x07) == 0)
		   write_register(this, ABOV_CTRL_MODE_REG, 0x02);
	}
	return 0;
}

static int sar_top_batch(int flag,
	int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
	return 0;
}
static int sar_bottom_lmb_batch(int flag,
	int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
	return 0;
}
static int sar_bottom_nb_batch(int flag,
	int64_t samplingPeriodNs, int64_t maxBatchReportLatencyNs)
{
	return 0;
}
static int sar_top_flush(void)
{
	return moto_sar_flush_report(ID_SAR_TOP);
}
static int sar_bottom_lmb_flush(void)
{
	return moto_sar_flush_report(ID_SAR_BOTTOM_LEFT);
}
static int sar_bottom_nb_flush(void)
{
	return moto_sar_flush_report(ID_SAR_BOTTOM_RIGHT);
}

static int sar_top_get_data(int *probability, int *status)
{
	return 0;
}
static int sar_bottom_lmb_get_data(int *probability, int *status)
{
	return 0;
}

static int sar_bottom_nb_get_data(int *probability, int *status)
{
	return 0;
}

static int abov_sar_top_local_init(void)
{
	struct situation_control_path ctl = {0};
	struct situation_data_path data = {0};
	int err = 0;

	pr_debug("%s\n", __func__);

    if (false == i2c_added) {
	   i2c_add_driver(&abov_driver);
       i2c_added = true;
    }
	ctl.open_report_data = sar_top_open_report_data;
	ctl.batch = sar_top_batch;
	ctl.flush = sar_top_flush;
	ctl.is_support_wake_lock = true;
	ctl.is_support_batch = false;
	err = situation_register_control_path(&ctl, ID_SAR_TOP);
	if (err) {
		pr_err("register sar top control path err\n");
		goto exit;
	}
	data.get_data = sar_top_get_data;
	err = situation_register_data_path(&data, ID_SAR_TOP);
	if (err) {
		pr_err("register sar top data path err\n");
		goto exit;
	}

	return 0;
exit:
	return -1;
}

static int abov_sar_bottom_lmb_local_init(void)
{
	struct situation_control_path ctl = {0};
	struct situation_data_path data = {0};
	int err = 0;

	pr_debug("%s\n", __func__);
    if (false == i2c_added) {
	   i2c_add_driver(&abov_driver);
       i2c_added = true;
    }

	ctl.open_report_data = sar_bottom_lmb_open_report_data;
	ctl.batch = sar_bottom_lmb_batch;
	ctl.flush = sar_bottom_lmb_flush;
	ctl.is_support_wake_lock = true;
	ctl.is_support_batch = false;
	err = situation_register_control_path(&ctl, ID_SAR_BOTTOM_LEFT);
	if (err) {
		pr_err("register sar bottom lmb control path err\n");
		goto exit;
	}
	data.get_data = sar_bottom_lmb_get_data;
	err = situation_register_data_path(&data, ID_SAR_BOTTOM_LEFT);
	if (err) {
		pr_err("register sar bottom lmb data path err\n");
		goto exit;
	}

	return 0;
exit:
	return -1;
}

static int abov_sar_bottom_nb_local_init(void)
{
	struct situation_control_path ctl = {0};
	struct situation_data_path data = {0};
	int err = 0;

	pr_debug("%s\n", __func__);

    if (false == i2c_added) {
	   i2c_add_driver(&abov_driver);
       i2c_added = true;
    }

	ctl.open_report_data = sar_bottom_nb_open_report_data;
	ctl.batch = sar_bottom_nb_batch;
	ctl.flush = sar_bottom_nb_flush;
	ctl.is_support_wake_lock = true;
	ctl.is_support_batch = false;
	err = situation_register_control_path(&ctl, ID_SAR_BOTTOM_RIGHT);
	if (err) {
		pr_err("register sar bottom nb control path err\n");
		goto exit;
	}
	data.get_data = sar_bottom_nb_get_data;
	err = situation_register_data_path(&data, ID_SAR_BOTTOM_RIGHT);
	if (err) {
		pr_err("register sar bottom nb data path err\n");
		goto exit;
	}

	return 0;
exit:
	return -1;
}
static int abov_sar_top_local_uninit(void)
{
	return 0;
}
static int abov_sar_bottom_lmb_local_uninit(void)
{
	return 0;
}
static int abov_sar_bottom_nb_local_uninit(void)
{
	return 0;
}
static struct situation_init_info abov_sar_top_init_info = {
	.name = "abov_sar_top",
	.init = abov_sar_top_local_init,
	.uninit = abov_sar_top_local_uninit,
};
static struct situation_init_info abov_sar_bottom_lmb_init_info = {
	.name = "abov_sar_bottom_lmb",
	.init = abov_sar_bottom_lmb_local_init,
	.uninit = abov_sar_bottom_lmb_local_uninit,
};
static struct situation_init_info abov_sar_bottom_nb_init_info = {
	.name = "abov_sar_bottom_nb",
	.init = abov_sar_bottom_nb_local_init,
	.uninit = abov_sar_bottom_nb_local_uninit,
};

/*---------------------add to situation----------------*/
static int __init abov_init(void)
{
	printk("func = %s, line = %d\n", __func__, __LINE__);
	situation_driver_add(&abov_sar_top_init_info, ID_SAR_TOP);
	situation_driver_add(&abov_sar_bottom_lmb_init_info, ID_SAR_BOTTOM_LEFT);
	situation_driver_add(&abov_sar_bottom_nb_init_info, ID_SAR_BOTTOM_RIGHT);
	return 0;
}
static void __exit abov_exit(void)
{
	i2c_del_driver(&abov_driver);
}

module_init(abov_init);
module_exit(abov_exit);

MODULE_AUTHOR("ABOV Corp.");
MODULE_DESCRIPTION("ABOV Capacitive Touch Controller Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

#ifdef USE_THREADED_IRQ
static void abovXX_process_interrupt(pabovXX_t this, u8 nirqlow)
{
	int status = 0;

	if (!this) {
		pr_err("abovXX_worker_func, NULL abovXX_t\n");
		return;
	}
	/* since we are not in an interrupt don't need to disable irq. */
	status = this->refreshStatus(this);
	LOG_INFO("Worker - Refresh Status %d\n", status);
	this->statusFunc[6](this);
	if (unlikely(this->useIrqTimer && nirqlow)) {
		/* In case we need to send a timer for example on a touchscreen
		 * checking penup, perform this here
		 */
		cancel_delayed_work(&this->dworker);
		schedule_delayed_work(&this->dworker, msecs_to_jiffies(this->irqTimeout));
		LOG_INFO("Schedule Irq timer");
	}
}


static void abovXX_worker_func(struct work_struct *work)
{
	pabovXX_t this = 0;

	if (work) {
		this = container_of(work, abovXX_t, dworker.work);
		if (!this) {
			LOG_DBG("abovXX_worker_func, NULL abovXX_t\n");
			return;
		}
		if ((!this->get_nirq_low) || (!this->get_nirq_low(this->board->irq_gpio))) {
			/* only run if nirq is high */
			abovXX_process_interrupt(this, 0);
		}
	} else {
		LOG_INFO("abovXX_worker_func, NULL work_struct\n");
	}
}
static irqreturn_t abovXX_interrupt_thread(int irq, void *data)
{
	pabovXX_t this = 0;
	this = data;

	if (mEnabled == 0) {
		LOG_INFO("abovXX_irq sensor not enable\n");
		return IRQ_HANDLED;
	}

	mutex_lock(&this->mutex);
	LOG_INFO("abovXX_irq\n");
	if ((!this->get_nirq_low) || this->get_nirq_low(this->board->irq_gpio))
		abovXX_process_interrupt(this, 1);
	else
		LOG_DBG("abovXX_irq - nirq read high\n");
	mutex_unlock(&this->mutex);
	return IRQ_HANDLED;
}
#else
static void abovXX_schedule_work(pabovXX_t this, unsigned long delay)
{
	unsigned long flags;

	if (this) {
		LOG_INFO("abovXX_schedule_work()\n");
		spin_lock_irqsave(&this->lock, flags);
		/* Stop any pending penup queues */
		cancel_delayed_work(&this->dworker);
		/*
		 *after waiting for a delay, this put the job in the kernel-global
		 workqueue. so no need to create new thread in work queue.
		 */
		schedule_delayed_work(&this->dworker, delay);
		spin_unlock_irqrestore(&this->lock, flags);
	} else
		LOG_DBG("abovXX_schedule_work, NULL pabovXX_t\n");
}

static irqreturn_t abovXX_irq(int irq, void *pvoid)
{
	pabovXX_t this = 0;
	if (pvoid) {
		this = (pabovXX_t)pvoid;
		LOG_INFO("abovXX_irq\n");
		if ((!this->get_nirq_low) || this->get_nirq_low(this->board->irq_gpio)) {
			LOG_INFO("abovXX_irq - Schedule Work\n");
			abovXX_schedule_work(this, 0);
		} else
			LOG_INFO("abovXX_irq - nirq read high\n");
	} else
		LOG_INFO("abovXX_irq, NULL pvoid\n");
	return IRQ_HANDLED;
}

static void abovXX_worker_func(struct work_struct *work)
{
	pabovXX_t this = 0;
	int status = 0;
	u8 nirqLow = 0;

	if (work) {
		this = container_of(work, abovXX_t, dworker.work);

		if (!this) {
			LOG_INFO("abovXX_worker_func, NULL abovXX_t\n");
			return;
		}
		if (unlikely(this->useIrqTimer)) {
			if ((!this->get_nirq_low) || this->get_nirq_low(this->board->irq_gpio))
				nirqLow = 1;
		}
		/* since we are not in an interrupt don't need to disable irq. */
		status = this->refreshStatus(this);
		LOG_INFO("Worker - Refresh Status %d\n", status);
		this->statusFunc[6](this);
		if (unlikely(this->useIrqTimer && nirqLow)) {
			/* Early models and if RATE=0 for newer models require a penup timer */
			/* Queue up the function again for checking on penup */
			abovXX_schedule_work(this, msecs_to_jiffies(this->irqTimeout));
		}
	} else {
		LOG_INFO("abovXX_worker_func, NULL work_struct\n");
	}
}
#endif

void abovXX_suspend(pabovXX_t this)
{
	if (this) {
		LOG_INFO("ABOV suspend: enter stop mode!\n");
		disable_irq(this->irq);
		if (mEnabled)
			write_register(this, ABOV_CTRL_MODE_REG, 0x02);
	}
}
void abovXX_resume(pabovXX_t this)
{
	if (this) {
		LOG_INFO("ABOV resume: enter ative mode!\n");
		if (mEnabled)
			write_register(this, ABOV_CTRL_MODE_REG, 0x00);
		enable_irq(this->irq);
	}
}

int abovXX_sar_init(pabovXX_t this)
{
	int err = 0;

	if (this && this->pDevice) {
#ifdef USE_THREADED_IRQ

		/* initialize worker function */
		INIT_DELAYED_WORK(&this->dworker, abovXX_worker_func);


		/* initialize mutex */
		mutex_init(&this->mutex);
		/* initailize interrupt reporting */
		this->irq_disabled = 0;

		err = request_threaded_irq(this->irq, NULL, abovXX_interrupt_thread,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT, this->pdev->driver->name,
				this);
#else
		/* initialize spin lock */
		spin_lock_init(&this->lock);

		/* initialize worker function */
		INIT_DELAYED_WORK(&this->dworker, abovXX_worker_func);

		/* initailize interrupt reporting */
		this->irq_disabled = 0;
		err = request_irq(this->irq, abovXX_irq, IRQF_TRIGGER_FALLING,
				this->pdev->driver->name, this);
#endif
		if (err) {
			LOG_DBG("irq %d busy?\n", this->irq);
			return err;
		}
#ifdef USE_THREADED_IRQ
		LOG_DBG("registered with threaded irq (%d)\n", this->irq);
#else
		LOG_DBG("registered with irq (%d)\n", this->irq);
#endif
		/* call init function pointer (this should initialize all registers */
		if (this->init)
			return this->init(this);
		LOG_DBG("No init function!!!!\n");
	}
	return -ENOMEM;
}

int abovXX_sar_remove(pabovXX_t this)
{
	if (this) {
		cancel_delayed_work_sync(&this->dworker); /* Cancel the Worker Func */
		free_irq(this->irq, this);
		kfree(this);
		return 0;
	}
	return -ENOMEM;
}

int abovXX_sar_data_report(pabovXX_t this,int32_t channel)
{
	int32_t value = 0;

	value = this->report_data;

	switch (channel) {
		case CHANNEL_TOP:
		  moto_sar_data_report(value,ID_SAR_TOP);
		  break;
		case CHANNEL_BOTTOM_LMB:
		  moto_sar_data_report(value,ID_SAR_BOTTOM_RIGHT);
		  break;
		case CHANNEL_BOTTOM_NB:
		  moto_sar_data_report(value,ID_SAR_BOTTOM_LEFT);
		  break;
		default:
		  pr_err("abovXX_sar_data_report err channel=%d\n",channel);
		  break;
	}
	return 0;
}


