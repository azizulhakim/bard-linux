/*
 * udlfb.c -- Framebuffer driver for DisplayLink USB controller
 *
 * Copyright (C) 2009 Roberto De Ioris <roberto@unbit.it>
 * Copyright (C) 2009 Jaya Kumar <jayakumar.lkml@gmail.com>
 * Copyright (C) 2009 Bernie Thompson <bernie@plugable.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Layout is based on skeletonfb by James Simmons and Geert Uytterhoeven,
 * usb-skeleton by GregKH.
 *
 * Device-specific portions based on information from Displaylink, with work
 * from Florian Echtler, Henrik Bjerregaard Pedersen, and others.
 */

#define pr_fmt(fmt) "udlfb: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include <linux/delay.h>
#include <linux/version.h> /* many users build as module against old kernels*/
#include "udlfb.h"
#include "devices.h"

// A temp var just to see how many times hline_render is being called.
int vline_count = 0;

unsigned char sony_sdmhs53_edid[] = {0x00, 0xff, 0xff,0xff,
					0xff,0xff,0xff,0x00,0x4d,0xd9,0x50,0x22,
					0x01,0x01,0x01,0x01,0x0b,0x0e,0x01,0x03,
					0x0c,0x1e,0x17,0x78,0xea,0x8c,0x3e,0xa4,
					0x58,0x4d,0x91,0x24,0x15,0x4f,0x51,0xa1,
					0x08,0x00,0x01,0x01,0x01,0x01,0x01,0x01,
					0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
					0x01,0x01,0x64,0x19,0x00,0x40,0x41,0x00,
					0x26,0x30,0x18,0x88,0x36,0x00,0x30,0xe4,
					0x10,0x00,0x00,0x18,0x00,0x00,0x00,0xfd,
					0x00,0x39,0x3f,0x1c,0x31,0x09,0x00,0x0a,
					0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,
					0x00,0xfc,0x00,0x53,0x44,0x4d,0x2d,0x48,
					0x53,0x35,0x33,0x0a,0x20,0x20,0x20,0x20,
					0x00,0x00,0x00,0xff,0x00,0x31,0x32,0x35,
					0x33,0x37,0x30,0x36,0x0a,0x20,0x20,0x20,
					0x20,0x20,0x00,0xce,
};

static struct fb_fix_screeninfo dlfb_fix = {
	.id =           "udlfb",
	.type =         FB_TYPE_PACKED_PIXELS,
	.visual =       FB_VISUAL_TRUECOLOR,
	.xpanstep =     0,
	.ypanstep =     0,
	.ywrapstep =    0,
	.accel =        FB_ACCEL_NONE,
};

static const u32 udlfb_info_flags = FBINFO_DEFAULT | FBINFO_READS_FAST |
#ifdef FBINFO_VIRTFB
		FBINFO_VIRTFB |
#endif
		FBINFO_HWACCEL_IMAGEBLIT | FBINFO_HWACCEL_FILLRECT |
		FBINFO_HWACCEL_COPYAREA | FBINFO_MISC_ALWAYS_SETPAR;

/*
 * There are many DisplayLink-based graphics products, all with unique PIDs.
 * So we match on DisplayLink's VID + Vendor-Defined Interface Class (0xff)
 * We also require a match on SubClass (0x00) and Protocol (0x00),
 * which is compatible with all known USB 2.0 era graphics chips and firmware,
 * but allows DisplayLink to increment those for any future incompatible chips
 */
static struct usb_device_id id_table[] = {
	{.idVendor = 0x17e9,
	 .bInterfaceClass = 0xff,
	 .bInterfaceSubClass = 0x00,
	 .bInterfaceProtocol = 0x00,
	 .match_flags = USB_DEVICE_ID_MATCH_VENDOR |
		USB_DEVICE_ID_MATCH_INT_CLASS |
		USB_DEVICE_ID_MATCH_INT_SUBCLASS |
		USB_DEVICE_ID_MATCH_INT_PROTOCOL,
	},
	{ USB_DEVICE_AND_INTERFACE_INFO(VID1, PID1, CL1, SC1, PR1) },
	{ USB_DEVICE_AND_INTERFACE_INFO(VID1, 0x2d00, CL1, SC1, PR1) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

/* module options */
static bool console = 1; /* Allow fbcon to open framebuffer */
static bool fb_defio = 1;  /* Detect mmap writes using page faults */
static bool shadow = 1; /* Optionally disable shadow framebuffer */
static int pixel_limit; /* Optionally force a pixel resolution limit */

/*
 * When building as a separate module against an arbitrary kernel,
 * check on build presence of other kernel modules we have dependencies on.
 * In some cases we can't build at all without the dependency.
 * For others, we can build without them, but lose functionality.
 * When rebuilding entire kernel, our Kconfig should pull in everything we need.
 */

#ifndef CONFIG_FB_DEFERRED_IO
#warning CONFIG_FB_DEFERRED_IO kernel support required for fb_defio mmap support
#endif

#ifndef CONFIG_FB_SYS_IMAGEBLIT
#warning CONFIG_FB_SYS_IMAGEBLIT kernel support required for fb console
#endif

#ifndef CONFIG_FB_SYS_FOPS
#warning FB_SYS_FOPS kernel support required for filesystem char dev access
#endif

#ifndef CONFIG_FB_MODE_HELPERS
#warning CONFIG_FB_MODE_HELPERS required. Expect build break
#endif

/* dlfb keeps a list of urbs for efficient bulk transfers */
static void dlfb_urb_completion(struct urb *urb);
static struct urb *dlfb_get_urb(struct dlfb_data *dev);
static int dlfb_submit_urb(struct dlfb_data *dev, struct urb * urb, size_t len);
static int dlfb_alloc_urb_list(struct dlfb_data *dev, int count, size_t size);
static void dlfb_free_urb_list(struct dlfb_data *dev);


/* Function added by me to fix make errors */
static void err (char *msg){
	printk(msg);
}

/*
 * All DisplayLink bulk operations start with 0xAF, followed by specific code
 * All operations are written to buffers which then later get sent to device
 */
static char *dlfb_set_register(char *buf, u8 reg, u8 val)
{
	printk("dlfb_set_register called\n");
	*buf++ = reg;
	*buf++ = val;
	return buf;
}

static char *dlfb_vidreg_lock(char *buf)
{
	printk("dlfb_vidreg_lock called \n");
	return dlfb_set_register(buf, 0xFF, 0x00);
}

static char *dlfb_vidreg_unlock(char *buf)
{
	printk("dlfb_vidreg_unlock called \n");
	return dlfb_set_register(buf, 0xFF, 0xFF);
}

/*
 * Map FB_BLANK_* to DisplayLink register
 * DLReg FB_BLANK_*
 * ----- -----------------------------
 *  0x00 FB_BLANK_UNBLANK (0)
 *  0x01 FB_BLANK (1)
 *  0x03 FB_BLANK_VSYNC_SUSPEND (2)
 *  0x05 FB_BLANK_HSYNC_SUSPEND (3)
 *  0x07 FB_BLANK_POWERDOWN (4) Note: requires modeset to come back
 */
static char *dlfb_blanking(char *buf, int fb_blank)
{
	u8 reg;
	
	printk("dlfb_blanking called \n");

	switch (fb_blank) {
	case FB_BLANK_POWERDOWN:
		reg = 0x07;
		break;
	case FB_BLANK_HSYNC_SUSPEND:
		reg = 0x05;
		break;
	case FB_BLANK_VSYNC_SUSPEND:
		reg = 0x03;
		break;
	case FB_BLANK_NORMAL:
		reg = 0x01;
		break;
	default:
		reg = 0x00;
	}

	buf = dlfb_set_register(buf, 0x1F, reg);

	return buf;
}

static char *dlfb_set_color_depth(char *buf, u8 selection)
{
	printk("dlfb_set_color_depth called\n");
	return dlfb_set_register(buf, 0x00, selection);
}

static char *dlfb_set_base16bpp(char *wrptr, u32 base)
{
	printk("dlfb_set_base16bpp called\n");
	/* the base pointer is 16 bits wide, 0x20 is hi byte. */
	wrptr = dlfb_set_register(wrptr, 0x20, base >> 16);
	wrptr = dlfb_set_register(wrptr, 0x21, base >> 8);
	return dlfb_set_register(wrptr, 0x22, base);
}

/*
 * DisplayLink HW has separate 16bpp and 8bpp framebuffers.
 * In 24bpp modes, the low 323 RGB bits go in the 8bpp framebuffer
 */
static char *dlfb_set_base8bpp(char *wrptr, u32 base)
{
	printk("dlfb_set_base8bpp called\n");
	wrptr = dlfb_set_register(wrptr, 0x26, base >> 16);
	wrptr = dlfb_set_register(wrptr, 0x27, base >> 8);
	return dlfb_set_register(wrptr, 0x28, base);
}

static char *dlfb_set_register_16(char *wrptr, u8 reg, u16 value)
{
	printk("dlfb_set_register_16 called\n");
	wrptr = dlfb_set_register(wrptr, reg, value >> 8);
	return dlfb_set_register(wrptr, reg+1, value);
}

/*
 * This is kind of weird because the controller takes some
 * register values in a different byte order than other registers.
 */
static char *dlfb_set_register_16be(char *wrptr, u8 reg, u16 value)
{
	printk("dlfb_set_register_16be called\n");
	wrptr = dlfb_set_register(wrptr, reg, value);
	return dlfb_set_register(wrptr, reg+1, value >> 8);
}

/*
 * LFSR is linear feedback shift register. The reason we have this is
 * because the display controller needs to minimize the clock depth of
 * various counters used in the display path. So this code reverses the
 * provided value into the lfsr16 value by counting backwards to get
 * the value that needs to be set in the hardware comparator to get the
 * same actual count. This makes sense once you read above a couple of
 * times and think about it from a hardware perspective.
 */
static u16 dlfb_lfsr16(u16 actual_count)
{
	u32 lv = 0xFFFF; /* This is the lfsr value that the hw starts with */
	printk("dlfb_lfsr16 called\n");

	while (actual_count--) {
		lv =	 ((lv << 1) |
			(((lv >> 15) ^ (lv >> 4) ^ (lv >> 2) ^ (lv >> 1)) & 1))
			& 0xFFFF;
	}

	return (u16) lv;
}

/*
 * This does LFSR conversion on the value that is to be written.
 * See LFSR explanation above for more detail.
 */
static char *dlfb_set_register_lfsr16(char *wrptr, u8 reg, u16 value)
{
	printk("dlfb_set_register_lfsr16 called\n");
	return dlfb_set_register_16(wrptr, reg, dlfb_lfsr16(value));
}

/*
 * This takes a standard fbdev screeninfo struct and all of its monitor mode
 * details and converts them into the DisplayLink equivalent register commands.
 */
static char *dlfb_set_vid_cmds(char *wrptr, struct fb_var_screeninfo *var)
{
	u16 xds, yds;
	u16 xde, yde;
	u16 yec;
	
	printk("dlfb_set_vid_cmds called\n");

	/* x display start */
	xds = var->left_margin + var->hsync_len;
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x01, xds);
	/* x display end */
	xde = xds + var->xres;
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x03, xde);

