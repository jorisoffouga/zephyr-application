#include <zephyr.h>
#include <misc/printk.h>
#include <misc/util.h>
#include <device.h>
#include <gpio.h>

#define GPIO_DRV_NAME CONFIG_GPIO_IMX_PORT_7_NAME
#define GPIO_MIKROBUS_INT_PIN 7
#define GPIO_KEY_PIN 1
#define EDGE    (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)
#define PULL_UP 0

static struct gpio_callback gpio_cb;

void gpio_callback(struct device *port,
		   struct gpio_callback *cb, u32_t pins)
{
    int ret;
    static int pin_state = 0;

    printk("Button pressed at %d\n", k_cycle_get_32());
    
    if(pin_state == 0){
        pin_state = 1;
        printk("Led On\n");
    }else{
        pin_state = 0;
        printk("Led Off\n");
    }

    ret = gpio_pin_write(port, GPIO_MIKROBUS_INT_PIN, pin_state);

    if(ret) {
        printk("Error set " GPIO_DRV_NAME "%d!\n", GPIO_MIKROBUS_INT_PIN);
    }
    

}

void main(void)
{
    struct device *gpio_dev;
    int ret;

    gpio_dev = device_get_binding(GPIO_DRV_NAME);

    if(!gpio_dev){
        printk("Cannot find %s!\n", GPIO_DRV_NAME);
		return;
    }

    ret = gpio_pin_configure(gpio_dev, GPIO_MIKROBUS_INT_PIN, (GPIO_DIR_OUT));
	
    if (ret) {
		printk("Error configuring " GPIO_DRV_NAME "%d!\n", GPIO_MIKROBUS_INT_PIN);
	}

    ret = gpio_pin_configure(gpio_dev,GPIO_KEY_PIN, GPIO_DIR_IN 
                        | GPIO_INT |  PULL_UP | EDGE);
    if (ret) {
		printk("Error configuring " GPIO_DRV_NAME "%d!\n", GPIO_KEY_PIN);
	}

    gpio_init_callback(&gpio_cb, gpio_callback, BIT(GPIO_KEY_PIN));

	ret = gpio_add_callback(gpio_dev, &gpio_cb);

	if (ret) {
		printk("Cannot setup callback!\n");
	}

	ret = gpio_pin_enable_callback(gpio_dev, GPIO_KEY_PIN);

	if (ret) {
		printk("Error enabling callback!\n");                
    
    }

    printk("Started Simple Application Zephyr\n");
    printk("Press The user button for turn on or turn off led\n");

    while(1) {
        u32_t val =0;
        
        gpio_pin_read(gpio_dev,GPIO_KEY_PIN,&val);
        k_sleep(500);

    }

}