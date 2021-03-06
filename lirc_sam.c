/*
 * lirc_sam.c
 *
 * lirc_sam - Device driver that records pulse- and pause-lengths
 *	      (space-lengths) (just like the lirc_serial driver does)
 *	      between GPIO interrupt events on the AT91SAM9G25.
 *	      Lots of code has been taken from the lirc_serial module
 *	      and the lirc_rpi module<http://www.aron.ws/projects/lirc_rpi/>,
 *	      so I would like say thanks to the authors.
 *
 * Copyright (C) 2015 Hai Peng Yu <yuhp@moband>,
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <linux/gpio.h>

#define LIRC_DRIVER_NAME "lirc_sam"
#define RBUF_LEN 256
#define LIRC_TRANSMITTER_LATENCY 256

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG LIRC_DRIVER_NAME ": "	\
			       fmt, ## args);			\
	} while (0)

/* module parameters */

/* set the default GPIO input pin */
static int gpio_in_pin = 43;
/* set the default GPIO output pin */
static int gpio_out_pin = 44;


/* set the default GPIO input pin offset in gpiochip */
static int gpiochip_in_pin = 11;
/* set the default GPIO output pin offset in gpiochip */
static int gpiochip_out_pin = 12;


/* set the pin offset base on the gpiochip bank */
static int gpiochip_in_offset = 0;
static int gpiochip_out_offset = 0;

/* set the index of the gpiochip */
static int gpiochip_in_label_index = 0;
static int gpiochip_out_label_index = 0;

char* gpiochip_label[] = {
   "fffff400.gpio" ,
   "fffff600.gpio" ,
   "fffff800.gpio" ,
   "fffffa00.gpio"
};

/* enable debugging messages */
static int debug;
/* -1 = auto, 0 = active high, 1 = active low */
static int sense = -1;
/* use softcarrier by default */
static int softcarrier = 1;

struct gpio_chip *gpiochip_in; //gpio_chip for ir input pin
struct gpio_chip *gpiochip_out; //gpio_chip for ir output pin
struct irq_chip *irqchip; //Only the ir input pin need irq
struct irq_data *irqdata; //and irq data

/* forward declarations */
static long send_pulse(unsigned long length);
static void send_space(long length);
static void lirc_sam_exit(void);

int valid_gpio_pins[] = {21 ,22 ,23 ,43 ,44 ,45 ,46 ,66 ,67 ,68 ,95}; 


static struct platform_device *lirc_sam_dev;
static struct timeval lasttv = { 0, 0 };
static struct lirc_buffer rbuf;
static spinlock_t lock;

/* initialized/set in init_timing_params() */
static unsigned int freq = 38000;
static unsigned int duty_cycle = 50;
static unsigned long period;
static unsigned long pulse_width;
static unsigned long space_width;


/* calculate the pin offset of the gpiochip for Arietta G25  */
static int gpio_chip_label_offset(int pin, int *gpiochip_pin, int *gpiochip_offset, int *gpiochip_label_index){
    if(0 <= pin && pin <=31){
         *gpiochip_offset = 0;
         *gpiochip_label_index = 0; 
    }else if(32 <= pin && pin <=63){
         *gpiochip_offset = 32;
         *gpiochip_label_index = 1;
    }else if(64 <= pin && pin <=95){
         *gpiochip_offset = 64;
         *gpiochip_label_index = 2; 
    }else if(96 <= pin && pin <=127){
         *gpiochip_offset = 96;
         *gpiochip_label_index = 3;
    }
    *gpiochip_pin  = pin - *gpiochip_offset;
    return 0;
}



static void safe_udelay(unsigned long usecs)
{
	while (usecs > MAX_UDELAY_US) {
		udelay(MAX_UDELAY_US);
		usecs -= MAX_UDELAY_US;
	}
	udelay(usecs);
}