	/* y display start */
	yds = var->upper_margin + var->vsync_len;
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x05, yds);
	/* y display end */
	yde = yds + var->yres;
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x07, yde);

	/* x end count is active + blanking - 1 */
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x09,
			xde + var->right_margin - 1);

	/* libdlo hardcodes hsync start to 1 */
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x0B, 1);

	/* hsync end is width of sync pulse + 1 */
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x0D, var->hsync_len + 1);

	/* hpixels is active pixels */
	wrptr = dlfb_set_register_16(wrptr, 0x0F, var->xres);

	/* yendcount is vertical active + vertical blanking */
	yec = var->yres + var->upper_margin + var->lower_margin +
			var->vsync_len;
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x11, yec);

	/* libdlo hardcodes vsync start to 0 */
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x13, 0);

	/* vsync end is width of vsync pulse */
	wrptr = dlfb_set_register_lfsr16(wrptr, 0x15, var->vsync_len);

	/* vpixels is active pixels */
	wrptr = dlfb_set_register_16(wrptr, 0x17, var->yres);

	/* convert picoseconds to 5kHz multiple for pclk5k = x * 1E12/5k */
	wrptr = dlfb_set_register_16be(wrptr, 0x1B,
			200*1000*1000/var->pixclock);

	return wrptr;
}

/*
 * This takes a standard fbdev screeninfo struct that was fetched or prepared
 * and then generates the appropriate command sequence that then drives the
 * display controller.
 */
static int dlfb_set_video_mode(struct dlfb_data *dev,
				struct fb_var_screeninfo *var)
{
	char *buf;
	char *wrptr;
	int retval = 0;
	int writesize;
	struct urb *urb;
	
	printk("dlfb_set_video_mode called\n");

	if (!atomic_read(&dev->video.usb_active))
		return -EPERM;

	urb = dlfb_get_urb(dev);
	if (!urb)
		return -ENOMEM;

	buf = (char *) urb->transfer_buffer;

	/*
	* This first section has to do with setting the base address on the
	* controller * associated with the display. There are 2 base
	* pointers, currently, we only * use the 16 bpp segment.
	*/
	wrptr = dlfb_vidreg_lock(buf);
	wrptr = dlfb_set_color_depth(wrptr, 0x00);
	/* set base for 16bpp segment to 0 */
	wrptr = dlfb_set_base16bpp(wrptr, 0);
	/* set base for 8bpp segment to end of fb */
	wrptr = dlfb_set_base8bpp(wrptr, dev->video.info->fix.smem_len);

	wrptr = dlfb_set_vid_cmds(wrptr, var);
	wrptr = dlfb_blanking(wrptr, FB_BLANK_UNBLANK);
	wrptr = dlfb_vidreg_unlock(wrptr);

	writesize = wrptr - buf;

	
	printk("Writesize in video mode set: %d\n", writesize);
	/* 
	 * This accounts for 72 Bytes
	 * Removing all usb transfers besides the frame data.
	 * Transfer removal is handled in the submit_urb function
	 * -TODO- Ensure the driver without such restriction
	 */
	retval = dlfb_submit_urb(dev, urb, writesize);

	dev->video.blank_mode = FB_BLANK_UNBLANK;

	return retval;
}

static int dlfb_ops_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page, pos;
	
	printk("dlfb_ops_mmap called\n");

	if (offset + size > info->fix.smem_len)
		return -EINVAL;

	pos = (unsigned long)info->fix.smem_start + offset;

	pr_notice("mmap() framebuffer addr:%lu size:%lu\n",
		  pos, size);

	while (size > 0) {
		page = vmalloc_to_pfn((void *)pos);
		if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED))
			return -EAGAIN;

		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	vma->vm_flags |= VM_RESERVED;	/* avoid to swap out this VMA */
	return 0;
}

/*
 * There are 3 copies of every pixel: The front buffer that the fbdev
 * client renders to, the actual framebuffer across the USB bus in hardware
 * (that we can only write to, slowly, and can never read), and (optionally)
 * our shadow copy that tracks what's been sent to that hardware buffer.
 */
static int dlfb_render_hline(struct dlfb_data *dev, struct urb **urb_ptr,
			      const char *front, char **urb_buf_ptr,
			      u32 byte_offset, u32 byte_width,
			      int *ident_ptr, int *sent_ptr)
{				  
	const u8 *line_start, *line_end, *next_pixel;
	u32 dev_addr = dev->video.base16 + byte_offset;
	
	// For page y-index encoding
	u8 *data;
	u16 page_index = byte_offset/4096;
	
	data = kmalloc((2 + byte_width), GFP_KERNEL);
	
	if(data){
		// Save page index
		*data = page_index;
		*(data+1) = page_index >> 8;
	}
	
	int transferred = 0;
	int retval;

	line_start = (u8 *) (front + byte_offset);
	next_pixel = line_start;
	line_end = next_pixel + byte_width;
	
	// Copy current page
	memcpy(data + 2, line_start, byte_width);
	
	vline_count++;
	
	// identical pixels value to zero.
	ident_ptr += 0;
	
	/* -TODO- 
	 * -Remove hardcoded bulk-out address
	 * -can prefectch_line() be of some perf. boostr here? Check.
	 */
	retval = usb_bulk_msg(dev->usbdev,
	      usb_sndbulkpipe(dev->usbdev, dev->bulk_out_endpointAddr),
	      data, byte_width + 2, &transferred, HZ*5);
		      
	sent_ptr = transferred;
		      
	printk("Return: %d transferred: %d \n", retval, transferred);
	
	if(data)
		kfree(data);
	
	return 0;
}

