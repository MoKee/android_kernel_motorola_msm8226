/*
 * Copyright (C) 2010-2013 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#include <linux/stm401.h>


irqreturn_t stm401_wake_isr(int irq, void *dev)
{
	struct stm401_data *ps_stm401 = dev;

	if (stm401_irq_disable) {
		disable_irq_wake(ps_stm401->irq);
		return IRQ_HANDLED;
	}

	wake_lock_timeout(&ps_stm401->wakelock, HZ);

	queue_work(ps_stm401->irq_work_queue, &ps_stm401->irq_wake_work);
	return IRQ_HANDLED;
}

void stm401_irq_wake_work_func(struct work_struct *work)
{
	int err;
	unsigned short irq_status, irq2_status;
	signed short x, y, z, q;

	struct stm401_data *ps_stm401 = container_of(work,
			struct stm401_data, irq_wake_work);

	dev_dbg(&ps_stm401->client->dev, "stm401_irq_wake_work_func\n");
	mutex_lock(&ps_stm401->lock);

	/* read interrupt mask register */
	stm401_cmdbuff[0] = WAKESENSOR_STATUS;
	err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
	if (err < 0) {
		dev_err(&ps_stm401->client->dev, "Reading from stm401 failed\n");
		goto EXIT;
	}
	irq_status = (stm401_readbuff[1] << 8) | stm401_readbuff[0];

	/* read algorithm interrupt status register */
	stm401_cmdbuff[0] = ALGO_INT_STATUS;
	err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
	if (err < 0) {
		dev_err(&ps_stm401->client->dev, "Reading from stm401 failed\n");
		goto EXIT;
	}
	irq2_status = (stm401_readbuff[1] << 8) | stm401_readbuff[0];

	/* First, check for error messages */
	if (irq_status & M_LOG_MSG) {
		stm401_cmdbuff[0] = ERROR_STATUS;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff,
			1, ESR_SIZE);
		if (err >= 0) {
			memcpy(stat_string, stm401_readbuff, ESR_SIZE);
			stat_string[ESR_SIZE] = 0;
			dev_err(&ps_stm401->client->dev,
				"STM401 Error: %s\n", stat_string);
		} else
			dev_err(&ps_stm401->client->dev,
				"Failed to read error message %d\n", err);
	}

	/* Second, check for a reset request */
	if (irq_status & M_HUB_RESET) {
		if (strnstr(stat_string, "modality", ESR_SIZE))
			x = 0x01;
		else if (strnstr(stat_string, "Algo", ESR_SIZE))
			x = 0x02;
		else if (strnstr(stat_string, "Watchdog", ESR_SIZE))
			x = 0x03;
		else
			x = 0x04;

		stm401_as_data_buffer_write(ps_stm401, DT_RESET, x, 0, 0, 0);

		stm401_reset_and_init();
		dev_err(&ps_stm401->client->dev, "STM401 requested a reset\n");
		goto EXIT;
	}

	/* Check all other status bits */
	if (irq_status & M_DOCK) {
		stm401_cmdbuff[0] = DOCK_DATA;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading Dock state failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401, DT_DOCK, x, 0, 0, 0);
		if (ps_stm401->dsdev.dev != NULL)
			switch_set_state(&ps_stm401->dsdev, x);
		if (ps_stm401->edsdev.dev != NULL)
			switch_set_state(&ps_stm401->edsdev, x);

		dev_dbg(&ps_stm401->client->dev, "Dock status:%d\n", x);
	}
	if (irq_status & M_PROXIMITY) {
		stm401_cmdbuff[0] = PROXIMITY;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading prox from stm401 failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401, DT_PROX, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending Proximity distance %d\n", x);
	}
	if (irq_status & M_TOUCH) {
		u8 aod_wake_up_reason;
		stm401_cmdbuff[0] = STM401_STATUS_REG;
		if (stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2)
			< 0) {
			dev_err(&ps_stm401->client->dev,
				"Get status reg failed\n");
			goto EXIT;
		}
		aod_wake_up_reason = (stm401_readbuff[1] >> 4) & 0xf;
		if (aod_wake_up_reason == AOD_WAKEUP_REASON_ESD) {
			char *envp[2];
			envp[0] = "STM401WAKE=ESD";
			envp[1] = NULL;
			if (kobject_uevent_env(&ps_stm401->client->dev.kobj,
				KOBJ_CHANGE, envp)) {
				dev_err(&ps_stm401->client->dev,
					"Failed to create uevent\n");
				goto EXIT;
			}
			sysfs_notify(&ps_stm401->client->dev.kobj,
				NULL, "stm401_esd");
			dev_info(&ps_stm401->client->dev,
				"Sent uevent, STM401 ESD wake\n");
		} else {
			input_report_key(ps_stm401->input_dev, KEY_POWER, 1);
			input_report_key(ps_stm401->input_dev, KEY_POWER, 0);
			input_sync(ps_stm401->input_dev);
			dev_info(&ps_stm401->client->dev,
				"Report pwrkey toggle, touch event wake\n");
		}
	}
	if (irq_status & M_FLATUP) {
		stm401_cmdbuff[0] = FLAT_DATA;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading flat data from stm401 failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401, DT_FLAT_UP, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev, "Sending Flat up %d\n", x);
	}
	if (irq_status & M_FLATDOWN) {
		stm401_cmdbuff[0] = FLAT_DATA;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading flat data from stm401 failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401,
			DT_FLAT_DOWN, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev, "Sending Flat down %d\n", x);
	}
	if (irq_status & M_STOWED) {
		stm401_cmdbuff[0] = STOWED;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading stowed from stm401 failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401, DT_STOWED, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending Stowed status %d\n", x);
	}
	if (irq_status & M_CAMERA_ACT) {
		stm401_cmdbuff[0] = CAMERA;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading camera data from stm failed\n");
			goto EXIT;
		}
		x = CAMERA_DATA;
		y = (stm401_readbuff[0] << 8) | stm401_readbuff[1];

		stm401_as_data_buffer_write(ps_stm401, DT_CAMERA_ACT,
			x, y, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending Camera(x,y,z)values:x=%d,y=%d,z=%d\n",
			x, y, 0);

		input_report_key(ps_stm401->input_dev, KEY_CAMERA, 1);
		input_report_key(ps_stm401->input_dev, KEY_CAMERA, 0);
		input_sync(ps_stm401->input_dev);
		dev_dbg(&ps_stm401->client->dev,
			"Report camkey toggle\n");
	}
	if (irq_status & M_NFC) {
		stm401_cmdbuff[0] = NFC;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading nfc data from stm failed\n");
			goto EXIT;
		}
		x = stm401_readbuff[0];
		stm401_as_data_buffer_write(ps_stm401, DT_NFC,
			x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending NFC(x,y,z)values:x=%d,y=%d,z=%d\n",
		x, 0, 0);
	}
	if (irq2_status & M_MMOVEME) {
		/* Client recieving action will be upper 2 MSB of status */
		x = (irq2_status & STM401_CLIENT_MASK) | M_MMOVEME;
		stm401_ms_data_buffer_write(ps_stm401, DT_MMMOVE, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending meaningful movement event\n");
	}
	if (irq2_status & M_NOMMOVE) {
		/* Client recieving action will be upper 2 MSB of status */
		x = (irq2_status & STM401_CLIENT_MASK) | M_NOMMOVE;
		stm401_ms_data_buffer_write(ps_stm401, DT_NOMOVE, x, 0, 0, 0);

		dev_dbg(&ps_stm401->client->dev,
			"Sending no meaningful movement event\n");
	}
	if (irq2_status & M_ALGO_MODALITY) {
		stm401_cmdbuff[0] =
			stm401_algo_info[STM401_IDX_MODALITY].evt_register;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_EVT_SZ_TRANSITION);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading modality event failed\n");
			goto EXIT;
		}
		/* x (data1) msb: algo index, lsb: past, confidence */
		x = (STM401_IDX_MODALITY << 8) | stm401_readbuff[0];
		/* y (data2) old state */
		y = (stm401_readbuff[2] << 8) | stm401_readbuff[1];
		/* z (data3) new state */
		z = (stm401_readbuff[4] << 8) | stm401_readbuff[3];
		/* q (data4) time in state, in seconds */
		q = (stm401_readbuff[6] << 8) | stm401_readbuff[5];
		stm401_ms_data_buffer_write(ps_stm401, DT_ALGO_EVT, x, y, z, q);
		dev_dbg(&ps_stm401->client->dev, "Sending modality event\n");
	}
	if (irq2_status & M_ALGO_ORIENTATION) {
		stm401_cmdbuff[0] =
			stm401_algo_info[STM401_IDX_ORIENTATION].evt_register;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_EVT_SZ_TRANSITION);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading orientation event failed\n");
			goto EXIT;
		}
		/* x (data1) msb: algo index, lsb: past, confidence */
		x = (STM401_IDX_ORIENTATION << 8) | stm401_readbuff[0];
		/* y (data2) old state */
		y = (stm401_readbuff[2] << 8) | stm401_readbuff[1];
		/* z (data3) new state */
		z = (stm401_readbuff[4] << 8) | stm401_readbuff[3];
		/* q (data4) time in state, in seconds */
		q = (stm401_readbuff[6] << 8) | stm401_readbuff[5];
		stm401_ms_data_buffer_write(ps_stm401, DT_ALGO_EVT, x, y, z, q);
		dev_dbg(&ps_stm401->client->dev, "Sending orientation event\n");
	}
	if (irq2_status & M_ALGO_STOWED) {
		stm401_cmdbuff[0] =
			stm401_algo_info[STM401_IDX_STOWED].evt_register;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_EVT_SZ_TRANSITION);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading stowed event failed\n");
			goto EXIT;
		}
		/* x (data1) msb: algo index, lsb: past, confidence */
		x = (STM401_IDX_STOWED << 8) | stm401_readbuff[0];
		/* y (data2) old state */
		y = (stm401_readbuff[2] << 8) | stm401_readbuff[1];
		/* z (data3) new state */
		z = (stm401_readbuff[4] << 8) | stm401_readbuff[3];
		/* q (data4) time in state, in seconds */
		q = (stm401_readbuff[6] << 8) | stm401_readbuff[5];
		stm401_ms_data_buffer_write(ps_stm401, DT_ALGO_EVT, x, y, z, q);
		dev_dbg(&ps_stm401->client->dev, "Sending stowed event\n");
	}
	if (irq2_status & M_ALGO_ACCUM_MODALITY) {
		stm401_cmdbuff[0] =
			stm401_algo_info[STM401_IDX_ACCUM_MODALITY]
				.evt_register;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_EVT_SZ_ACCUM_STATE);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading accum modality event failed\n");
			goto EXIT;
		}
		/* x (data1) msb: algo index */
		x = STM401_IDX_ACCUM_MODALITY << 8;
		/* y (data2) id */
		y = (stm401_readbuff[1] << 8) | stm401_readbuff[0];
		stm401_ms_data_buffer_write(ps_stm401, DT_ALGO_EVT, x, y, 0, 0);
		dev_dbg(&ps_stm401->client->dev, "Sending accum modality event\n");
	}
	if (irq2_status & M_ALGO_ACCUM_MVMT) {
		stm401_cmdbuff[0] =
			stm401_algo_info[STM401_IDX_ACCUM_MVMT].evt_register;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_EVT_SZ_ACCUM_MVMT);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading accum mvmt event failed\n");
			goto EXIT;
		}
		/* x (data1) msb: algo index */
		x = STM401_IDX_ACCUM_MVMT << 8;
		/* y (data2) time_s */
		y = (stm401_readbuff[1] << 8) | stm401_readbuff[0];
		/* z (data3) distance */
		z = (stm401_readbuff[3] << 8) | stm401_readbuff[2];
		stm401_ms_data_buffer_write(ps_stm401, DT_ALGO_EVT, x, y, z, 0);
		dev_dbg(&ps_stm401->client->dev, "Sending accum mvmt event\n");
	}
EXIT:
	mutex_unlock(&ps_stm401->lock);
}
