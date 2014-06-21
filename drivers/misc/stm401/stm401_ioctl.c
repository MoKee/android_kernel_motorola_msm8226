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


long stm401_misc_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	static int brightness_table_loaded;
	int err = -ENOTTY;
	unsigned int addr = 0;
	struct stm401_data *ps_stm401 = file->private_data;
	unsigned char byte;
	unsigned char bytes[2];
	unsigned short delay;
	unsigned long current_posix_time;
	struct timespec current_time;

	if (ps_stm401->stm401_hub_fail)
		return -EIO;

	mutex_lock(&ps_stm401->lock);
	switch (cmd) {
	case STM401_IOCTL_BOOTLOADERMODE:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_BOOTLOADERMODE");
		err = switch_stm401_mode(BOOTMODE);
		break;
	case STM401_IOCTL_NORMALMODE:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_NORMALMODE");
		err = switch_stm401_mode(NORMALMODE);
		break;
	case STM401_IOCTL_MASSERASE:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_MASSERASE");
		err = stm401_boot_flash_erase();
		break;
	case STM401_IOCTL_SETSTARTADDR:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SETSTARTADDR");
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			dev_err(&ps_stm401->client->dev,
				"Copy start address returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_misc_data->current_addr = addr;
		err = 0;
		break;
	case STM401_IOCTL_SET_FACTORY_MODE:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_FACTORY_MODE");
		err = switch_stm401_mode(FACTORYMODE);
		break;
	case STM401_IOCTL_TEST_BOOTMODE:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_TEST_BOOTMODE");
		err = switch_stm401_mode(BOOTMODE);
		break;
	case STM401_IOCTL_SET_DEBUG:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_DEBUG");
		err = 0;
		break;
	case STM401_IOCTL_GET_VERNAME:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_VERNAME");
		if (copy_to_user(argp, &(ps_stm401->pdata->fw_version),
				FW_VERSION_SIZE))
			err = -EFAULT;
		else
			err = 0;
		break;
	default:
		if (ps_stm401->mode == BOOTMODE) {
			dev_err(&ps_stm401->client->dev,
				"Attempted normal mode ioctl in boot\n");
			mutex_unlock(&ps_stm401->lock);
			return -EFAULT;
		}
	}

	switch (cmd) {
	case STM401_IOCTL_GET_VERSION:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_VERSION");
		err = stm401_get_version(ps_stm401);
		break;
	case STM401_IOCTL_SET_ACC_DELAY:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_ACC_DELAY");
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy acc delay returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = ACCEL_UPDATE_RATE;
		stm401_cmdbuff[1] = delay;
		stm401_g_acc_delay = delay;
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;

	case STM401_IOCTL_SET_MAG_DELAY:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_MAG_DELAY");
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy mag delay returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = MAG_UPDATE_RATE;
		stm401_cmdbuff[1] = delay;
		stm401_g_mag_delay = delay;
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;
	case STM401_IOCTL_SET_GYRO_DELAY:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_GYRO_DELAY");
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy gyro delay returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = GYRO_UPDATE_RATE;
		stm401_cmdbuff[1] = delay;
		stm401_g_gyro_delay = delay;
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;
	case STM401_IOCTL_SET_PRES_DELAY:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_PRES_DELAY");
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy pres delay returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = PRESSURE_UPDATE_RATE;
		stm401_cmdbuff[1] = delay;
		stm401_g_baro_delay = delay;
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;
	case STM401_IOCTL_SET_SENSORS:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_SENSORS");
		if (copy_from_user(bytes, argp, 2 * sizeof(unsigned char))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy set sensors returned error\n");
			err = -EFAULT;
			break;
		}

		if ((brightness_table_loaded == 0)
				&& (bytes[1] & (M_DISP_BRIGHTNESS >> 8))) {
			err = stm401_load_brightness_table(ps_stm401);
			if (err) {
				dev_err(&ps_stm401->client->dev,
					"Loading brightness failed\n");
				break;
			}
			brightness_table_loaded = 1;
		}

		stm401_cmdbuff[0] = NONWAKESENSOR_CONFIG;
		stm401_cmdbuff[1] = bytes[0];
		stm401_cmdbuff[2] = bytes[1];
		stm401_g_nonwake_sensor_state = (stm401_cmdbuff[2] << 8)
			| stm401_cmdbuff[1];
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 3);
		dev_dbg(&ps_stm401->client->dev, "Sensor enable = 0x%02X\n",
			stm401_g_nonwake_sensor_state);
		break;
	case STM401_IOCTL_GET_SENSORS:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_SENSORS");
		stm401_cmdbuff[0] = NONWAKESENSOR_CONFIG;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading get sensors failed\n");
			break;
		}
		bytes[0] = stm401_readbuff[0];
		bytes[1] = stm401_readbuff[1];
		if (copy_to_user(argp, bytes, 2 * sizeof(unsigned char)))
			err = -EFAULT;
		break;
	case STM401_IOCTL_SET_WAKESENSORS:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_WAKESENSORS");
		if (copy_from_user(bytes, argp, 2 * sizeof(unsigned char))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy set sensors returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = WAKESENSOR_CONFIG;
		stm401_cmdbuff[1] = bytes[0];
		stm401_cmdbuff[2] = bytes[1];
		stm401_g_wake_sensor_state =  (stm401_cmdbuff[2] << 8)
			| stm401_cmdbuff[1];
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 3);
		dev_dbg(&ps_stm401->client->dev, "Sensor enable = 0x%02X\n",
			stm401_g_wake_sensor_state);
		break;
	case STM401_IOCTL_GET_WAKESENSORS:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_GET_WAKESENSORS");
		stm401_cmdbuff[0] = WAKESENSOR_CONFIG;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading get sensors failed\n");
			break;
		}
		bytes[0] = stm401_readbuff[0];
		bytes[1] = stm401_readbuff[1];
		if (copy_to_user(argp, bytes, 2 * sizeof(unsigned char)))
			err = -EFAULT;
		break;
	case STM401_IOCTL_SET_ALGOS:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_ALGOS");
		if (copy_from_user(&bytes, argp, sizeof(bytes))) {
			dev_err(&ps_stm401->client->dev,
				"Copy set algos returned error\n");
			err = -EFAULT;
			break;
		}
		dev_dbg(&ps_stm401->client->dev,
			"Set algos config: 0x%x", (bytes[1] << 8) | bytes[0]);
		stm401_cmdbuff[0] = ALGO_CONFIG;
		stm401_cmdbuff[1] = bytes[0];
		stm401_cmdbuff[2] = bytes[1];
		stm401_g_algo_state = (stm401_cmdbuff[2] << 8)
			| stm401_cmdbuff[1];
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 3);
		break;
	case STM401_IOCTL_GET_ALGOS:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_ALGOS");
		stm401_cmdbuff[0] = ALGO_CONFIG;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 2);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading get algos failed\n");
			break;
		}
		bytes[0] = stm401_readbuff[0];
		bytes[1] = stm401_readbuff[1];
		dev_info(&ps_stm401->client->dev,
			"Get algos config: 0x%x", (bytes[1] << 8) | bytes[0]);
		if (copy_to_user(argp, bytes, sizeof(bytes)))
			err = -EFAULT;
		break;
	case STM401_IOCTL_GET_MAG_CAL:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_MAG_CAL");
		stm401_cmdbuff[0] = MAG_CAL;
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			STM401_MAG_CAL_SIZE);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Reading get mag cal failed\n");
			break;
		}
		if (copy_to_user(argp, &stm401_readbuff[0],
				STM401_MAG_CAL_SIZE))
			err = -EFAULT;

		break;
	case STM401_IOCTL_SET_MAG_CAL:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_MAG_CAL");
		if (copy_from_user(&stm401_cmdbuff[1], argp,
			STM401_MAG_CAL_SIZE)) {
			dev_err(&ps_stm401->client->dev,
				"Copy set mag cal returned error\n");
			err = -EFAULT;
			break;
		}
		memcpy(stm401_g_mag_cal, &stm401_cmdbuff[1],
			STM401_MAG_CAL_SIZE);
		stm401_cmdbuff[0] = MAG_CAL;
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff,
			(STM401_MAG_CAL_SIZE + 1));
		break;
	case STM401_IOCTL_SET_MOTION_DUR:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_MOTION_DUR");
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy set motion dur returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = MOTION_DUR;
		stm401_cmdbuff[1] = addr & 0xFF;
		stm401_g_motion_dur =  stm401_cmdbuff[1];
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;
	case STM401_IOCTL_SET_ZRMOTION_DUR:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_ZRMOTION_DUR");
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			dev_dbg(&ps_stm401->client->dev,
				"Copy zmotion dur returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_cmdbuff[0] = ZRMOTION_DUR;
		stm401_cmdbuff[1] = addr & 0xFF;
		stm401_g_zmotion_dur =  stm401_cmdbuff[1];
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 2);
		break;
	case STM401_IOCTL_GET_DOCK_STATUS:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_GET_DOCK_STATUS");
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1, 1);
		byte = stm401_readbuff[0];
		if (copy_to_user(argp, &byte, sizeof(byte)))
			err = -EFAULT;
		break;
	case STM401_IOCTL_TEST_READ:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_TEST_READ");
		err = stm401_i2c_read(ps_stm401, &byte, 1);
		/* stm401 will return num of bytes read or error */
		if (err > 0)
			err = byte;
		break;
	case STM401_IOCTL_TEST_WRITE:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_TEST_WRITE");
		if (copy_from_user(&byte, argp, sizeof(unsigned char))) {
			dev_err(&ps_stm401->client->dev,
				"Copy test write returned error\n");
			err = -EFAULT;
			break;
		}
		err = stm401_i2c_write(ps_stm401, &byte, 1);
		break;
	case STM401_IOCTL_SET_POSIX_TIME:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_POSIX_TIME");

		if (copy_from_user(&current_posix_time, argp,
			 sizeof(current_posix_time))) {
			dev_err(&ps_stm401->client->dev,
				"Copy from user returned error\n");
			err = -EFAULT;
			break;
		}
		getnstimeofday(&current_time);
		stm401_time_delta = current_posix_time - current_time.tv_sec;
		stm401_cmdbuff[0] = AP_POSIX_TIME;
		stm401_cmdbuff[1] = (unsigned char)(current_posix_time >> 24);
		stm401_cmdbuff[2] = (unsigned char)((current_posix_time >> 16)
				& 0xff);
		stm401_cmdbuff[3] = (unsigned char)((current_posix_time >> 8)
				& 0xff);
		stm401_cmdbuff[4] = (unsigned char)((current_posix_time)
			& 0xff);
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 5);
		break;
	case STM401_IOCTL_SET_CONTROL_REG:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_SET_CONTROL_REG");
		if (brightness_table_loaded == 0) {
			err = stm401_load_brightness_table(ps_stm401);
			if (err) {
				dev_err(&ps_stm401->client->dev,
					"Loading brightness failed\n");
				break;
			}
			brightness_table_loaded = 1;
		}
		stm401_cmdbuff[0] = STM401_CONTROL_REG;
		if (copy_from_user(&stm401_cmdbuff[1], argp,
			 STM401_CONTROL_REG_SIZE)) {
			dev_err(&ps_stm401->client->dev,
				"Copy from user returned error\n");
			err = -EFAULT;
			break;
		}
		memcpy(stm401_g_control_reg, &stm401_cmdbuff[1],
			STM401_CONTROL_REG_SIZE);

		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff,
			(STM401_CONTROL_REG_SIZE + 1));
		if (err < 0)
			dev_err(&stm401_misc_data->client->dev,
				"unable to write control reg %d\n", err);
		else
			ps_stm401->ap_stm401_handoff_enable = true;

		break;
	case STM401_IOCTL_GET_AOD_INSTRUMENTATION_REG:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_GET_AOD_INTRUMENTATION_REG");
		stm401_cmdbuff[0] = STM_AOD_INSTRUMENTATION_REG;
		err = stm401_i2c_write_read(ps_stm401,
			stm401_cmdbuff, 1, STM_AOD_INSTRUMENTATION_REG_SIZE);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Get AOD instrumentation reg failed\n");
			break;
		}
		if (copy_to_user(argp, stm401_readbuff,
				STM_AOD_INSTRUMENTATION_REG_SIZE))
			err = -EFAULT;
		break;
	case STM401_IOCTL_GET_STATUS_REG:
		dev_dbg(&ps_stm401->client->dev,
			"STM401_IOCTL_GET_STATUS_REG");
		stm401_cmdbuff[0] = STM401_STATUS_REG;
		err = stm401_i2c_write_read(ps_stm401,
			 stm401_cmdbuff, 1, STM401_STATUS_REG_SIZE);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Get status reg failed\n");
			break;
		}

		if (copy_to_user(argp, stm401_readbuff, STM401_STATUS_REG_SIZE))
			err = -EFAULT;
		break;
	case STM401_IOCTL_GET_TOUCH_REG:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_TOUCH_REG");
		stm401_cmdbuff[0] = STM401_TOUCH_REG;
		err = stm401_i2c_write_read(ps_stm401,
			 stm401_cmdbuff, 1, STM401_TOUCH_REG_SIZE);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Get touch reg failed\n");
			break;
		}

		if (copy_to_user(argp, stm401_readbuff, STM401_TOUCH_REG_SIZE))
			err = -EFAULT;
		break;
	case STM401_IOCTL_SET_ALGO_REQ:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_SET_ALGO_REQ");
		/* copy algo into bytes[2] */
		if (copy_from_user(&bytes, argp, sizeof(bytes))) {
			dev_err(&ps_stm401->client->dev,
				"Set algo req copy bytes returned error\n");
			err = -EFAULT;
			break;
		}
		addr = (bytes[1] << 8) | bytes[0];
		/* copy len into byte */
		if (copy_from_user(&byte, argp + sizeof(bytes), sizeof(byte))) {
			dev_err(&ps_stm401->client->dev,
				"Set algo req copy byte returned error\n");
			err = -EFAULT;
			break;
		}
		/* algo req register */
		dev_dbg(&ps_stm401->client->dev,
			"Set algo req, algo idx: %d, len: %u\n", addr, byte);
		if (addr < STM401_NUM_ALGOS) {
			stm401_cmdbuff[0] = stm401_algo_info[addr].req_register;
			dev_dbg(&ps_stm401->client->dev,
				"Register: 0x%x", stm401_cmdbuff[0]);
		} else {
			dev_err(&ps_stm401->client->dev,
				"Set algo req invalid arg\n");
			err = -EFAULT;
			break;
		}
		if (copy_from_user(&stm401_cmdbuff[1],
			argp + sizeof(bytes) + sizeof(byte), byte)) {
			dev_err(&ps_stm401->client->dev,
				"Set algo req copy req info returned error\n");
			err = -EFAULT;
			break;
		}
		stm401_g_algo_requst[addr].size = byte;
		memcpy(stm401_g_algo_requst[addr].data,
			&stm401_cmdbuff[1], byte);
		err = stm401_i2c_write(ps_stm401, stm401_cmdbuff, 1 + byte);
		break;
	case STM401_IOCTL_GET_ALGO_EVT:
		dev_dbg(&ps_stm401->client->dev, "STM401_IOCTL_GET_ALGO_EVT");
		/* copy algo into bytes[2] */
		if (copy_from_user(&bytes, argp, sizeof(bytes))) {
			dev_err(&ps_stm401->client->dev,
				"Get algo evt copy bytes returned error\n");
			err = -EFAULT;
			break;
		}
		addr = (bytes[1] << 8) | bytes[0];
		/* algo evt register */
		dev_dbg(&ps_stm401->client->dev,
			"Get algo evt, algo idx: %d\n", addr);
		if (addr < STM401_NUM_ALGOS) {
			stm401_cmdbuff[0] = stm401_algo_info[addr].evt_register;
			dev_dbg(&ps_stm401->client->dev,
				"Register: 0x%x", stm401_cmdbuff[0]);
		} else {
			dev_err(&ps_stm401->client->dev,
				"Get algo evt invalid arg\n");
			err = -EFAULT;
			break;
		}
		err = stm401_i2c_write_read(ps_stm401, stm401_cmdbuff, 1,
			stm401_algo_info[addr].evt_size);
		if (err < 0) {
			dev_err(&ps_stm401->client->dev,
				"Get algo evt failed\n");
			break;
		}
		if (copy_to_user(argp + sizeof(bytes), stm401_readbuff,
			stm401_algo_info[addr].evt_size))
			err = -EFAULT;
		break;
	/* No default here since previous switch could have
	   handled the command and cannot over write that */
	}

	mutex_unlock(&ps_stm401->lock);
	return err;
}