int dlfb_handle_damage(struct dlfb_data *dev, int x, int y,
	       int width, int height, char *data)
{
	int i, ret;
	char *cmd;
	cycles_t start_cycles, end_cycles;
	int bytes_sent = 0;
	int bytes_identical = 0;
	struct urb *urb;
	int aligned_x;
	
	printk("dlfb_handle_damage called\n");
	
	printk("handle damage x: %d, y:%d, width:%d, height:%d\n", x, y, width, height);
	
	/* -TODO- 
	 * -Remove this BB specific issue code
	 * Exit if width and height are something else.
	 */
	if((width - x) != 1024 || (height - y) != 768){
		printk("Dim. mismatch. Not sending\n");
		return 0; 
	}

	start_cycles = get_cycles();

	aligned_x = DL_ALIGN_DOWN(x, sizeof(unsigned long));
	width = DL_ALIGN_UP(width + (x-aligned_x), sizeof(unsigned long));
	x = aligned_x;

	if ((width <= 0) ||
	    (x + width > dev->video.info->var.xres) ||
	    (y + height > dev->video.info->var.yres))
		return -EINVAL;

	if (!atomic_read(&dev->video.usb_active))
		return 0;

	/* Modified: 
	 * From 1 line per transfer to 2 hlines per transfer
	 * Now all USB transfers would be of same length - 4096
	 */
	for (i = y; i < y + height ; i++) {
		const int line_offset = dev->video.info->fix.line_length * i;
		const int byte_offset = line_offset + (x * BPP);

		if (dlfb_render_hline(dev, &urb,
				      (char *) dev->video.info->fix.smem_start,
				      &cmd, byte_offset, width * BPP * 2,
				      &bytes_identical, &bytes_sent))
			goto error;
		i++;
	}

error:
	atomic_add(bytes_sent, &dev->video.bytes_sent);
	atomic_add(bytes_identical, &dev->video.bytes_identical);
	atomic_add(width*height*2, &dev->video.bytes_rendered);
	end_cycles = get_cycles();
	atomic_add(((unsigned int) ((end_cycles - start_cycles)
		    >> 10)), /* Kcycles */
		   &dev->video.cpu_kcycles_used);

	return 0;
}

static ssize_t dlfb_ops_read(struct fb_info *info, char __user *buf,
			 size_t count, loff_t *ppos)
{
	ssize_t result = -ENOSYS;
	
	printk("dlfb_ops_read called\n");

#if defined CONFIG_FB_SYS_FOPS
	result = fb_sys_read(info, buf, count, ppos);
#endif

	return result;
}

/*
 * Path triggered by usermode clients who write to filesystem
 * e.g. cat filename > /dev/fb1
 * Not used by X Windows or text-mode console. But useful for testing.
 * Slow because of extra copy and we must assume all pixels dirty.
 */
static ssize_t dlfb_ops_write(struct fb_info *info, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	ssize_t result = -ENOSYS;
	
	printk("dlfb_ops_write called\n");

#if defined CONFIG_FB_SYS_FOPS

	struct dlfb_data *dev = info->par;
	u32 offset = (u32) *ppos;

	result = fb_sys_write(info, buf, count, ppos);

	if (result > 0) {
		int start = max((int)(offset / info->fix.line_length) - 1, 0);
		int lines = min((u32)((result / info->fix.line_length) + 1),
				(u32)info->var.yres);

		dlfb_handle_damage(dev, 0, start, info->var.xres,
			lines, info->screen_base);
	}

#endif
	return result;
}

/* hardware has native COPY command (see libdlo), but not worth it for fbcon */
static void dlfb_ops_copyarea(struct fb_info *info,
				const struct fb_copyarea *area)
{
	printk("dlfb_ops_copyarea called\n");
	
#if defined CONFIG_FB_SYS_COPYAREA

	struct dlfb_data *dev = info->par;

	sys_copyarea(info, area);

	dlfb_handle_damage(dev, area->dx, area->dy,
			area->width, area->height, info->screen_base);
#endif

}

static void dlfb_ops_imageblit(struct fb_info *info,
				const struct fb_image *image)
{
	printk("dlfb_ops_imageblit called\n");
	
#if defined CONFIG_FB_SYS_IMAGEBLIT

	struct dlfb_data *dev = info->par;

	sys_imageblit(info, image);

	dlfb_handle_damage(dev, image->dx, image->dy,
			image->width, image->height, info->screen_base);

#endif

}

static void dlfb_ops_fillrect(struct fb_info *info,
			  const struct fb_fillrect *rect)
{
	printk("dlfb_ops_fillrect called\n");
	
#if defined CONFIG_FB_SYS_FILLRECT

	struct dlfb_data *dev = info->par;

	sys_fillrect(info, rect);

	dlfb_handle_damage(dev, rect->dx, rect->dy, rect->width,
			      rect->height, info->screen_base);
#endif

}

#ifdef CONFIG_FB_DEFERRED_IO
/*
 * NOTE: fb_defio.c is holding info->fbdefio.mutex
 *   Touching ANY framebuffer memory that triggers a page fault
 *   in fb_defio will cause a deadlock, when it also tries to
 *   grab the same mutex.
 */
 
static void dlfb_dpy_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	struct page *cur;
	struct fb_deferred_io *fbdefio = info->fbdefio;
	struct dlfb_data *dev = info->par;
	struct urb *urb;
	char *cmd;
	cycles_t start_cycles, end_cycles;
	int bytes_sent = 0;
	int bytes_identical = 0;
	int bytes_rendered = 0;
	
	printk("A deferred io call occured\n");

	if (!fb_defio)
		return;

	if (!atomic_read(&dev->video.usb_active))
		return;

	start_cycles = get_cycles();

	/* walk the written page list and render each to device */
	list_for_each_entry(cur, &fbdefio->pagelist, lru) {

		if (dlfb_render_hline(dev, &urb, (char *) info->fix.smem_start,
				  &cmd, cur->index << PAGE_SHIFT,
				  PAGE_SIZE, &bytes_identical, &bytes_sent))
			goto error;
		bytes_rendered += PAGE_SIZE;
	}

error:
	atomic_add(bytes_sent, &dev->video.bytes_sent);
	atomic_add(bytes_identical, &dev->video.bytes_identical);
	atomic_add(bytes_rendered, &dev->video.bytes_rendered);
	end_cycles = get_cycles();
	atomic_add(((unsigned int) ((end_cycles - start_cycles)
		    >> 10)), /* Kcycles */
		   &dev->video.cpu_kcycles_used);
}

#endif

static int dlfb_ops_ioctl(struct fb_info *info, unsigned int cmd,
				unsigned long arg)
{

	struct dlfb_data *dev = info->par;
	
	printk("dlfb_ops_ioctl called\n");

	if (!atomic_read(&dev->video.usb_active))
		return 0;

	/* TODO: Update X server to get this from sysfs instead */
	if (cmd == DLFB_IOCTL_RETURN_EDID) {
		void __user *edid = (void __user *)arg;
		if (copy_to_user(edid, dev->video.edid, dev->video.edid_size))
			return -EFAULT;
		return 0;
	}

	/* TODO: Help propose a standard fb.h ioctl to report mmap damage */
	if (cmd == DLFB_IOCTL_REPORT_DAMAGE) {
		struct dloarea area;

		if (copy_from_user(&area, (void __user *)arg,
				  sizeof(struct dloarea)))
			return -EFAULT;

		/*
		 * If we have a damage-aware client, turn fb_defio "off"
		 * To avoid perf imact of unnecessary page fault handling.
		 * Done by resetting the delay for this fb_info to a very
		 * long period. Pages will become writable and stay that way.
		 * Reset to normal value when all clients have closed this fb.
		 */
#ifdef CONFIG_FB_DEFERRED_IO
		if (info->fbdefio)
			info->fbdefio->delay = DL_DEFIO_WRITE_DISABLE;
#endif
		if (area.x < 0)
			area.x = 0;

		if (area.x > info->var.xres)
			area.x = info->var.xres;

		if (area.y < 0)
			area.y = 0;

		if (area.y > info->var.yres)
			area.y = info->var.yres;

		dlfb_handle_damage(dev, area.x, area.y, area.w, area.h,
			   info->screen_base);
	}

	return 0;
}

