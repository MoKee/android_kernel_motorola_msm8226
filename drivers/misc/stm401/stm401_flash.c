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


#define OLD_BOOT_VER 0x10

#define ACK_BYTE 0x79
#define BUSY_BYTE 0x76

#define ERASE_DELAY 200
#define ERASE_TIMEOUT 50

#define WRITE_DELAY 20
#define WRITE_TIMEOUT 20

enum stm_command {
	GET_VERSION = 0x01,
	GET_ID = 0x02,
	READ_MEMORY = 0x11,
	GO = 0x21,
	WRITE_MEMORY = 0x31,
	NO_WAIT_WRITE_MEMORY = 0x32,
	ERASE = 0x44,
	NO_WAIT_ERASE = 0x45,
};


static unsigned char stm401_bootloader_ver;


static int stm401_boot_i2c_write(struct stm401_data *ps_stm401,
	u8 *buf, int len)
{
	int ret;
	struct i2c_msg msgs[] = {
		{
			.addr = ps_stm401->client->addr,
			.flags = ps_stm401->client->flags,
			.len = len,
			.buf = buf,
		}
	};

	if (buf == NULL || len == 0)
		return -EFAULT;

	ret = i2c_transfer(ps_stm401->client->adapter, msgs, 1);
	if (ret < 0) {
		dev_err(&stm401_misc_data->client->dev,
			"I2C write bus error\n");
	}

	return  ret;
}

static int stm401_boot_i2c_read(struct stm401_data *ps_stm401, u8 *buf, int len)
{
	int ret;
	struct i2c_msg msgs[] = {
		{
			.addr = ps_stm401->client->addr,
			.flags = ps_stm401->client->flags | I2C_M_RD,
			.len = len,
			.buf = buf,
		}
	};

	if (buf == NULL || len == 0)
		return -EFAULT;

	ret = i2c_transfer(ps_stm401->client->adapter, msgs, 1);
	if (ret < 0) {
		dev_err(&stm401_misc_data->client->dev,
			"I2C read bus error\n");
	}

	return  ret;
}

static int stm401_boot_cmd_write(struct stm401_data *ps_stm401,
	enum stm_command command)
{
	int index = 0;

	stm401_cmdbuff[index++] = command;
	stm401_cmdbuff[index++] = ~command;

	return stm401_boot_i2c_write(ps_stm401, stm401_cmdbuff, index);
}

static int stm401_boot_checksum_write(struct stm401_data *ps_stm401,
	u8 *buf, int len)
{
	int i;
	u8 checksum = 0;

	for (i = 0; i < len; i++)
		checksum ^= buf[i];
	buf[i++] = checksum;

	return stm401_boot_i2c_write(ps_stm401, buf, i);
}

static int stm401_get_boot_ver(void)
{
	int err;

	err = stm401_boot_cmd_write(stm401_misc_data, GET_VERSION);
	if (err < 0)
		return err;
	err = stm401_boot_i2c_read(stm401_misc_data,
		stm401_readbuff, 1);
	if (err < 0)
		return err;

	if (stm401_readbuff[0] != ACK_BYTE) {
		dev_err(&stm401_misc_data->client->dev,
			"Error sending GET_VERSION command 0x%02x\n",
			stm401_readbuff[0]);
		return -EIO;
	}

	err = stm401_boot_i2c_read(stm401_misc_data,
		stm401_readbuff, 1);
	if (err < 0)
		return err;
	stm401_bootloader_ver = stm401_readbuff[0];
	dev_err(&stm401_misc_data->client->dev,
		"Bootloader version 0x%02x\n", stm401_bootloader_ver);

	err = stm401_boot_i2c_read(stm401_misc_data,
		stm401_readbuff, 1);
	if (err < 0)
		return err;
	if (stm401_readbuff[0] != ACK_BYTE) {
		dev_err(&stm401_misc_data->client->dev,
			"Error sending GET_VERSION command 0x%02x\n",
			stm401_readbuff[0]);
		return -EIO;
	}

	return stm401_bootloader_ver;
}

