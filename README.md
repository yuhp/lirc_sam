#LIRC GPIO Driver for homebrew adapter on AT91SAM9G25

lirc_sam - Device driver that records pulse- and pause-lengths(space-lengths) between GPIO interrupt events on the AT91SAM9G25.

This driver impletements the lirc interface like the lirc_serial driver.

Lots of code has been taken from the lirc_rpi module, so I would like say thanks to the [authors](http://aron.ws).

This driver has been tested on the [Arietta G25 and Aria G25 Boards](http://www.acmesystems.it/) with 3.1.6 and 4.X linux kernal.

The input&output pins could be selected among gpio pins 21 ,22 ,23 ,43 ,44 ,45 ,46 ,66 ,67 ,68 ,95 which marked as kernel ID. The gpio pins 43 ,44 ,45 ,46 are recommand to use.

The default configuration:
GPIO 43 is default IR input pin.
GPIO 44 is default IR output pin.

#Recompile the kernel:
**Copy the files to the linux kernel source code directory:**
>cp lirc_sam.c $SOURCE_PATH/driver/staging/media/lirc/  
>cp Makefile $SOURCE_PATH/driver/staging/media/lirc/  
>cp Kconfig $SOURCE_PATH/driver/staging/media/lirc/  

**Add the following to .config or enable in menuconfig:**
>CONFIG_MEDIA_RC_SUPPORT=y
>CONFIG_MEDIA_SUPPORT=y
>CONFIG_RC_DECODERS=y
>CONFIG_RC_CORE=m  
>CONFIG_LIRC=m  
>CONFIG_RC_MAP=m  
>CONFIG_IR_NEC_DECODER=m  
>CONFIG_IR_RC5_DECODER=m  
>CONFIG_IR_RC6_DECODER=m  
>CONFIG_IR_JVC_DECODER=m  
>CONFIG_IR_SONY_DECODER=m  
>CONFIG_IR_RC5_SZ_DECODER=m  
>CONFIG_IR_MCE_KBD_DECODER=m  
>CONFIG_IR_LIRC_CODEC=m  
>CONFIG_STAGING=y  
>CONFIG_STAGING_MEDIA=y  
>CONFIG_LIRC_STAGING=y  
>CONFIG_LIRC_SAM=m  

#Loading the driver:

The driver has 5 parameters(same as the lirc driver of raspberry pi): debug, gpio_out_pin, gpio_in_pin, sense, softcarrier.
The default gpio input pin 43(PB11) is used when no input pin is specified as a parameter. The default gpio output pin for transmission is pin 44(PB12). 

**Example:**
modprobe lirc_sam gpio_in_pin=22 gpio_out_pin=23  
The driver will use GPIO pin 22(PA22) as input, GPIO pin 23(PA23) as output.