static int init_timing_params(unsigned int new_duty_cycle,
	unsigned int new_freq)
{
	/*
	 * period, pulse/space width are kept with 8 binary places -
	 * IE multiplied by 256.
	 */
	if (256 * 1000000L / new_freq * new_duty_cycle / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	if (256 * 1000000L / new_freq * (100 - new_duty_cycle) / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	duty_cycle = new_duty_cycle;
	freq = new_freq;
	period = 256 * 1000000L / freq;
	pulse_width = period * duty_cycle / 100;
	space_width = period - pulse_width;
	dprintk("in init_timing_params, freq=%d pulse=%ld, "
		"space=%ld\n", freq, pulse_width, space_width);
	return 0;
}

static long send_pulse_softcarrier(unsigned long length)
{
	int flag;
	unsigned long actual, target, d;

	length <<= 8;

	actual = 0; target = 0; flag = 0;
	while (actual < length) {
		if (flag) {
			gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 0);
			target += space_width;
		} else {
			gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 1);
			target += pulse_width;
		}
		d = (target - actual -
		     LIRC_TRANSMITTER_LATENCY + 128) >> 8;
		/*
		 * Note - we've checked in ioctl that the pulse/space
		 * widths are big enough so that d is > 0
		 */
		udelay(d);
		actual += (d << 8) + LIRC_TRANSMITTER_LATENCY;
		flag = !flag;
	}
	return (actual-length) >> 8;
}

static long send_pulse(unsigned long length)
{
	if (length <= 0)
		return 0;

	if (softcarrier) {
		return send_pulse_softcarrier(length);
	} else {
		gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 1);
		safe_udelay(length);
		return 0;
	}
}

static void send_space(long length)
{
	gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 0);
	if (length <= 0)
		return;
	safe_udelay(length);
}

static void rbwrite(int l)
{
	if (lirc_buffer_full(&rbuf)) {
		/* no new signals will be accepted */
		dprintk("Buffer overrun\n");
		return;
	}
	lirc_buffer_write(&rbuf, (void *)&l);
}

static void frbwrite(int l)
{
	/* simple noise filter */
	static int pulse, space;
	static unsigned int ptr;

	if (ptr > 0 && (l & PULSE_BIT)) {
		pulse += l & PULSE_MASK;
		if (pulse > 250) {
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
		return;
	}
	if (!(l & PULSE_BIT)) {
		if (ptr == 0) {
			if (l > 20000) {
				space = l;
				ptr++;
				return;
			}
		} else {
			if (l > 20000) {
				space += pulse;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				space += l;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				pulse = 0;
				return;
			}
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
	}
	rbwrite(l);
}

static irqreturn_t irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	long deltv;
	int data;
	int signal;

	/* use the GPIO signal level */
	signal = gpiochip_in->get(gpiochip_in, gpiochip_in_pin);

	/* unmask the irq */
	irqchip->irq_unmask(irqdata);

	if (sense != -1) {
		/* get current time */
		do_gettimeofday(&tv);

		/* calc time since last interrupt in microseconds */
		deltv = tv.tv_sec-lasttv.tv_sec;
		if (tv.tv_sec < lasttv.tv_sec ||
		    (tv.tv_sec == lasttv.tv_sec &&
		     tv.tv_usec < lasttv.tv_usec)) {
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": AIEEEE: your clock just jumped backwards\n");
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": %d %d %lx %lx %lx %lx\n", signal, sense,
			       tv.tv_sec, lasttv.tv_sec,
			       tv.tv_usec, lasttv.tv_usec);
			data = PULSE_MASK;
		} else if (deltv > 15) {
			data = PULSE_MASK; /* really long time */
			if (!(signal^sense)) {
				/* sanity check */
				printk(KERN_WARNING LIRC_DRIVER_NAME
				       ": AIEEEE: %d %d %lx %lx %lx %lx\n",
				       signal, sense, tv.tv_sec, lasttv.tv_sec,
				       tv.tv_usec, lasttv.tv_usec);
				/*
				 * detecting pulse while this
				 * MUST be a space!
				 */
				sense = sense ? 0 : 1;
			}
		} else {
			data = (int) (deltv*1000000 +
				      (tv.tv_usec - lasttv.tv_usec));
		}
		frbwrite(signal^sense ? data : (data|PULSE_BIT));
		lasttv = tv;
		wake_up_interruptible(&rbuf.wait_poll);
	}

	return IRQ_HANDLED;
}