int stm401_boot_flash_erase(void)
{
	int index = 0;
	int count = 0;
	int err = 0;

	if (stm401_bootloader_ver == 0) {
		if (stm401_get_boot_ver() <= 0) {
			err = -EIO;
			goto EXIT;
		}
	}

	dev_dbg(&stm401_misc_data->client->dev,
		"Starting flash erase\n");

	if (stm401_bootloader_ver > OLD_BOOT_VER) {
		/* Use new bootloader erase command */
		err = stm401_boot_cmd_write(stm401_misc_data, NO_WAIT_ERASE);
		if (err < 0)
			goto EXIT;

		err = stm401_boot_i2c_read(stm401_misc_data,
			stm401_readbuff, 1);
		if (err < 0)
			goto EXIT;
		if (stm401_readbuff[0] != ACK_BYTE) {
			dev_err(&stm401_misc_data->client->dev,
				"Error sending ERASE command 0x%02x\n",
				stm401_readbuff[0]);
			err = -EIO;
			goto EXIT;
		}

		stm401_cmdbuff[index++] = 0xFF;
		stm401_cmdbuff[index++] = 0xFF;
		err = stm401_boot_checksum_write(stm401_misc_data,
			stm401_cmdbuff, index);
		if (err < 0)
			goto EXIT;

		stm401_readbuff[0] = 0;
		do {
			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] == BUSY_BYTE) {
				msleep(ERASE_DELAY);
				count++;
				if (count == ERASE_TIMEOUT)
					break;
			}
		} while (stm401_readbuff[0] == BUSY_BYTE);
		if (stm401_readbuff[0] != ACK_BYTE) {
			dev_err(&stm401_misc_data->client->dev,
				"Error sending ERASE data 0x%02x\n",
				stm401_readbuff[0]);
			err = -EIO;
			goto EXIT;
		}
	} else {
		/* Use old bootloader erase command */
		err = stm401_boot_cmd_write(stm401_misc_data, ERASE);
		if (err < 0)
			goto EXIT;

		err = stm401_boot_i2c_read(stm401_misc_data,
			stm401_readbuff, 1);
		if (err < 0)
			goto EXIT;
		if (stm401_readbuff[0] != ACK_BYTE) {
			dev_err(&stm401_misc_data->client->dev,
				"Error sending ERASE command 0x%02x\n",
				stm401_readbuff[0]);
			err = -EIO;
			goto EXIT;
		}

		stm401_cmdbuff[index++] = 0xFF;
		stm401_cmdbuff[index++] = 0xFF;
		err = stm401_boot_checksum_write(stm401_misc_data,
			stm401_cmdbuff, index);
		if (err < 0)
			goto EXIT;

		/* We should be checking for an ACK here, but waiting
		   for the erase to complete takes too long and the I2C
		   driver will time out and fail.
		   Instead we just wait and hope the erase was succesful.
		*/
		msleep(10000);
	}

	dev_dbg(&stm401_misc_data->client->dev,
		"Flash erase successful\n");
EXIT:
	return err;
}

int stm401_get_version(struct stm401_data *ps_stm401)
{
	int err = 0;
	if (ps_stm401->mode == BOOTMODE) {
		dev_dbg(&ps_stm401->client->dev,
			"Switch to normal to get version\n");
		switch_stm401_mode(NORMALMODE);
		msleep_interruptible(stm401_i2c_retry_delay);
	}
	dev_dbg(&ps_stm401->client->dev, "STM software version: ");
	stm401_cmdbuff[0] = REV_ID;
	err = stm401_i2c_write_read_no_reset(ps_stm401, stm401_cmdbuff, 1, 1);
	if (err >= 0) {
		err = (int)stm401_readbuff[0];
		dev_err(&ps_stm401->client->dev, "STM401 version %02x",
			stm401_readbuff[0]);
	}
	return err;
}