/* taken from vesafb */
static int
dlfb_ops_setcolreg(unsigned regno, unsigned red, unsigned green,
	       unsigned blue, unsigned transp, struct fb_info *info)
{
	int err = 0;

	printk("dlfb_ops_setcolreg called\n");	
	
	if (regno >= info->cmap.len)
		return 1;

	if (regno < 16) {
		if (info->var.red.offset == 10) {
			/* 1:5:5:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800) >> 1) |
			    ((green & 0xf800) >> 6) | ((blue & 0xf800) >> 11);
		} else {
			/* 0:5:6:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800)) |
			    ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
		}
	}

	return err;
}

/*
 * It's common for several clients to have framebuffer open simultaneously.
 * e.g. both fbcon and X. Makes things interesting.
 * Assumes caller is holding info->lock (for open and release at least)
 */
static int dlfb_ops_open(struct fb_info *info, int user)
{
	struct dlfb_data *dev = info->par;
	
	printk("dlfb_ops_open called\n");

	/*
	 * fbcon aggressively connects to first framebuffer it finds,
	 * preventing other clients (X) from working properly. Usually
	 * not what the user wants. Fail by default with option to enable.
	 */
	if ((user == 0) && (!console))
		return -EBUSY;

	/* If the USB device is gone, we don't accept new opens */
	if (dev->video.virtualized)
		return -ENODEV;

	dev->video.fb_count++;

	kref_get(&dev->kref);

#ifdef CONFIG_FB_DEFERRED_IO
	if (fb_defio && (info->fbdefio == NULL)) {
		/* enable defio at last moment if not disabled by client */

		struct fb_deferred_io *fbdefio;

		fbdefio = kmalloc(sizeof(struct fb_deferred_io), GFP_KERNEL);

		if (fbdefio) {
			fbdefio->delay = DL_DEFIO_WRITE_DELAY;
			fbdefio->deferred_io = dlfb_dpy_deferred_io;
		}

		info->fbdefio = fbdefio;
		fb_deferred_io_init(info);
	}
#endif

	pr_notice("open /dev/fb%d user=%d fb_info=%p count=%d\n",
	    info->node, user, info, dev->video.fb_count);

	return 0;
}

/*
 * Called when all client interfaces to start transactions have been disabled,
 * and all references to our device instance (dlfb_data) are released.
 * Every transaction must have a reference, so we know are fully spun down
 */
static void dlfb_free(struct kref *kref)
{
	struct dlfb_data *dev = container_of(kref, struct dlfb_data, kref);
	
	printk("dlfb_free called\n");

	if (dev->video.backing_buffer)
		vfree(dev->video.backing_buffer);

	kfree(dev->video.edid);

	pr_warn("freeing dlfb_data %p\n", dev);

	kfree(dev);
}

static void dlfb_release_urb_work(struct work_struct *work)
{
	struct urb_node *unode = container_of(work, struct urb_node,
					      release_urb_work.work);
					      
	printk("dlfb_release_urb_work called\n");

	up(&unode->dev->video.urbs.limit_sem);
}

static void dlfb_free_framebuffer(struct dlfb_data *dev)
{
	struct fb_info *info = dev->video.info;
	
	printk("dlfb_free_framebuffer called\n");

	if (info) {
		int node = info->node;

		unregister_framebuffer(info);

		if (info->cmap.len != 0)
			fb_dealloc_cmap(&info->cmap);
		if (info->monspecs.modedb)
			fb_destroy_modedb(info->monspecs.modedb);
		if (info->screen_base)
			vfree(info->screen_base);

		fb_destroy_modelist(&info->modelist);

		dev->video.info = NULL;

		/* Assume info structure is freed after this point */
		framebuffer_release(info);

		pr_warn("fb_info for /dev/fb%d has been freed\n", node);
	}

	/* ref taken in probe() as part of registering framebfufer */
	kref_put(&dev->kref, dlfb_free);
}

static void dlfb_free_framebuffer_work(struct work_struct *work)
{
	struct dlfb_data *dev = container_of(work, struct dlfb_data,
					     free_framebuffer_work.work);
					     
	printk("dlfb_free_framebuffer_work called\n");
	
	dlfb_free_framebuffer(dev);
}
/*
 * Assumes caller is holding info->lock mutex (for open and release at least)
 */
static int dlfb_ops_release(struct fb_info *info, int user)
{
	struct dlfb_data *dev = info->par;

	printk("dlfb_ops_release called\n");	
	
	dev->video.fb_count--;

	/* We can't free fb_info here - fbmem will touch it when we return */
	if (dev->video.virtualized && (dev->video.fb_count == 0))
		schedule_delayed_work(&dev->free_framebuffer_work, HZ);

#ifdef CONFIG_FB_DEFERRED_IO
	if ((dev->video.fb_count == 0) && (info->fbdefio)) {
		fb_deferred_io_cleanup(info);
		kfree(info->fbdefio);
		info->fbdefio = NULL;
		info->fbops->fb_mmap = dlfb_ops_mmap;
	}
#endif

	pr_warn("released /dev/fb%d user=%d count=%d\n",
		  info->node, user, dev->video.fb_count);

	kref_put(&dev->kref, dlfb_free);

	return 0;
}

/*
 * Check whether a video mode is supported by the DisplayLink chip
 * We start from monitor's modes, so don't need to filter that here
 */
static int dlfb_is_valid_mode(struct fb_videomode *mode,
		struct fb_info *info)
{
	struct dlfb_data *dev = info->par;
	
	printk("dlfb_is_valid_mode called\n");

	if (mode->xres * mode->yres > dev->video.sku_pixel_limit) {
		pr_warn("%dx%d beyond chip capabilities\n",
		       mode->xres, mode->yres);
		return 0;
	}

	pr_info("%dx%d @ %d Hz valid mode\n", mode->xres, mode->yres,
		mode->refresh);

	return 1;
}

static void dlfb_var_color_format(struct fb_var_screeninfo *var)
{
	const struct fb_bitfield red = { 11, 5, 0 };
	const struct fb_bitfield green = { 5, 6, 0 };
	const struct fb_bitfield blue = { 0, 5, 0 };
	
	printk("dlfb_var_color_format called\n");

	var->bits_per_pixel = 16;
	var->red = red;
	var->green = green;
	var->blue = blue;
}

static int dlfb_ops_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	struct fb_videomode mode;
	
	printk("dlfb_ops_check_var called\n");

	/* TODO: support dynamically changing framebuffer size */
	if ((var->xres * var->yres * 2) > info->fix.smem_len)
		return -EINVAL;

	/* set device-specific elements of var unrelated to mode */
	dlfb_var_color_format(var);

	fb_var_to_videomode(&mode, var);

	if (!dlfb_is_valid_mode(&mode, info))
		return -EINVAL;

	return 0;
}

static int dlfb_ops_set_par(struct fb_info *info)
{
	struct dlfb_data *dev = info->par;
	int result;
	u16 *pix_framebuffer;
	int i;
	
	printk("dlfb_ops_set_par called\n");

	pr_notice("set_par mode %dx%d\n", info->var.xres, info->var.yres);

	result = dlfb_set_video_mode(dev, &info->var);

	if ((result == 0) && (dev->video.fb_count == 0)) {

		/* paint greenscreen */

		pix_framebuffer = (u16 *) info->screen_base;
		for (i = 0; i < info->fix.smem_len / 2; i++)
			pix_framebuffer[i] = 0x37e6;

		dlfb_handle_damage(dev, 0, 0, info->var.xres, info->var.yres,
				   info->screen_base);
	}
	
	printk("Painting green completed \n");

	return result;
}

/* To fonzi the jukebox (e.g. make blanking changes take effect) */
static char *dlfb_dummy_render(char *buf)
{
	
	printk("dlfb_dummy_render called\n");	
	
	*buf++ = 0xAF;
	*buf++ = 0x6A; /* copy */
	*buf++ = 0x00; /* from address*/
	*buf++ = 0x00;
	*buf++ = 0x00;
	*buf++ = 0x01; /* one pixel */
	*buf++ = 0x00; /* to address */
	*buf++ = 0x00;
	*buf++ = 0x00;
	return buf;
}

/*
 * In order to come back from full DPMS off, we need to set the mode again
 */
static int dlfb_ops_blank(int blank_mode, struct fb_info *info)
{
	struct dlfb_data *dev = info->par;
	char *bufptr;
	struct urb *urb;
	
	printk("dlfb_ops_blank called\n");

	pr_info("/dev/fb%d FB_BLANK mode %d --> %d\n",
		info->node, dev->video.blank_mode, blank_mode);

	if ((dev->video.blank_mode == FB_BLANK_POWERDOWN) &&
	    (blank_mode != FB_BLANK_POWERDOWN)) {

		/* returning from powerdown requires a fresh modeset */
		dlfb_set_video_mode(dev, &info->var);
	}

	urb = dlfb_get_urb(dev);
	if (!urb)
		return 0;

	bufptr = (char *) urb->transfer_buffer;
	bufptr = dlfb_vidreg_lock(bufptr);
	bufptr = dlfb_blanking(bufptr, blank_mode);
	bufptr = dlfb_vidreg_unlock(bufptr);

	/* seems like a render op is needed to have blank change take effect */
	bufptr = dlfb_dummy_render(bufptr);

	dlfb_submit_urb(dev, urb, bufptr -
			(char *) urb->transfer_buffer);

	dev->video.blank_mode = blank_mode;

	return 0;
}

static struct fb_ops dlfb_ops = {
	.owner = THIS_MODULE,
	.fb_read = dlfb_ops_read,
	.fb_write = dlfb_ops_write,
	.fb_setcolreg = dlfb_ops_setcolreg,
	.fb_fillrect = dlfb_ops_fillrect,
	.fb_copyarea = dlfb_ops_copyarea,
	.fb_imageblit = dlfb_ops_imageblit,
	.fb_mmap = dlfb_ops_mmap,
	.fb_ioctl = dlfb_ops_ioctl,
	.fb_open = dlfb_ops_open,
	.fb_release = dlfb_ops_release,
	.fb_blank = dlfb_ops_blank,
	.fb_check_var = dlfb_ops_check_var,
	.fb_set_par = dlfb_ops_set_par,
};


/*
 * Assumes &info->lock held by caller
 * Assumes no active clients have framebuffer open
 */
static int dlfb_realloc_framebuffer(struct dlfb_data *dev, struct fb_info *info)
{
	int retval = -ENOMEM;
	int old_len = info->fix.smem_len;
	int new_len;
	unsigned char *old_fb = info->screen_base;
	unsigned char *new_fb;
	unsigned char *new_back = 0;
	
	printk("dlfb_realloc_framebuffer called\n");

	pr_warn("Reallocating framebuffer. Addresses will change!\n");

	new_len = info->fix.line_length * info->var.yres;

	if (PAGE_ALIGN(new_len) > old_len) {
		/*
		 * Alloc system memory for virtual framebuffer
		 */
		new_fb = vmalloc(new_len);
		if (!new_fb) {
			pr_err("Virtual framebuffer alloc failed\n");
			goto error;
		}

		if (info->screen_base) {
			memcpy(new_fb, old_fb, old_len);
			vfree(info->screen_base);
		}

		info->screen_base = new_fb;
		info->fix.smem_len = PAGE_ALIGN(new_len);
		info->fix.smem_start = (unsigned long) new_fb;
		info->flags = udlfb_info_flags;

/*
 * For a range of kernels, must set these to workaround bad logic
 * that assumes all framebuffers are using PCI aperture.
 * If we don't do this, when we call register_framebuffer, fbmem.c will
 * forcibly unregister other framebuffers with smem_start of zero.  And
 * that's most of them (VESA, EFI, etc).
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32) && \
	LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 34))
		info->aperture_base = info->fix.smem_start;
		info->aperture_size = info->fix.smem_len;
#endif

		/*
		 * Second framebuffer copy to mirror the framebuffer state
		 * on the physical USB device. We can function without this.
		 * But with imperfect damage info we may send pixels over USB
		 * that were, in fact, unchanged - wasting limited USB bandwidth
		 */
		if (shadow)
			new_back = vzalloc(new_len);
		if (!new_back)
			pr_info("No shadow/backing buffer allocated\n");
		else {
			if (dev->video.backing_buffer)
				vfree(dev->video.backing_buffer);
			dev->video.backing_buffer = new_back;
		}
	}

	retval = 0;

error:
	return retval;
}

/*
 * 1) Get EDID from hw, or use sw defaultstatic int dlfb_setup_modes
 * 2) Parse into various fb_info structs
 * 3) Allocate virtual framebuffer memory to back highest res mode
 *
 * Parses EDID into three places used by various parts of fbdev:
 * fb_var_screeninfo contains the timing of the monitor's preferred mode
 * fb_info.monspecs is full parsed EDID info, including monspecs.modedb
 * fb_info.modelist is a linked list of all monitor & VESA modes which work
 *
 * If EDID is not readable/valid, then modelist is all VESA modes,
 * monspecs is NULL, and fb_var_screeninfo is set to safe VESA mode
 * Returns 0 if successful
 */
static int dlfb_setup_modes(struct dlfb_data *dev,
			   struct fb_info *info,
			   char *default_edid, size_t default_edid_size)
{
	int i, j;
	const struct fb_videomode *default_vmode = NULL;
	int result = 0;
	char *edid = NULL;
	int tries = 3;
	
	printk("dlfb_setup_modes called\n");

	if (info->dev) /* only use mutex if info has been registered */
		mutex_lock(&info->lock);

	edid = kmalloc(EDID_LENGTH, GFP_KERNEL);
	if (!edid) {
		result = -ENOMEM;
		goto error;
	}
	
	for(j = 0; j < EDID_LENGTH; j++){
		edid[j] = sony_sdmhs53_edid[j];
	}

	fb_destroy_modelist(&info->modelist);
	memset(&info->monspecs, 0, sizeof(info->monspecs));

	/*
	 * Try to (re)read EDID from hardware first
	 * EDID data may return, but not parse as valid
	 * Try again a few times, in case of e.g. analog cable noise
	 */
	while (tries--) {

		//i = dlfb_get_edid(dev, edid, EDID_LENGTH);
		i = 128;
		
		if (i >= EDID_LENGTH){
			fb_edid_to_monspecs(edid, &info->monspecs);
		}

		if (info->monspecs.modedb_len > 0) {
			dev->video.edid = edid;
			dev->video.edid_size = i;
			break;
		}
	}

	/* If that fails, use a previously returned EDID if available */
	if (info->monspecs.modedb_len == 0) {

		pr_err("Unable to get valid EDID from device/display\n");

		if (dev->video.edid) {
			fb_edid_to_monspecs(dev->video.edid, &info->monspecs);
			if (info->monspecs.modedb_len > 0)
				pr_err("Using previously queried EDID\n");
		}
	}

	/* If that fails, use the default EDID we were handed */
	if (info->monspecs.modedb_len == 0) {
		if (default_edid_size >= EDID_LENGTH) {
			fb_edid_to_monspecs(default_edid, &info->monspecs);
			if (info->monspecs.modedb_len > 0) {
				memcpy(edid, default_edid, default_edid_size);
				dev->video.edid = edid;
				dev->video.edid_size = default_edid_size;
				pr_err("Using default/backup EDID\n");
			}
		}
	}

	/* If we've got modes, let's pick a best default mode */
	if (info->monspecs.modedb_len > 0) {

		for (i = 0; i < info->monspecs.modedb_len; i++) {
			if (dlfb_is_valid_mode(&info->monspecs.modedb[i], info))
				fb_add_videomode(&info->monspecs.modedb[i],
					&info->modelist);
			else {
				if (i == 0)
					/* if we've removed top/best mode */
					info->monspecs.misc
						&= ~FB_MISC_1ST_DETAIL;
			}
		}

		default_vmode = fb_find_best_display(&info->monspecs,
						     &info->modelist);
	}

#ifdef CONFIG_FB_MODE_HELPERS
	/* If everything else has failed, fall back to safe default mode */
	if (default_vmode == NULL) {

		struct fb_videomode fb_vmode = {0};

		/*
		 * Add the standard VESA modes to our modelist
		 * Since we don't have EDID, there may be modes that
		 * overspec monitor and/or are incorrect aspect ratio, etc.
		 * But at least the user has a chance to choose
		 */
		for (i = 0; i < VESA_MODEDB_SIZE; i++) {
			if (dlfb_is_valid_mode((struct fb_videomode *)
						&vesa_modes[i], info))
				fb_add_videomode(&vesa_modes[i],
						 &info->modelist);
		}

		/*
		 * default to resolution safe for projectors
		 * (since they are most common case without EDID)
		 */
		//fb_vmode.xres = 800;
		//fb_vmode.yres = 600;
		fb_vmode.xres = 1024;
		fb_vmode.yres = 768;
		fb_vmode.refresh = 60;
		default_vmode = fb_find_nearest_mode(&fb_vmode,
						     &info->modelist);
	}
#endif
	/* If we have good mode and no active clients*/
	if ((default_vmode != NULL) && (dev->video.fb_count == 0)) {

		fb_videomode_to_var(&info->var, default_vmode);
		dlfb_var_color_format(&info->var);

		/*
		 * with mode size info, we can now alloc our framebuffer.
		 */
		memcpy(&info->fix, &dlfb_fix, sizeof(dlfb_fix));
		info->fix.line_length = info->var.xres *
			(info->var.bits_per_pixel / 8);

		result = dlfb_realloc_framebuffer(dev, info);

	} else
		result = -EINVAL;

error:
	if (edid && (dev->video.edid != edid))
		kfree(edid);

	if (info->dev)
		mutex_unlock(&info->lock);

	return result;
}

static ssize_t metrics_bytes_rendered_show(struct device *fbdev,
				   struct device_attribute *a, char *buf) {
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("metrics_bytes_rendered_show called\n");
	
	return snprintf(buf, PAGE_SIZE, "%u\n",
			atomic_read(&dev->video.bytes_rendered));
}

static ssize_t metrics_bytes_identical_show(struct device *fbdev,
				   struct device_attribute *a, char *buf) {
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("metrics_bytes_identical_show called\n");
	
	return snprintf(buf, PAGE_SIZE, "%u\n",
			atomic_read(&dev->video.bytes_identical));
}

static ssize_t metrics_bytes_sent_show(struct device *fbdev,
				   struct device_attribute *a, char *buf) {
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("metrics_bytes_sent_show called\n");
	
	return snprintf(buf, PAGE_SIZE, "%u\n",
			atomic_read(&dev->video.bytes_sent));
}

static ssize_t metrics_cpu_kcycles_used_show(struct device *fbdev,
				   struct device_attribute *a, char *buf) {
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("metrics_cpu_kcycles_used_show called\n");
	
	return snprintf(buf, PAGE_SIZE, "%u\n",
			atomic_read(&dev->video.cpu_kcycles_used));
}

static ssize_t monitor_show(struct device *fbdev,
				   struct device_attribute *a, char *buf) {
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	
	printk("monitor_show called\n");
	
	return snprintf(buf, PAGE_SIZE, "%s-%s\n",
			fb_info->monspecs.monitor,
			fb_info->monspecs.serial_no);
}

static ssize_t edid_show(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
			struct file *filp,
#endif
			struct kobject *kobj, struct bin_attribute *a,
			char *buf, loff_t off, size_t count) {
	struct device *fbdev = container_of(kobj, struct device, kobj);
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("edid_show called\n");

	if (dev->video.edid == NULL)
		return 0;

	if ((off >= dev->video.edid_size) || (count > dev->video.edid_size))
		return 0;

	if (off + count > dev->video.edid_size)
		count = dev->video.edid_size - off;

	pr_info("sysfs edid copy %p to %p, %d bytes\n",
		dev->video.edid, buf, (int) count);

	memcpy(buf, dev->video.edid, count);

	return count;
}

static ssize_t edid_store(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
			struct file *filp,
#endif
			struct kobject *kobj, struct bin_attribute *a,
			char *src, loff_t src_off, size_t src_size) {
	struct device *fbdev = container_of(kobj, struct device, kobj);
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("edid_store called\n");

	/* We only support write of entire EDID at once, no offset*/
	if ((src_size != EDID_LENGTH) || (src_off != 0))
		return 0;

	dlfb_setup_modes(dev, fb_info, src, src_size);

	if (dev->video.edid && (memcmp(src, dev->video.edid, src_size) == 0)) {
		pr_info("sysfs written EDID is new default\n");
		dlfb_ops_set_par(fb_info);
		return src_size;
	} else
		return 0;
}

static ssize_t metrics_reset_store(struct device *fbdev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct fb_info *fb_info = dev_get_drvdata(fbdev);
	struct dlfb_data *dev = fb_info->par;
	
	printk("metrics_reset_store called\n");

	atomic_set(&dev->video.bytes_rendered, 0);
	atomic_set(&dev->video.bytes_identical, 0);
	atomic_set(&dev->video.bytes_sent, 0);
	atomic_set(&dev->video.cpu_kcycles_used, 0);

	return count;
}

static struct bin_attribute edid_attr = {
	.attr.name = "edid",
	.attr.mode = 0666,
	.size = EDID_LENGTH,
	.read = edid_show,
	.write = edid_store
};

static struct device_attribute fb_device_attrs[] = {
	__ATTR_RO(metrics_bytes_rendered),
	__ATTR_RO(metrics_bytes_identical),
	__ATTR_RO(metrics_bytes_sent),
	__ATTR_RO(metrics_cpu_kcycles_used),
	__ATTR_RO(monitor),
	__ATTR(metrics_reset, S_IWUSR, NULL, metrics_reset_store),
};

/*
 * This is necessary before we can communicate with the display controller.
 */
static int dlfb_select_std_channel(struct dlfb_data *dev)
{
	int ret;
	u8 set_def_chn[] = {	   0x57, 0xCD, 0xDC, 0xA7,
				0x1C, 0x88, 0x5E, 0x15,
				0x60, 0xFE, 0xC6, 0x97,
				0x16, 0x3D, 0x47, 0xF2  };

	printk("dlfb_select_std_channel called\n");

	ret = usb_control_msg(dev->usbdev, usb_sndctrlpipe(dev->usbdev, 0),
			NR_USB_REQUEST_CHANNEL,
			(USB_DIR_OUT | USB_TYPE_VENDOR), 0, 0,
			set_def_chn, sizeof(set_def_chn), USB_CTRL_SET_TIMEOUT);
	return ret;
}

static int dlfb_parse_vendor_descriptor(struct dlfb_data *dev,
					struct usb_interface *interface)
{
	char *desc;
	char *buf;
	char *desc_end;

	int total_len = 0;
	
	printk("dlfb_parse_vendor_descriptor called\n");

	buf = kzalloc(MAX_VENDOR_DESCRIPTOR_SIZE, GFP_KERNEL);
	if (!buf)
		return false;
	desc = buf;

	total_len = usb_get_descriptor(interface_to_usbdev(interface),
					0x5f, /* vendor specific */
					0, desc, MAX_VENDOR_DESCRIPTOR_SIZE);

	/* if not found, look in configuration descriptor */
	if (total_len < 0) {
		if (0 == usb_get_extra_descriptor(interface->cur_altsetting,
			0x5f, &desc))
			total_len = (int) desc[0];
	}

	if (total_len > 5) {
		pr_info("vendor descriptor length:%x data:%02x %02x %02x %02x" \
			"%02x %02x %02x %02x %02x %02x %02x\n",
			total_len, desc[0],
			desc[1], desc[2], desc[3], desc[4], desc[5], desc[6],
			desc[7], desc[8], desc[9], desc[10]);

		if ((desc[0] != total_len) || /* descriptor length */
		    (desc[1] != 0x5f) ||   /* vendor descriptor type */
		    (desc[2] != 0x01) ||   /* version (2 bytes) */
		    (desc[3] != 0x00) ||
		    (desc[4] != total_len - 2)) /* length after type */
			goto unrecognized;

		desc_end = desc + total_len;
		desc += 5; /* the fixed header we've already parsed */

		while (desc < desc_end) {
			u8 length;
			u16 key;

			key = le16_to_cpu(*((u16 *) desc));
			desc += sizeof(u16);
			length = *desc;
			desc++;

			switch (key) {
			case 0x0200: { /* max_area */
				u32 max_area;
				max_area = le32_to_cpu(*((u32 *)desc));
				pr_warn("DL chip limited to %d pixel modes\n",
					max_area);
				dev->video.sku_pixel_limit = max_area;
				break;
			}
			default:
				break;
			}
			desc += length;
		}
	} else {
		pr_info("vendor descriptor not available (%d)\n", total_len);
	}

	goto success;

unrecognized:
	/* allow udlfb to load for now even if firmware unrecognized */
	pr_err("Unrecognized vendor firmware descriptor\n");

success:
	kfree(buf);
	return true;
}

static void dlfb_init_framebuffer_work(struct work_struct *work);


static void
set_bulk_address (
	struct dlfb_data *dev,
	struct usb_interface *interface)
{
	struct usb_endpoint_descriptor *endpoint;
	struct usb_host_interface *iface_desc;
	int i;
	
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;
		
		/* check for bulk endpoint */
		if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) 
			== USB_ENDPOINT_XFER_BULK){
			
			/* bulk in */
			if(endpoint->bEndpointAddress & USB_DIR_IN) {
				dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
				dev->bulk_in_size = endpoint->wMaxPacketSize;
				dev->video.bulk_in_buffer = kmalloc(dev->bulk_in_size,
							 	GFP_KERNEL);
				if (!dev->video.bulk_in_buffer)
					printk("Could not allocate bulk buffer");
			}
			
			/* bulk out */
			else
				dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;	
		}
	}
}