static int is_right_chip(struct gpio_chip *chip, void *data)
{
	dprintk("is_right_chip %s %s %d\n", chip->label, (char*)data, strcmp(data, chip->label));

	if (strcmp(data, chip->label) == 0)
		return 1;
	return 0;
}

static int init_port(void)
{
	int i, nlow, nhigh, ret, irq;
       
        gpio_chip_label_offset(gpio_in_pin, &gpiochip_in_pin, &gpiochip_in_offset, &gpiochip_in_label_index); //Get the gpiochip info for input pin
        gpio_chip_label_offset(gpio_out_pin,&gpiochip_out_pin,&gpiochip_out_offset,&gpiochip_out_label_index);//Get the gpiochip info for output pin
	gpiochip_in  = gpiochip_find(gpiochip_label[gpiochip_in_label_index], is_right_chip);
	gpiochip_out = gpiochip_find(gpiochip_label[gpiochip_out_label_index], is_right_chip);
        
	if (!gpiochip_in || !gpiochip_out)
		return -ENODEV;

	if (gpio_request(gpio_out_pin, LIRC_DRIVER_NAME " ir/out")) {
		printk(KERN_ALERT LIRC_DRIVER_NAME
		       ": cant claim gpio pin %d\n", gpio_out_pin);
		ret = -ENODEV;
		goto exit_init_port;
	}

	if (gpio_request(gpio_in_pin, LIRC_DRIVER_NAME " ir/in")) {
		printk(KERN_ALERT LIRC_DRIVER_NAME
		       ": cant claim gpio pin %d\n", gpio_in_pin);
		ret = -ENODEV;
		goto exit_gpio_free_out_pin;
	}

	gpiochip_in->direction_input(gpiochip_in, gpiochip_in_pin);
	gpiochip_out->direction_output(gpiochip_out, gpiochip_out_pin, 1);
	gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 0);

	irq = gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin);

	dprintk("to_irq %d\n", irq);
	irqdata = irq_get_irq_data(irq);

	if (irqdata && irqdata->chip) {
		irqchip = irqdata->chip;
	} else {
		ret = -ENODEV;
		goto exit_gpio_free_in_pin;
	}

	/* if pin is high, then this must be an active low receiver. */
	if (sense == -1) {
		/* wait 1/2 sec for the power supply */
		msleep(500);

		/*
		 * probe 9 times every 0.04s, collect "votes" for
		 * active high/low
		 */
		nlow = 0;
		nhigh = 0;
		for (i = 0; i < 9; i++) {
			if (gpiochip_in->get(gpiochip_in, gpiochip_in_pin))
				nlow++;
			else
				nhigh++;
			msleep(40);
		}
		sense = (nlow >= nhigh ? 1 : 0);
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": auto-detected active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
	} else {
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": manually using active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
	}

	return 0;

	exit_gpio_free_in_pin:
	gpio_free(gpio_in_pin);

	exit_gpio_free_out_pin:
	gpio_free(gpio_out_pin);

	exit_init_port:
	return ret;
}

// called when the character device is opened
static int set_use_inc(void *data)
{
	int result;
	unsigned long flags;

	/* initialize timestamp */
	do_gettimeofday(&lasttv);

	result = request_irq(gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin),
			     (irq_handler_t) irq_handler, 0,
			     LIRC_DRIVER_NAME, (void*) 0);

	switch (result) {
	case -EBUSY:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": IRQ %d is busy\n",
		       gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin));
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": Bad irq number or handler\n");
		return -EINVAL;
	default:
		dprintk("Interrupt %d obtained\n",
			gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin));
		break;
	};

	/* initialize pulse/space widths */
	init_timing_params(duty_cycle, freq);

	spin_lock_irqsave(&lock, flags);

	/* GPIO Pin Falling/Rising Edge Detect Enable */
	irqchip->irq_set_type(irqdata,
			      IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING);

	/* unmask the irq */
	irqchip->irq_unmask(irqdata);

	spin_unlock_irqrestore(&lock, flags);

	return 0;
}