int switch_stm401_mode(enum stm_mode mode)
{
	struct stm401_platform_data *pdata;
	unsigned int bslen_pin_active_value =
		stm401_misc_data->pdata->bslen_pin_active_value;
	int ret;

	pdata = stm401_misc_data->pdata;
	stm401_misc_data->mode = mode;

	/* bootloader mode */
	if (mode == BOOTMODE) {
		gpio_set_value(pdata->gpio_bslen,
				(bslen_pin_active_value));
		dev_dbg(&stm401_misc_data->client->dev,
			"Switching to boot mode\n");
		stm401_reset(pdata);

		ret = stm401_boot_cmd_write(stm401_misc_data, GET_ID);
		if (ret < 0)
			return ret;

		ret = stm401_boot_i2c_read(stm401_misc_data,
			stm401_readbuff, 1);
		if (ret < 0)
			return ret;
		if (stm401_readbuff[0] != ACK_BYTE) {
			dev_err(&stm401_misc_data->client->dev,
				"Error sending GET_ID command 0x%02x\n",
				stm401_readbuff[0]);
			return -EIO;
		}

		ret = stm401_boot_i2c_read(stm401_misc_data,
			stm401_readbuff, 3);
		if (ret < 0)
			return ret;
		dev_err(&stm401_misc_data->client->dev,
			"Part ID 0x%02x 0x%02x 0x%02x\n", stm401_readbuff[0],
			stm401_readbuff[1], stm401_readbuff[2]);

		ret = stm401_boot_i2c_read(stm401_misc_data,
			stm401_readbuff, 1);
		if (ret < 0)
			return ret;
		if (stm401_readbuff[0] != ACK_BYTE) {
			dev_err(&stm401_misc_data->client->dev,
				"Error reading part ID 0x%02x\n",
				stm401_readbuff[0]);
			return -EIO;
		}
	} else {
		/*normal mode */
		gpio_set_value(pdata->gpio_bslen,
				!(bslen_pin_active_value));
		dev_dbg(&stm401_misc_data->client->dev,
			"Switching to normal mode\n");
		stm401_reset(pdata);
	}

	return 0;
}

static int stm401_misc_open(struct inode *inode, struct file *file)
{
	int err = 0;
	dev_dbg(&stm401_misc_data->client->dev, "stm401_misc_open\n");

	err = nonseekable_open(inode, file);
	if (err < 0)
		return err;
	file->private_data = stm401_misc_data;

	err = stm401_enable(stm401_misc_data);

	return err;
}