static int dlfb_video_init(struct dlfb_data *dev){

	dev->video.sku_pixel_limit = 2048 * 1152; /* default to maximum */

	if (pixel_limit) {
		pr_warn("DL chip limit of %d overriden"
			" by module param to %d\n",
			dev->video.sku_pixel_limit, pixel_limit);
		dev->video.sku_pixel_limit = pixel_limit;
	}


	if (!dlfb_alloc_urb_list(dev, WRITES_IN_FLIGHT, MAX_TRANSFER)) {
		//retval = -ENOMEM;
		pr_err("dlfb_alloc_urb_list failed\n");
		return -ENOMEM;
	}

	return 0;
}

static int dlfb_usb_probe(struct usb_interface *interface,
			const struct usb_device_id *id)
{
	struct usb_device *usbdev;
	struct dlfb_data *dev = 0;
	int retval = -ENOMEM;

	#ifdef CONFIG_FB_DEFERRED_IO
	printk("Kernel has FB_DEFERRED_IO support\n");
	#endif
	
	#ifdef CONFIG_FB_SYS_IMAGEBLIT
	printk("Kernel has FB_SYS_IMAGEBLIT support\n");
	#endif

	#ifdef CONFIG_FB_SYS_FOPS
	printk("Kernel has FB_SYS_FOPS support\n");
	#endif

	#ifdef CONFIG_FB_MODE_HELPERS
	printk("Kernel has FB_MODE_HELPERS support\n");
	#endif

	printk("dlfb_usb_probe called\n");

	/* usb initialization */

	usbdev = interface_to_usbdev(interface);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL) {
		err("dlfb_usb_probe: failed alloc of dev struct\n");
		goto error;
	}

	kref_init(&dev->kref); /* matching kref_put in usb .disconnect fn */

	dev->usbdev = usbdev;
	dev->dev = &usbdev->dev; /* our generic struct device * */
	usb_set_intfdata(interface, dev);
	set_bulk_address(dev, interface);

	pr_info("%s %s - serial #%s\n",
		usbdev->manufacturer, usbdev->product, usbdev->serial);
	pr_info("vid_%04x&pid_%04x&rev_%04x driver's dlfb_data struct at %p\n",
		usbdev->descriptor.idVendor, usbdev->descriptor.idProduct,
		usbdev->descriptor.bcdDevice, dev);
	pr_info("console enable=%d\n", console);
	pr_info("fb_defio enable=%d\n", fb_defio);
	pr_info("shadow enable=%d\n", shadow);


	if (!dlfb_parse_vendor_descriptor(dev, interface)) {
		pr_err("firmware not recognized. Assume incompatible device\n");
		goto error;
	}


	if (dlfb_video_init(dev)){
		retval = -ENOMEM;
		goto error;
	}



	kref_get(&dev->kref); /* matching kref_put in free_framebuffer_work */

	/* We don't register a new USB class. Our client interface is fbdev */

	/* Workitem keep things fast & simple during USB enumeration */
	INIT_DELAYED_WORK(&dev->init_framebuffer_work,
			  dlfb_init_framebuffer_work);
	schedule_delayed_work(&dev->init_framebuffer_work, 0);

	return 0;