static void set_use_dec(void *data)
{
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);

	/* GPIO Pin Falling/Rising Edge Detect Disable */
	//irqchip->irq_set_type(irqdata, IRQ_TYPE_NONE ); //The IRQ_TYPE_NONE just do nothing but a warning in AT91
	irqchip->irq_mask(irqdata);

	spin_unlock_irqrestore(&lock, flags);

	free_irq(gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin), (void *) 0);

	dprintk(KERN_INFO LIRC_DRIVER_NAME
		": freed IRQ %d\n", gpiochip_in->to_irq(gpiochip_in, gpiochip_in_pin));
}

static ssize_t lirc_write(struct file *file, const char *buf,
	size_t n, loff_t *ppos)
{
	int i, count;
	unsigned long flags;
	long delta = 0;
	int *wbuf;

	count = n / sizeof(int);
	if (n % sizeof(int) || count % 2 == 0)
		return -EINVAL;
	wbuf = memdup_user(buf, n);
	if (IS_ERR(wbuf))
		return PTR_ERR(wbuf);
	spin_lock_irqsave(&lock, flags);

	for (i = 0; i < count; i++) {
		if (i%2)
			send_space(wbuf[i] - delta);
		else
			delta = send_pulse(wbuf[i]);
	}
	gpiochip_out->set(gpiochip_out, gpiochip_out_pin, 0);

	spin_unlock_irqrestore(&lock, flags);
	kfree(wbuf);
	return n;
}

static long lirc_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	int result;
	__u32 value;

	switch (cmd) {
	case LIRC_GET_SEND_MODE:
		return -ENOIOCTLCMD;
		break;

	case LIRC_SET_SEND_MODE:
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		/* only LIRC_MODE_PULSE supported */
		if (value != LIRC_MODE_PULSE)
			return -ENOSYS;
		break;

	case LIRC_GET_LENGTH:
		return -ENOSYS;
		break;

	case LIRC_SET_SEND_DUTY_CYCLE:
		dprintk("SET_SEND_DUTY_CYCLE\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value <= 0 || value > 100)
			return -EINVAL;
		return init_timing_params(value, freq);
		break;

	case LIRC_SET_SEND_CARRIER:
		dprintk("SET_SEND_CARRIER\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value > 500000 || value < 20000)
			return -EINVAL;
		return init_timing_params(duty_cycle, value);
		break;

	default:
		return lirc_dev_fop_ioctl(filep, cmd, arg);
	}
	return 0;
}

static const struct file_operations lirc_fops = {
	.owner		= THIS_MODULE,
	.write		= lirc_write,
	.unlocked_ioctl	= lirc_ioctl,
	.read		= lirc_dev_fop_read,
	.poll		= lirc_dev_fop_poll,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= no_llseek,
};

static struct lirc_driver driver = {
	.name		= LIRC_DRIVER_NAME,
	.minor		= -1,
	.code_length	= 1,
	.sample_rate	= 0,
	.data		= NULL,
	.add_to_buf	= NULL,
	.rbuf		= &rbuf,
	.set_use_inc	= set_use_inc,
	.set_use_dec	= set_use_dec,
	.fops		= &lirc_fops,
	.dev		= NULL,
	.owner		= THIS_MODULE,
};

static struct platform_driver lirc_sam_driver = {
	.driver = {
		.name   = LIRC_DRIVER_NAME,
		.owner  = THIS_MODULE,
	},
};

static int __init lirc_sam_init(void)
{
	int result;

	/* Init read buffer. */
	result = lirc_buffer_init(&rbuf, sizeof(int), RBUF_LEN);
	if (result < 0)
		return -ENOMEM;

	result = platform_driver_register(&lirc_sam_driver);
	if (result) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": lirc register returned %d\n", result);
		goto exit_buffer_free;
	}

	lirc_sam_dev = platform_device_alloc(LIRC_DRIVER_NAME, 0);
	if (!lirc_sam_dev) {
		result = -ENOMEM;
		goto exit_driver_unregister;
	}

	result = platform_device_add(lirc_sam_dev);
	if (result)
		goto exit_device_put;

	return 0;

	exit_device_put:
	platform_device_put(lirc_sam_dev);

	exit_driver_unregister:
	platform_driver_unregister(&lirc_sam_driver);

	exit_buffer_free:
	lirc_buffer_free(&rbuf);

	return result;
}