ssize_t stm401_misc_write(struct file *file, const char __user *buff,
				 size_t count, loff_t *ppos)
{
	int index = 0;
	int wait_count = 0;
	int err = 0;

	if (count > STM401_MAXDATA_LENGTH || count == 0) {
		dev_err(&stm401_misc_data->client->dev,
			"Invalid packet size %d\n", count);
		err = -EINVAL;
		return err;
	}

	mutex_lock(&stm401_misc_data->lock);

	if (stm401_bootloader_ver == 0) {
		if (stm401_get_boot_ver() <= 0) {
			err = -EIO;
			goto EXIT;
		}
	}

	if (stm401_misc_data->mode == BOOTMODE) {
		dev_dbg(&stm401_misc_data->client->dev,
			"Starting flash write, %d bytes to address 0x%08x\n",
			count, stm401_misc_data->current_addr);

		if (stm401_bootloader_ver > OLD_BOOT_VER) {
			/* Use new bootloader write command */
			err = stm401_boot_cmd_write(stm401_misc_data,
				NO_WAIT_WRITE_MEMORY);
			if (err < 0)
				goto EXIT;

			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
				 "Error sending WRITE_MEMORY command 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}

			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 24) & 0xFF;
			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 16) & 0xFF;
			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 8) & 0xFF;
			stm401_cmdbuff[index++]
				= stm401_misc_data->current_addr & 0xFF;
			err = stm401_boot_checksum_write(stm401_misc_data,
				stm401_cmdbuff, index);
			if (err < 0)
				goto EXIT;

			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
					"Error sending memory address 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}

			stm401_cmdbuff[0] = count - 1;
			if (copy_from_user(&stm401_cmdbuff[1], buff, count)) {
				dev_err(&stm401_misc_data->client->dev,
					"Copy from user returned error\n");
				err = -EINVAL;
				goto EXIT;
			}
			if (count & 0x1) {
				stm401_cmdbuff[count + 1] = 0xFF;
				count++;
			}
			err = stm401_boot_checksum_write(stm401_misc_data,
				stm401_cmdbuff, count + 1);
			if (err < 0)
				goto EXIT;

			stm401_readbuff[0] = 0;
			do {
				err = stm401_boot_i2c_read(stm401_misc_data,
					stm401_readbuff, 1);
				if (err < 0)
					goto EXIT;
				if (stm401_readbuff[0] == BUSY_BYTE) {
					msleep(WRITE_DELAY);
					wait_count++;
					if (wait_count == WRITE_TIMEOUT)
						break;
				}
			} while (stm401_readbuff[0] == BUSY_BYTE);
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
					"Error sending write data 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}

			stm401_misc_data->current_addr += count;
		} else {
			/* Use old bootloader write command */
			err = stm401_boot_cmd_write(stm401_misc_data,
				WRITE_MEMORY);
			if (err < 0)
				goto EXIT;
			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
				 "Error sending WRITE_MEMORY command 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}

			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 24) & 0xFF;
			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 16) & 0xFF;
			stm401_cmdbuff[index++]
				= (stm401_misc_data->current_addr >> 8) & 0xFF;
			stm401_cmdbuff[index++]
				= stm401_misc_data->current_addr & 0xFF;
			err = stm401_boot_checksum_write(stm401_misc_data,
				stm401_cmdbuff, index);
			if (err < 0)
				goto EXIT;
			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
					"Error sending memory address 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}

			stm401_cmdbuff[0] = count - 1;
			if (copy_from_user(&stm401_cmdbuff[1], buff, count)) {
				dev_err(&stm401_misc_data->client->dev,
					"Copy from user returned error\n");
				err = -EINVAL;
				goto EXIT;
			}
			if (count & 0x1) {
				stm401_cmdbuff[count + 1] = 0xFF;
				count++;
			}
			err = stm401_boot_checksum_write(stm401_misc_data,
				stm401_cmdbuff, count + 1);
			if (err < 0)
				goto EXIT;
			err = stm401_boot_i2c_read(stm401_misc_data,
				stm401_readbuff, 1);
			if (err < 0)
				goto EXIT;
			if (stm401_readbuff[0] != ACK_BYTE) {
				dev_err(&stm401_misc_data->client->dev,
					"Error sending flash data 0x%02x\n",
					stm401_readbuff[0]);
				err = -EIO;
				goto EXIT;
			}
			stm401_misc_data->current_addr += count;
		}
		dev_dbg(&stm401_misc_data->client->dev,
			"Flash write completed\n");
	} else {
		dev_dbg(&stm401_misc_data->client->dev,
			"Normal mode write started\n");
		if (copy_from_user(stm401_cmdbuff, buff, count)) {
			dev_err(&stm401_misc_data->client->dev,
				"Copy from user returned error\n");
			err = -EINVAL;
		}
		if (err == 0)
			err = stm401_i2c_write_no_reset(stm401_misc_data,
				stm401_cmdbuff, count);
		if (err == 0)
			err = count;
	}

EXIT:
	mutex_unlock(&stm401_misc_data->lock);
	return err;
}


const struct file_operations stm401_misc_fops = {
	.owner = THIS_MODULE,
	.open = stm401_misc_open,
	.unlocked_ioctl = stm401_misc_ioctl,
	.write = stm401_misc_write,
};