error:
	if (dev) {

		kref_put(&dev->kref, dlfb_free); /* ref for framebuffer */
		kref_put(&dev->kref, dlfb_free); /* last ref from kref_init */

		/* dev has been deallocated. Do not dereference */
	}

	return retval;
}

static void dlfb_init_framebuffer_work(struct work_struct *work)
{
	struct dlfb_data *dev = container_of(work, struct dlfb_data,
					     init_framebuffer_work.work);
	struct fb_info *info;
	int retval;
	int i;

	printk("dlfb_init_framebuffer_work called\n");

	/* allocates framebuffer driver structure, not framebuffer memory */
	info = framebuffer_alloc(0, dev->dev);
	if (!info) {
		retval = -ENOMEM;
		pr_err("framebuffer_alloc failed\n");
		goto error;
	}

	dev->video.info = info;
	info->par = dev;
	info->pseudo_palette = dev->video.pseudo_palette;
	info->fbops = &dlfb_ops;

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0) {
		pr_err("fb_alloc_cmap failed %x\n", retval);
		goto error;
	}

	INIT_DELAYED_WORK(&dev->free_framebuffer_work,
			  dlfb_free_framebuffer_work);

	INIT_LIST_HEAD(&info->modelist);

	retval = dlfb_setup_modes(dev, info, NULL, 0);
	if (retval != 0) {
		pr_err("unable to find common mode for display and adapter\n");
		goto error;
	}

	/* ready to begin using device */

	atomic_set(&dev->video.usb_active, 1);
	dlfb_select_std_channel(dev);

	dlfb_ops_check_var(&info->var, info);
	dlfb_ops_set_par(info);
	
	/* -TODO- No debug messages here after */

	retval = register_framebuffer(info);
	printk("Registering framebuffer\n");
	if (retval < 0) {
		pr_err("register_framebuffer failed %d\n", retval);
		goto error;
	} else{
		printk("Frame buffer registered\n");
	}
	printk("Registering framebuffer done\n");

	for (i = 0; i < ARRAY_SIZE(fb_device_attrs); i++) {
		retval = device_create_file(info->dev, &fb_device_attrs[i]);
		if (retval) {
			pr_warn("device_create_file failed %d\n", retval);
		} else{
			printk("device_create_file success!\n");
		}
	}

	retval = device_create_bin_file(info->dev, &edid_attr);
	if (retval) {
		pr_warn("device_create_bin_file failed %d\n", retval);
	} else{
		printk("device_create_bin_file success\n");
	}

	pr_info("DisplayLink USB device /dev/fb%d attached. %dx%d resolution."
			" Using %dK framebuffer memory\n", info->node,
			info->var.xres, info->var.yres,
			((dev->video.backing_buffer) ?
			info->fix.smem_len * 2 : info->fix.smem_len) >> 10);
	return;