static void lirc_sam_exit(void)
{
	gpio_free(gpio_out_pin);
	gpio_free(gpio_in_pin);
	platform_device_unregister(lirc_sam_dev);
	platform_driver_unregister(&lirc_sam_driver);
	lirc_buffer_free(&rbuf);
}

static int __init lirc_sam_init_module(void)
{
	int result, i;

	result = lirc_sam_init();
	if (result)
		return result;

	/* check if the module received valid gpio pin numbers */
	result = 0;
	if (gpio_in_pin != gpio_out_pin) {
		for(i = 0; (i < ARRAY_SIZE(valid_gpio_pins)) && (result != 2); i++) {
			if (gpio_in_pin == valid_gpio_pins[i] ||
			   gpio_out_pin == valid_gpio_pins[i]) {
				result++;
			}
		}
	}

	if (result != 2) {
		result = -EINVAL;
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": invalid GPIO pin(s) specified!\n");
		goto exit_sam;
	}

	driver.features = LIRC_CAN_SET_SEND_DUTY_CYCLE |
			  LIRC_CAN_SET_SEND_CARRIER |
			  LIRC_CAN_SEND_PULSE |
			  LIRC_CAN_REC_MODE2;

	driver.dev = &lirc_sam_dev->dev;
	driver.minor = lirc_register_driver(&driver);

	if (driver.minor < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": device registration failed with %d\n", result);
		result = -EIO;
		goto exit_sam;
	}

	printk(KERN_INFO LIRC_DRIVER_NAME ": driver registered!\n");

	result = init_port();
	if (result < 0)
		goto exit_sam;

	return 0;

	exit_sam:
	lirc_sam_exit();

	return result;
}

static void __exit lirc_sam_exit_module(void)
{
	lirc_sam_exit();

	lirc_unregister_driver(driver.minor);
	printk(KERN_INFO LIRC_DRIVER_NAME ": cleaned up module\n");
}

module_init(lirc_sam_init_module);
module_exit(lirc_sam_exit_module);

MODULE_DESCRIPTION("Infra-red receiver and blaster driver for Arietta G25 GPIO.");
MODULE_AUTHOR("Hai Peng Yu <yuhp@iwave.cc>");
MODULE_LICENSE("GPL");

module_param(gpio_out_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_out_pin, "GPIO output/transmitter pin number of the AT91SAMG25"
		 " processor. Valid pin numbers are: 21 ,22 ,23 ,43 ,44 ,45 ,46 ,66 ,67 ,68 ,95"
		 " default 44");

module_param(gpio_in_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_in_pin, "GPIO input pin number of the AT91SAMG25"
		 " processor. Valid pin numbers are: 21 ,22 ,23 ,43 ,44 ,45 ,46 ,66 ,67 ,68 ,95"
		 " default 43");

module_param(sense, bool, S_IRUGO);
MODULE_PARM_DESC(sense, "Override autodetection of IR receiver circuit"
		 " (0 = active high, 1 = active low )");

module_param(softcarrier, bool, S_IRUGO);
MODULE_PARM_DESC(softcarrier, "Software carrier (0 = off, 1 = on, default on)");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Enable debugging messages");