error:
	dlfb_free_framebuffer(dev);
}

static void dlfb_usb_disconnect(struct usb_interface *interface)
{
	struct dlfb_data *dev;
	struct fb_info *info;
	int i;
	
	printk("dlfb_usb_disconnect called\n");

	dev = usb_get_intfdata(interface);
	info = dev->video.info;

	pr_info("USB disconnect starting\n");

	/* we virtualize until all fb clients release. Then we free */
	dev->video.virtualized = true;

	/* When non-active we'll update virtual framebuffer, but no new urbs */
	atomic_set(&dev->video.usb_active, 0);

	/* this function will wait for all in-flight urbs to complete */
	/* -TODO- Free these urbs.
	 * temporarily removed to see if this fixes the disconnect freeze issue
	 */
	
	//dlfb_free_urb_list(dev);

	if (info) {

		/* remove udlfb's sysfs interfaces */
		for (i = 0; i < ARRAY_SIZE(fb_device_attrs); i++)
			device_remove_file(info->dev, &fb_device_attrs[i]);
		device_remove_bin_file(info->dev, &edid_attr);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0))
		unlink_framebuffer(info);
#endif
	}

	usb_set_intfdata(interface, NULL);
	dev->usbdev = NULL;
	dev->dev = NULL;

	/* if clients still have us open, will be freed on last close */
	if (dev->video.fb_count == 0)
		schedule_delayed_work(&dev->free_framebuffer_work, 0);

	/* release reference taken by kref_init in probe() */
	kref_put(&dev->kref, dlfb_free);

	/* consider dlfb_data freed */

	return;
}

static struct usb_driver dlfb_driver = {
	.name = "udlfb",
	.probe = dlfb_usb_probe,
	.disconnect = dlfb_usb_disconnect,
	.id_table = id_table,
};

static int __init dlfb_module_init(void)
{
	int res;

	res = usb_register(&dlfb_driver);
	if (res)
		//err("usb_register failed. Error number %d", res);
		err("usb_register failed. Error number");

	return res;
}

static void __exit dlfb_module_exit(void)
{

	printk("dlfb_module_exit called\n");
	
	if(&dlfb_driver)
		usb_deregister(&dlfb_driver);
}

module_init(dlfb_module_init);
module_exit(dlfb_module_exit);

static void dlfb_urb_completion(struct urb *urb)
{
	struct urb_node *unode = urb->context;
	struct dlfb_data *dev = unode->dev;
	unsigned long flags;

	printk("dlfb_urb_completion called\n");

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN)) {
			pr_err("%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);
			atomic_set(&dev->video.lost_pixels, 1);
		}
	}

	urb->transfer_buffer_length = dev->video.urbs.size; /* reset to actual */

	spin_lock_irqsave(&dev->video.urbs.lock, flags);
	list_add_tail(&unode->entry, &dev->video.urbs.list);
	dev->video.urbs.available++;
	spin_unlock_irqrestore(&dev->video.urbs.lock, flags);

	/*
	 * When using fb_defio, we deadlock if up() is called
	 * while another is waiting. So queue to another process.
	 */
	if (fb_defio)
		schedule_delayed_work(&unode->release_urb_work, 0);
	else
		up(&dev->video.urbs.limit_sem);
}

static void dlfb_free_urb_list(struct dlfb_data *dev)
{
	int count = dev->video.urbs.count;
	struct list_head *node;
	struct urb_node *unode;
	struct urb *urb;
	int ret;
	unsigned long flags;
	
	printk("dlfb_free_urb_list called\n");

	pr_notice("Freeing all render urbs\n");

	/* keep waiting and freeing, until we've got 'em all */
	while (count--) {

		/* Getting interrupted means a leak, but ok at disconnect */
		ret = down_interruptible(&dev->video.urbs.limit_sem);
		if (ret)
			break;

		spin_lock_irqsave(&dev->video.urbs.lock, flags);

		node = dev->video.urbs.list.next; /* have reserved one with sem */
		list_del_init(node);

		spin_unlock_irqrestore(&dev->video.urbs.lock, flags);

		unode = list_entry(node, struct urb_node, entry);
		urb = unode->urb;

		/* Free each separately allocated piece */
		usb_free_coherent(urb->dev, dev->video.urbs.size,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
		kfree(node);
	}

	dev->video.urbs.count = 0;
}

static int dlfb_alloc_urb_list(struct dlfb_data *dev, int count, size_t size)
{
	int i = 0;
	struct urb *urb;
	struct urb_node *unode;
	char *buf;
	
	printk("dlfb_alloc_urb_list called\n");

	spin_lock_init(&dev->video.urbs.lock);

	dev->video.urbs.size = size;
	INIT_LIST_HEAD(&dev->video.urbs.list);

	while (i < count) {
		unode = kzalloc(sizeof(struct urb_node), GFP_KERNEL);
		if (!unode)
			break;
		unode->dev = dev;

		INIT_DELAYED_WORK(&unode->release_urb_work,
			  dlfb_release_urb_work);

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			kfree(unode);
			break;
		}
		unode->urb = urb;

		buf = usb_alloc_coherent(dev->usbdev, MAX_TRANSFER, GFP_KERNEL,
					 &urb->transfer_dma);
		if (!buf) {
			kfree(unode);
			usb_free_urb(urb);
			break;
		}

		// -TODO- Remove hardcoded bulkout address
		/* urb->transfer_buffer_length set to actual before submit */
		/*  */
		usb_fill_bulk_urb(urb, dev->usbdev, 
			usb_sndbulkpipe(dev->usbdev, dev->bulk_out_endpointAddr),
			buf, size, dlfb_urb_completion, unode);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		list_add_tail(&unode->entry, &dev->video.urbs.list);

		i++;
	}

	sema_init(&dev->video.urbs.limit_sem, i);
	dev->video.urbs.count = i;
	dev->video.urbs.available = i;

	pr_notice("allocated %d %d byte urbs\n", i, (int) size);

	return i;
}

static struct urb *dlfb_get_urb(struct dlfb_data *dev)
{
	int ret = 0;
	struct list_head *entry;
	struct urb_node *unode;
	struct urb *urb = NULL;
	unsigned long flags;
	
	printk("dlfb_get_urb called\n");

	/* Wait for an in-flight buffer to complete and get re-queued */
	ret = down_timeout(&dev->video.urbs.limit_sem, GET_URB_TIMEOUT);
	if (ret) {
		atomic_set(&dev->video.lost_pixels, 1);
		pr_warn("wait for urb interrupted: %x available: %d\n",
		       ret, dev->video.urbs.available);
		goto error;
	}

	spin_lock_irqsave(&dev->video.urbs.lock, flags);

	BUG_ON(list_empty(&dev->video.urbs.list)); /* reserved one with limit_sem */
	entry = dev->video.urbs.list.next;
	list_del_init(entry);
	dev->video.urbs.available--;

	spin_unlock_irqrestore(&dev->video.urbs.lock, flags);

	unode = list_entry(entry, struct urb_node, entry);
	urb = unode->urb;

error:
	return urb;
}

static int dlfb_submit_urb(struct dlfb_data *dev, struct urb *urb, size_t len)
{
	int ret;

	printk("dlfb_submit_urb called\n");	
	
	BUG_ON(len > dev->video.urbs.size);

	urb->transfer_buffer_length = len; /* set to actual payload len */
	
	/*
	 * Any urb submits are ignored and returned a success code.
	 * Right now, the build doesn't repond well when there are data
	 * transfers over usb, besides the frame data. Check git commits to
	 * to see what was deleted. Commit "Cleanup in dlfb_submit_urb"
	 */
	
	ret = 0;
	
	return ret;
}

module_param(console, bool, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
MODULE_PARM_DESC(console, "Allow fbcon to open framebuffer");

module_param(fb_defio, bool, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
MODULE_PARM_DESC(fb_defio, "Page fault detection of mmap writes");

module_param(shadow, bool, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
MODULE_PARM_DESC(shadow, "Shadow vid mem. Disable to save mem but lose perf");

module_param(pixel_limit, int, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
MODULE_PARM_DESC(pixel_limit, "Force limit on max mode (in x*y pixels)");

MODULE_AUTHOR("Roberto De Ioris <roberto@unbit.it>, "
	      "Jaya Kumar <jayakumar.lkml@gmail.com>, "
	      "Bernie Thompson <bernie@plugable.com>");
MODULE_DESCRIPTION("DisplayLink kernel framebuffer driver");
MODULE_LICENSE("GPL");

