/*
 * suspend.c
 *
 * A simple user space suspend handler for swsusp.
 *
 * Copyright (C) 2005 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * This file is released under the GPLv2.
 *
 */

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/vt.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>
#include <linux/kd.h>
#include <linux/tiocl.h>
#include <syscall.h>
#include <libgen.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#ifdef CONFIG_THREADS
#include <pthread.h>
#endif
#ifdef CONFIG_COMPRESS
#include <lzo/lzo1x.h>
#endif

#include "swsusp.h"
#include "memalloc.h"
#include "config_parser.h"
#include "md5.h"
#include "splash.h"
#include "vt.h"
#include "loglevel.h"
#ifdef CONFIG_BOTH
#include "s2ram.h"
#endif
static char test_file_name[MAX_STR_LEN] = "";
static loff_t test_image_size;

#define suspend_error(msg, args...) \
do { \
	fprintf(stderr, "%s: " msg " Reason: %m\n", my_name, ## args); \
} while (0)

#ifdef CONFIG_ARCH_S390
#define suspend_warning(msg)
#else
#define suspend_warning(msg) \
do { \
	fprintf(stderr, "%s: " msg "\n", my_name); \
} while (0)
#endif

static char snapshot_dev_name[MAX_STR_LEN] = SNAPSHOT_DEVICE;
static char resume_dev_name[MAX_STR_LEN] = RESUME_DEVICE;
static loff_t resume_offset;
static loff_t pref_image_size = IMAGE_SIZE;
static int suspend_loglevel = SUSPEND_LOGLEVEL;
static char compute_checksum;
#ifdef CONFIG_COMPRESS
static char do_compress;
#else
#define do_compress 0
#endif
#ifdef CONFIG_ENCRYPT
static char do_encrypt;
static char use_RSA;
static char key_name[MAX_STR_LEN] = SUSPEND_KEY_FILE_PATH;
static char password[PASSBUF_SIZE];
static unsigned long encrypt_buf_size;
#else
#define do_encrypt 0
#define key_name NULL
#define encrypt_buf_size 0
#endif
#ifdef CONFIG_BOTH
static char s2ram;
static char s2ram_kms;
#endif
static char early_writeout;
static char splash_param;
#ifdef CONFIG_FBSPLASH
char fbsplash_theme[MAX_STR_LEN] = "";
#endif
#define SHUTDOWN_LEN	16
static char shutdown_method_value[SHUTDOWN_LEN] = "";
static enum {
	SHUTDOWN_METHOD_SHUTDOWN,
	SHUTDOWN_METHOD_PLATFORM,
	SHUTDOWN_METHOD_REBOOT
} shutdown_method = SHUTDOWN_METHOD_PLATFORM;
static int resume_pause;
static char verify_image;
#ifdef CONFIG_THREADS
static char use_threads;
#else
#define use_threads	0
#endif

static int suspend_swappiness = 0/* SUSPEND_SWAPPINESS */; //madhu 131212
static struct vt_mode orig_vtm;
static int vfd;

static struct config_par parameters[] = {
	{
		.name = "snapshot device",
		.fmt = "%s",
		.ptr = snapshot_dev_name,
		.len = MAX_STR_LEN
	},
	{
		.name = "resume device",
		.fmt ="%s",
		.ptr = resume_dev_name,
		.len = MAX_STR_LEN
	},
	{
		.name = "resume offset",
		.fmt = "%llu",
		.ptr = &resume_offset,
	},
	{
		.name = "image size",
		.fmt = "%lu",
		.ptr = &pref_image_size,
	},
	{
		.name = "suspend loglevel",
		.fmt = "%d",
		.ptr = &suspend_loglevel,
	},
	{
		.name = "max loglevel",
		.fmt = "%d",
		.ptr = NULL,
	},
	{
		.name = "compute checksum",
		.fmt = "%c",
		.ptr = &compute_checksum,
	},
#ifdef CONFIG_COMPRESS
	{
		.name = "compress",
		.fmt = "%c",
		.ptr = &do_compress,
	},
#endif
#ifdef CONFIG_ENCRYPT
	{
		.name = "encrypt",
		.fmt = "%c",
		.ptr = &do_encrypt,
	},
	{
		.name = "RSA key file",
		.fmt = "%s",
		.ptr = key_name,
		.len = MAX_STR_LEN
	},
#endif
	{
		.name = "early writeout",
		.fmt = "%c",
		.ptr = &early_writeout,
	},
	{
		.name = "splash",
		.fmt = "%c",
		.ptr = &splash_param,
	},
	{
		.name = "shutdown method",
		.fmt = "%s",
		.ptr = shutdown_method_value,
		.len = SHUTDOWN_LEN,
	},
#ifdef CONFIG_FBSPLASH
	{
		.name = "fbsplash theme",
		.fmt = "%s",
		.ptr = fbsplash_theme,
		.len = MAX_STR_LEN,
	},
#endif
	{
		.name = "resume pause",
		.fmt = "%d",
		.ptr = &resume_pause,
	},
	{
		.name = "debug test file",
		.fmt = "%s",
		.ptr = test_file_name,
		.len = MAX_STR_LEN
	},
	{
		.name = "debug verify image",
		.fmt = "%c",
		.ptr = &verify_image,
	},
#ifdef CONFIG_THREADS
	{
		.name = "threads",
		.fmt = "%c",
		.ptr = &use_threads,
	},
#endif
	{
		.name = NULL,
		.fmt = NULL,
		.ptr = NULL,
		.len = 0,
	}
};

static loff_t check_free_swap(int dev)
{
	int error;
	loff_t free_swap;

	error = ioctl(dev, SNAPSHOT_AVAIL_SWAP_SIZE, &free_swap);
	if (error && errno == ENOTTY)
		error = ioctl(dev, SNAPSHOT_AVAIL_SWAP, &free_swap);
	if (!error)
		return free_swap;

	suspend_error("check_free_swap failed.");
	return 0;
}

static loff_t get_image_size(int dev)
{
	int error;
	loff_t image_size;

	error = ioctl(dev, SNAPSHOT_GET_IMAGE_SIZE, &image_size);
	if (!error)
		return image_size;

	suspend_error("get_image_size failed.");
	return 0;
}

static inline loff_t get_swap_page(int dev)
{
	int error;
	loff_t offset;

	error = ioctl(dev, SNAPSHOT_ALLOC_SWAP_PAGE, &offset);
	if (error && errno == ENOTTY)
		error = ioctl(dev, SNAPSHOT_GET_SWAP_PAGE, &offset);
	if (!error)
		return offset;
	return 0;
}

static inline int free_swap_pages(int dev)
{
	return ioctl(dev, SNAPSHOT_FREE_SWAP_PAGES, 0);
}

static int set_swap_file(int dev, u_int32_t blkdev, loff_t offset)
{
	struct resume_swap_area swap;
	int error;

	swap.dev = blkdev;
	swap.offset = offset;
	error = ioctl(dev, SNAPSHOT_SET_SWAP_AREA, &swap);
	if (error && !offset)
		error = ioctl(dev, SNAPSHOT_SET_SWAP_FILE, blkdev);
	return error;
}

static int atomic_snapshot(int dev, int *in_suspend)
{
	int error;

	fprintf(stderr, "calling atomic_snapshot: SNAPSHOT_CREATE_IMAGE\n", error);
	error = ioctl(dev, SNAPSHOT_CREATE_IMAGE, in_suspend);
	fprintf(stderr, "atomic_snapshot: SNAPSHOT_CREATE_IMAGE: %d\n", error);
	if (error && errno == ENOTTY) {
		error = ioctl(dev, SNAPSHOT_ATOMIC_SNAPSHOT, in_suspend);
		fprintf(stderr, "atomic_snapshot (enotty): SNAPSHOT_ATOMIC_SNAPSHOT: %d\n", error);
	}
	return error;
}

static inline int free_snapshot(int dev)
{
	return ioctl(dev, SNAPSHOT_FREE, 0);
}

static int set_image_size(int dev, loff_t size)
{
	int error;

	printf("MADHU: set_image_size(%d,%d) call SNAPSHOT_PREF_IMAGE_SIZE\n",
	       dev, size);
	error = ioctl(dev, SNAPSHOT_PREF_IMAGE_SIZE, size);
	printf("MADHU: SNAPSHOT_PREF_IMAGE_SIZE: ret %d\n", error);
	if (error && errno == ENOTTY) {
		printf("MADHU: call: set_image_size enotty call SNAPSHOT_SET_IMAGE_SIZE\n");
		error = ioctl(dev, SNAPSHOT_SET_IMAGE_SIZE, size);
		printf("MADHU: call: set_SNAPSHOT_SET_IMAGE_SIZE ret %d\n", error);
	}
	return error;
}

static inline int suspend_to_ram(int dev)
{
	return ioctl(dev, SNAPSHOT_S2RAM, 0);
}

static int platform_enter(int dev)
{
	int error;

	error = ioctl(dev, SNAPSHOT_POWER_OFF, 0);
	if (error  && errno == ENOTTY)
		error = ioctl(dev, SNAPSHOT_PMOPS, PMOPS_ENTER);
	return error;
}

/**
 *	alloc_swap - allocate a number of swap pages
 *	@dev:		Swap device to use for allocations.
 *	@extents:	Array of extents to track the allocations.
 *	@nr_extents:	Number of extents already in the array.
 *	@size_p:	Points to the number of bytes to allocate, used to
 *			return the number of allocated bytes.
 *
 *	Allocate the number of swap pages sufficient for saving the number of
 *	bytes pointed to by @size_p.  Use the array @extents to track the
 *	allocations.  This array has to be page_size big and may already
 *	contain some initial elements (in that case @nr_extents must be the
 *	number of these elements).
 *	Each element of the array represents an area of allocated swap space.
 *	These areas may be extended when swap pages that can be added to them
 *	are found.  They also can be merged with one another.
 *	The function returns when the requested amount of swap space is
 *	allocated or if there is no room for more extents.  In the latter case
 *	the last extent created is put at the end of the array and may be passed
 *	to alloc_swap() as the initial extent when it is invoked next time.
 */
static int
alloc_swap(int dev, struct extent *extents, int nr_extents, loff_t *size_p)
{
	const int max_extents = page_size / sizeof(struct extent) - 1;
	loff_t size, total_size, offset;

	total_size = *size_p;
	if (nr_extents <= 0) {
		offset = get_swap_page(dev);
		if (!offset)
			return -ENOSPC;
		extents->start = offset;
		extents->end = offset + page_size;
		nr_extents = 1;
		size = page_size;
	} else {
		size = 0;
	}
	while (size < total_size && nr_extents <= max_extents) {
		int i, j;

		offset = get_swap_page(dev);
		if (!offset)
			return -ENOSPC;
		/* Check if we have a matching extent. */
		i = 0;
		j = nr_extents - 1;
		do {
			struct extent *ext;
			int k = (i + j) / 2;

 Repeat:
			ext = extents + k;
			if (offset == ext->start - page_size) {
				ext->start = offset;
				/* Check if we can merge extents */
				if (k > 0 && extents[k-1].end == offset) {
					extents[k-1].end = ext->end;
					/* Pull the 'later' extents forward */
					memmove(ext, ext + 1,
						(nr_extents - k - 1) *
							sizeof(*ext));
					nr_extents--;
				}
				offset = 0;
				break;
			} else if (offset == ext->end) {
				ext->end += page_size;
				/* Check if we can merge extents */
				if (k + 1 < nr_extents
				    && ext->end == extents[k+1].start) {
					ext->end = extents[k+1].end;
					/* Pull the 'later' extents forward */
					memmove(ext + 1, ext + 2,
						(nr_extents - k - 2) *
							sizeof(*ext));
					nr_extents--;
				}
				offset = 0;
				break;
			} else if (offset > ext->end) {
				if (i == k) {
					if (i < j) {
						/* This means i == j + 1 */
						k = j;
						i = j;
						goto Repeat;
					}
				} else {
					i = k;
				}
			} else {
				/* offset < ext->start - page_size */
				j = k;
			}
		} while (i < j);
		if (offset > 0) {
			/* No match.  Create a new extent. */
			struct extent *ext;

			if (nr_extents < max_extents) {
				ext = extents + i;
				/*
				 * We want to always replace the extent 'i' with
				 * the new one.
				 */
				if (offset > ext->end) {
					i++;
					ext++;
				}
				/* Push the 'later' extents backwards. */
				memmove(ext + 1, ext,
					(nr_extents - i) * sizeof(*ext));
			} else {
				ext = extents + nr_extents;
			}
			ext->start = offset;
			ext->end = offset + page_size;
			nr_extents++;
		}
		size += page_size;
	}
	*size_p = size;
	return nr_extents;
}

/**
 *	write_page - Write page_size data to given swap location.
 *	@fd:		File handle of the resume partition.
 *	@buf:		Pointer to the area we're writing.
 *	@offset:	Offset of the swap page we're writing to.
 */
static int write_page(int fd, void *buf, loff_t offset)
{
	int res = 0;
	ssize_t cnt = 0;

	if (!offset)
		return -EINVAL;

	if (lseek64(fd, offset, SEEK_SET) == offset)
		cnt = write(fd, buf, page_size);
	if (cnt != page_size)
		res = -EIO;
	return res;
}

/*
 * The swap_writer structure is used for handling swap in a file-alike way.
 *
 * @extents:	Array of extents used for trackig swap allocations.  It is
 *		page_size bytes large and holds at most
 *		(page_size / sizeof(struct extent) - 1) extents.  The last slot
 *		is used to hold the extent that will be used as an initial one
 *		for the next batch of allocations.
 *
 * @nr_extents:		Number of entries in @extents actually used.
 *
 * @cur_extent:		The extent currently used as the source of swap pages.
 *
 * @cur_extent_idx:	The index of @cur_extent.
 *
 * @cur_offset:		The offset of the swap page that will be used next.
 *
 * @swap_needed:	The amount of swap needed for saving the image.
 *
 * @written_data:	The amount of data actually saved.
 *
 * @extents_spc:	The swap page to which to save @extents.
 *
 * @buffer:		Buffer used for storing image data pages.
 *
 * @write_buffer:	If compression is used, the compressed contents of
 *			@buffer are stored here.  Otherwise, it is equal to
 *			@buffer.
 *
 * @page_ptr:		Address to write the next image page to.
 *
 * @dev:		Snapshot device handle used for reading image pages and
 *			invoking ioctls.
 *
 * @fd:			File handle associated with the swap.
 *
 * @ctx:		Used for checksum computing, if so configured.
 *
 * @lzo_work_buffer:	Work buffer used for compression.
 *
 * @encrypt_buffer:	Buffer for storing encrypted data (page_size bytes).
 *
 * @encrypt_ptr:	Address to store the next encrypted page at.
 */
struct swap_writer {
	struct extent *extents;
	int nr_extents;
	struct extent *cur_extent;
	int cur_extent_idx;
	loff_t cur_offset;
	loff_t swap_needed;
	loff_t written_data;
	loff_t extents_spc;
	void *buffer;
	void *write_buffer;
	void *page_ptr;
	int dev, fd, input;
	struct md5_ctx ctx;
	void *lzo_work_buffer;
	void *encrypt_buffer;
	void *encrypt_ptr;
};

/**
 *	free_swap_writer - free memory allocated for saving the image
 *	@handle:	Structure containing pointers to memory buffers to free.
 */
static void free_swap_writer(struct swap_writer *handle)
{
	if (handle->write_buffer != handle->buffer)
		freemem(handle->write_buffer);
	if (do_compress)
		freemem(handle->lzo_work_buffer);
	if (handle->encrypt_buffer)
		freemem(handle->encrypt_buffer);
	freemem(handle->buffer);
	freemem(handle->extents);
}

/**
 *	init_swap_writer - initialize the structure used for saving the image
 *	@handle:	Structure to initialize.
 *	@dev:		Special device file to read image pages from.
 *	@fd:		File descriptor associated with the swap.
 *
 *	It doesn't preallocate swap, so preallocate_swap() has to be called on
 *	@handle after this.
 */
static int init_swap_writer(struct swap_writer *handle, int dev, int fd, int in)
{
	loff_t offset;
	unsigned int write_buf_size = 0;

	handle->extents = getmem(page_size);

	handle->buffer = getmem(buffer_size);
	handle->page_ptr = handle->buffer;

	if (do_encrypt) {
		handle->encrypt_buffer = getmem(encrypt_buf_size);
		handle->encrypt_ptr = handle->encrypt_buffer;
	} else {
		handle->encrypt_buffer = NULL;
	}

	if (do_compress) {
		handle->lzo_work_buffer = getmem(LZO1X_1_MEM_COMPRESS);
		write_buf_size = compress_buf_size;
		if (use_threads)
			write_buf_size +=
				(WRITE_BUFFERS - 1) * compress_buf_size;
	}

	if (write_buf_size > 0)
		handle->write_buffer = getmem(write_buf_size);
	else if (use_threads)
		handle->write_buffer = getmem(buffer_size * WRITE_BUFFERS);
	else
		handle->write_buffer = handle->buffer;

	handle->dev = dev;
	handle->fd = fd;
	handle->input = (in >= 0) ? in : dev;
	handle->written_data = 0;

	memset(handle->extents, 0, page_size);
	handle->nr_extents = 0;
	offset = get_swap_page(dev);
	if (!offset) {
		free_swap_writer(handle);
		return -ENOSPC;
	}
	handle->extents_spc = offset;

	if (compute_checksum || verify_image)
		md5_init_ctx(&handle->ctx);

	return 0;
}

/**
 *	preallocate_swap - use alloc_swap() to preallocate the number of pages
 *			given by @handle->swap_needed
 *	@handle:	Pointer to the structure in which to store information
 *			about the preallocated swap pool.
 *
 *	Returns the offset of the first swap page available from the
 *	preallocated pool.
 */
static loff_t preallocate_swap(struct swap_writer *handle)
{
	const int max = page_size / sizeof(struct extent) - 1;
	loff_t size;
	int nr_extents;

	if (handle->swap_needed < page_size)
		return 0;
	size = handle->swap_needed;
	if (do_compress && size > page_size)
		size /= 2;
	nr_extents = alloc_swap(handle->dev, handle->extents,
					handle->nr_extents, &size);
	if (nr_extents <= 0)
		return 0;
	handle->nr_extents = nr_extents < max ? nr_extents : max;
	handle->cur_extent = handle->extents;
	handle->cur_extent_idx = 0;
	handle->cur_offset = handle->cur_extent->start;
	return handle->cur_offset;
}

/**
 *	save_extents - save the array of extents
 *	handle:	Structure holding the pointer to the array of extents etc.
 *	finish:	If set, the last element of the extents array has to be filled
 *		with zeros.
 *
 *	Save the buffer (page) holding the array of extents to the swap
 *	location pointed to by @handle->extents_spc (this must be allocated
 *	earlier).  Before saving the last element of the array is used to store
 *	the swap offset of the next extents page (we allocate a swap page for
 *	this purpose).
 */
static int save_extents(struct swap_writer *handle, int finish)
{
	loff_t offset = 0;
	int error;

	if (!finish) {
		struct extent *last_extent;

		offset = get_swap_page(handle->dev);
		if (!offset)
			return -ENOSPC;
		last_extent = handle->extents +
				page_size / sizeof(struct extent) - 1;
		last_extent->start = offset;
	}
	error = write_page(handle->fd, handle->extents, handle->extents_spc);
	handle->extents_spc = offset;
	return error;
}

/**
 *	next_swap_page - take one swap page out of the pool allocated using
 *			alloc_swap() before
 *	@handle:	Pointer to the structure containing information about
 *			the preallocated swap pool.
 */
static loff_t next_swap_page(struct swap_writer *handle)
{
	struct extent ext;

	handle->cur_offset += page_size;
	if (handle->cur_offset < handle->cur_extent->end)
		return handle->cur_offset;
	/* We have exhausted the current extent.  Forward to the next one */
	handle->cur_extent++;
	handle->cur_extent_idx++;
	if (handle->cur_extent_idx < handle->nr_extents) {
		handle->cur_offset = handle->cur_extent->start;
		return handle->cur_offset;
	}
	/* No more extents.  Is there anything to pass to alloc_swap()? */
	if (handle->cur_extent->start < handle->cur_extent->end) {
		ext = *handle->cur_extent;
		memset(handle->cur_extent, 0, sizeof(struct extent));
		handle->nr_extents = 1;
	} else {
		memset(&ext, 0, sizeof(struct extent));
		handle->nr_extents = 0;
	}
	if (save_extents(handle, 0))
		return 0;
	memset(handle->extents, 0, page_size);
	*handle->extents = ext;
	return preallocate_swap(handle);
}

/**
 *	save_page - save one page of data to the swap
 *	@handle:	Pointer to the structure containing information about
 *			the swap.
 *	@src:		Pointer to the data.
 */
static int save_page(struct swap_writer *handle, void *src)
{
	loff_t offset;
	int error;

	offset = next_swap_page(handle);
	if (!offset)
		return -ENOSPC;
	error = write_page(handle->fd, src, offset);
	if (error)
		return error;
	handle->swap_needed -= page_size;
	handle->written_data += page_size;
	return 0;
}

#ifdef CONFIG_THREADS
/*
 * If threads are used for saving the image with compression and encryption,
 * there are three of them.
 *
 * The main one reads image pages from the kernel and puts them into a work
 * buffer.  When the work buffer is full, it gets compressed, but that's not an
 * in-place compression, so the result has to be stored somewhere else.  There
 * are four so-called "write" buffers for that and the first empty "write"
 * buffer is used as the target.  If all of the "write" buffers are full, the
 * thread has to wait (see the rules below).  Otherwise, after placing the
 * (compressed) contents of the work buffer into a "write" buffer, the main
 * thread regards the work buffer as empty and starts to read more image pages
 * from the kernel.
 *
 * The second thread (call it the "move" thread) encrypts the contents of the
 * "write" buffers, one buffer at a time.  It really encrypts individual pages
 * and the encryption is not in-place, too.  The encrypted pages of data are
 * placed in yet another buffer (call it the "encrypt" buffer) until it's full,
 * in which case the "move" thread has to wait. Of course, it also has to wait
 * for data from the main thread if all of the "write" buffers are empty.
 * After encrypting an entire "write" buffer, the "move" thread progresses to
 * the next "write" buffer, in a round-robin manner.
 *
 * The synchronization between the main thread and the "move" thread is done
 * with the help of two index variables, move_start and move_end. �The rule
 * is that:
 * (1) the main thread can only put data into write_buffers[move_start],
 * (2) after putting data into write_buffers[move_start], the main thread
 *     increases move_start, modulo the number of "write" buffers, but
 *     move_start cannot be modified as long as the _next_ "write" buffer is
 *     write_buffers[move_end] (the thread has to wait if that happens),
 * (3) the "move" thread can only read data from write_buffers[move_end] and
 *     only if move_end != move_start (it has to wait if that's not the case),
 * (4) after reading data from write_buffers[move_end], the "move" thread
 *     increases move_end, modulo the number of "write" buffers.
 * This way, move_end always "follows" move_start and the threads don't access
 * the same buffer at any time.
 *
 * The third thread (call it the "save" thread) reads (encrypted) pages of data
 * from the "encrypt" buffer and writes them out to the swap.  This is done if
 * there are some pages to write in the "encrypt" buffer, otherwise the "save"
 * thread has to wait for the "move" thread to put more pages in there.
 *
 * The synchronization between the "move" thread and the "save" thread is done
 * with the help of two pointers, save_start and save_end, where save_start
 * points to the first empty page and save_end points to the last data page
 * that hasn't been written out yet.  Thus, the rule is:
 * (1) the "move" thread can only put data into the page pointed to by
 *     save_start,
 * (2) after putting data into the page pointed to by save_start, the "move"
 *     thread increases save_start, modulo the number of pages in the buffer,
 *     provided that the _next_ page is not the one pointed to by save_end (it
 *     has to wait if that happens),
 * (3) the "save" thread can only read from the page pointed to by save_end,
 *     as long as save_end != save_start (it has to wait if the two pointers
 *     are equal),
 * (4) after writing data from the page pointed to by save_end, the "save"
 *     thread increases save_end, modulo the number of pages in the buffer.
 * IOW, the "encrypt" buffer is handled as a typical circular buffer with one
 * producer (the "move" thread) and one consumer (the "save" thread).
 *
 * If encryption is not used, the "save" thread is not started and the "move"
 * thread writes data to the swap directly out of the "write" buffers.
 */

static int save_ret;
static pthread_mutex_t finish_mutex;
static pthread_cond_t finish_cond;

static char *encrypt_buf;
static char *save_start, *save_end;
static pthread_mutex_t save_mutex;
static pthread_cond_t save_cond;
static pthread_t save_th;

struct write_buffer {
	ssize_t size;
	void *start;
};

static struct write_buffer write_buffers[WRITE_BUFFERS];
static int move_start, move_end;
static pthread_mutex_t move_mutex;
static pthread_cond_t move_cond;
static pthread_t move_th;

#define FORCE_EXIT	1

static char *save_inc(char *ptr)
{
	return encrypt_buf +
		(((ptr - encrypt_buf) + page_size) % encrypt_buf_size);
}

static int move_inc(int index)
{
	return (index + 1) % WRITE_BUFFERS;
}

static int wait_for_finish(void)
{
	pthread_mutex_lock(&finish_mutex);
	while((save_end != save_start || move_start != move_end) && !save_ret)
		pthread_cond_wait(&finish_cond, &finish_mutex);
	pthread_mutex_unlock(&finish_mutex);
	return save_ret;
}

static void *save_thread(void *arg)
{
	struct swap_writer *handle = arg;
	int error = 0;

	for (;;) {
		/* Wait until there is a buffer ready for processing. */
		pthread_mutex_lock(&save_mutex);
		while(save_end == save_start && !save_ret)
			pthread_cond_wait(&save_cond, &save_mutex);
		pthread_mutex_unlock(&save_mutex);

		if (save_ret)
			return NULL;

		error = save_page(handle, save_end);
		if (error) {
			pthread_mutex_lock(&finish_mutex);
			if (!save_ret)
				save_ret = error;
			pthread_mutex_unlock(&finish_mutex);
			pthread_cond_signal(&move_cond);
			pthread_cond_signal(&save_cond);
			pthread_cond_signal(&finish_cond);
			return NULL;
		}

		/* Go to the next page */
		pthread_mutex_lock(&finish_mutex);
		pthread_mutex_lock(&save_mutex);
		save_end = save_inc(save_end);
		pthread_mutex_unlock(&save_mutex);
		pthread_mutex_unlock(&finish_mutex);

		pthread_cond_signal(&save_cond);
		pthread_cond_signal(&finish_cond);
	}

	return NULL;
}

#ifdef CONFIG_ENCRYPT

static void encrypt_and_save_buffer(void)
{
	char *src;
	ssize_t buf_size, moved_size;

	/*
	 * The buffer to process is at write_buffers[move_end].start and the
	 * size of it is write_buffers[move_end].size .
	 */
	src = write_buffers[move_end].start;
	buf_size = write_buffers[move_end].size;
	moved_size = 0;
	do {
		int error;
		void *next_start;

		/* Encrypt page_size of data. */
		error = gcry_cipher_encrypt(cipher_handle,
						save_start, page_size,
							src, page_size);
		if (error) {
			pthread_mutex_lock(&finish_mutex);
			if (!save_ret)
				save_ret = error;
			pthread_mutex_unlock(&finish_mutex);
			pthread_cond_signal(&move_cond);
			pthread_cond_signal(&save_cond);
			pthread_cond_signal(&finish_cond);
			break;
		}

		moved_size += page_size;
		src += page_size;

		pthread_mutex_lock(&save_mutex);
		next_start = save_inc(save_start);
		while (next_start == save_end && !save_ret)
			pthread_cond_wait(&save_cond, &save_mutex);
		save_start = next_start;
		pthread_mutex_unlock(&save_mutex);

		pthread_cond_signal(&save_cond);
	} while (moved_size < buf_size && !save_ret);
}

#else /* !CONFIG_ENCRYPT */

static inline void encrypt_and_save_buffer(void) {}

#endif /* !CONFIG_ENCRYPT */

static void save_buffer(struct swap_writer *handle)
{
	void *src;
	ssize_t size;

	/*
	 * The buffer to process is at write_buffers[move_end].start and the
	 * size of it is write_buffers[move_end].size .
	 */
	src = write_buffers[move_end].start;
	size = write_buffers[move_end].size;
	while (size > 0) {
		int error = save_page(handle, src);
		if (error) {
			pthread_mutex_lock(&finish_mutex);
			if (!save_ret)
				save_ret = error;
			pthread_mutex_unlock(&finish_mutex);
			pthread_cond_signal(&move_cond);
			pthread_cond_signal(&finish_cond);
			break;
		}
		src += page_size;
		size -= page_size;
	}
}

static void *move_thread(void *arg)
{
	struct swap_writer *handle = arg;

	for (;;) {
		/* Wait until there is a buffer ready for processing. */
		pthread_mutex_lock(&move_mutex);
		while(move_end == move_start && !save_ret)
			pthread_cond_wait(&move_cond, &move_mutex);
		pthread_mutex_unlock(&move_mutex);

		if (save_ret)
			break;

		if (do_encrypt)
			encrypt_and_save_buffer();
		else
			save_buffer(handle);

		if (save_ret)
			break;

		/* Tell the reader thread that we have processed the buffer */
		pthread_mutex_lock(&finish_mutex);
		pthread_mutex_lock(&move_mutex);
		move_end = move_inc(move_end);
		pthread_mutex_unlock(&move_mutex);
		pthread_mutex_unlock(&finish_mutex);

		pthread_cond_signal(&move_cond);
		pthread_cond_signal(&finish_cond);
	}

	return NULL;
}

static inline void *current_write_buffer(void)
{
	return write_buffers[move_start].start;
}

static int prepare_next_write_buffer(ssize_t size)
{
	int next_start;

	/* Move to the next buffer and signal that the current one is ready*/
	write_buffers[move_start].size = size;

	pthread_mutex_lock(&move_mutex);
	next_start = move_inc(move_start);
	while (next_start == move_end && !save_ret)
		pthread_cond_wait(&move_cond, &move_mutex);
	move_start = next_start;
	pthread_mutex_unlock(&move_mutex);

	pthread_cond_signal(&move_cond);

	return save_ret;
}

static void start_threads(struct swap_writer *handle)
{
	int error;
	unsigned int write_buf_size;
	char *write_buf;
	int j;

	encrypt_buf = handle->encrypt_buffer;
	save_start = encrypt_buf;
	save_end = save_start;

	write_buf_size = do_compress ? compress_buf_size : buffer_size;
	write_buf = handle->write_buffer;
	for (j = 0; j < WRITE_BUFFERS; j++) {
		write_buffers[j].start = write_buf;
		write_buf += write_buf_size;
	}
	move_start = 0;
	move_end = move_start;

	if (do_encrypt) {
		error = pthread_mutex_init(&save_mutex, NULL);
		if (error) {
			perror("pthread_mutex_init() failed:");
			goto Error_exit;
		}
		error = pthread_cond_init(&save_cond, NULL);
		if (error) {
			perror("pthread_cond_init() failed:");
			goto Destroy_save_mutex;
		}
	}

	error = pthread_mutex_init(&move_mutex, NULL);
	if (error) {
		perror("pthread_mutex_init() failed:");
		goto Destroy_save_cond;
	}
	error = pthread_cond_init(&move_cond, NULL);
	if (error) {
		perror("pthread_cond_init() failed:");
		goto Destroy_move_mutex;
	}

	if (do_encrypt) {
		error = pthread_create(&save_th, NULL, save_thread, handle);
		if (error) {
			perror("pthread_create() failed:");
			goto Destroy_move_cond;
		}
	}

	error = pthread_create(&move_th, NULL, move_thread, handle);
	if (error) {
		perror("pthread_create() failed:");
		goto Stop_save_thread;
	}

	error = pthread_mutex_init(&finish_mutex, NULL);
	if (error) {
		perror("pthread_mutex_init() failed:");
		goto Stop_move_thread;
	}
	error = pthread_cond_init(&finish_cond, NULL);
	if (error) {
		perror("pthread_cond_init() failed:");
		goto Destroy_finish_mutex;
	}

	return;

 Destroy_finish_mutex:
	pthread_mutex_destroy(&finish_mutex);

 Stop_move_thread:
	save_ret = FORCE_EXIT;
	pthread_cond_signal(&move_cond);
	pthread_join(move_th, NULL);

 Stop_save_thread:
	if (do_encrypt) {
		save_ret = FORCE_EXIT;
		pthread_cond_signal(&save_cond);
		pthread_join(save_th, NULL);
	}

 Destroy_move_cond:
	pthread_cond_destroy(&move_cond);
 Destroy_move_mutex:
	pthread_mutex_destroy(&move_mutex);

 Destroy_save_cond:
	if (do_encrypt)
		pthread_cond_destroy(&save_cond);
 Destroy_save_mutex:
	if (do_encrypt)
		pthread_mutex_destroy(&save_mutex);

 Error_exit:
	use_threads = 0;
}

static void stop_threads(void)
{
	pthread_mutex_lock(&finish_mutex);
	if (!save_ret)
		save_ret = FORCE_EXIT;
	pthread_mutex_unlock(&finish_mutex);

	pthread_cond_destroy(&finish_cond);
	pthread_mutex_destroy(&finish_mutex);

	pthread_cond_signal(&move_cond);
	pthread_join(move_th, NULL);
	if (do_encrypt) {
		pthread_cond_signal(&save_cond);
		pthread_join(save_th, NULL);
	}

	pthread_cond_destroy(&move_cond);
	pthread_mutex_destroy(&move_mutex);

	if (do_encrypt) {
		pthread_cond_destroy(&save_cond);
		pthread_mutex_destroy(&save_mutex);
	}
}

#else /* !CONFIG_THREADS */

static inline int wait_for_finish(void) { return -ENOSYS; }
static inline void *current_write_buffer(void) { return NULL; }
static inline int prepare_next_write_buffer(ssize_t size)
{
	(void)size;
	return -ENOSYS;
}
static inline void start_threads(struct swap_writer *handle) { (void)handle; }
static inline void stop_threads(void) {}

#endif /* !CONFIG_THREADS */

/**
 *	encrypt_and_save_page - encrypt a page of data and write it to the swap
 */
static int encrypt_and_save_page(struct swap_writer *handle, void *src)
{
#ifdef CONFIG_ENCRYPT
	if (do_encrypt) {
		int error = gcry_cipher_encrypt(cipher_handle,
			handle->encrypt_ptr, page_size, src, page_size);
		if (error)
			return error;
		src = handle->encrypt_ptr;
		handle->encrypt_ptr += page_size;
		if (handle->encrypt_ptr - handle->encrypt_buffer
		    >= encrypt_buf_size)
			handle->encrypt_ptr = handle->encrypt_buffer;
	}
#endif
	return save_page(handle, src);
}

/**
 *	flush_buffer - flush data stored in the buffer to the swap
 */
static int flush_buffer(struct swap_writer *handle)
{
	ssize_t size;
	char *src;
	int error = 0;

	/* Check if there is anything to do */
	if (handle->page_ptr <= handle->buffer)
		return 0;

	size = handle->page_ptr - handle->buffer;
	if (compute_checksum || verify_image)
		md5_process_block(handle->buffer, size, &handle->ctx);

	src = use_threads ? current_write_buffer() : handle->write_buffer;

	/* Compress the buffer, if necessary */
	if (do_compress) {
#ifdef CONFIG_COMPRESS
		struct buf_block *block = (struct buf_block *)src;
		lzo_uint cnt;

		lzo1x_1_compress(handle->buffer, size,
					(lzo_bytep)block->data, &cnt,
						handle->lzo_work_buffer);
		block->size = cnt;
		size = cnt + sizeof(size_t);
#endif
	} else if (use_threads) {
		memcpy(src, handle->buffer, size);
	}

	if (use_threads)
		return prepare_next_write_buffer(size);

	/*
	 * If there's no compression and threads are not used, handle->buffer is
	 * equal to handle->write_buffer.  In that case, the data are taken
	 * directly out of handle->buffer.
	 */
	while (size > 0) {
		error = encrypt_and_save_page(handle, src);
		if (error)
			break;
		src += page_size;
		size -= page_size;
	}

	return error;
}

/**
 *	save_image - save the hibernation image data
 */
static int save_image(struct swap_writer *handle, unsigned int nr_pages)
{
	unsigned int m, writeout_rate;
	ssize_t ret;
	struct termios newtrm, savedtrm;
	int abort_possible, key, error = 0;
	char message[SPLASH_GENERIC_MESSAGE_SIZE];

	/* Switch the state of the terminal so that we can read the keyboard
	 * without blocking and with no echo.
	 *
	 * stdin must be attached to the terminal now.
	 */
	abort_possible = !splash.prepare_abort(&savedtrm, &newtrm);

	sprintf(message, "Saving %u image data pages", nr_pages);
	if (abort_possible)
		strcat(message, " (press " ABORT_KEY_NAME " to abort) ");
	strcat(message, "...");
	printf("%s: %s     ", my_name, message);
	splash.set_caption(message);

	if (use_threads)
		start_threads(handle);

	m = nr_pages / 100;
	if (!m)
		m = 1;

	if (early_writeout)
		writeout_rate = m;
	else
		writeout_rate = nr_pages + 1;

	/* The buffer may be partially filled at this point */
	for (nr_pages = 0; ; nr_pages++) {
		ret = read(handle->input, handle->page_ptr, page_size);
		if (ret < page_size) {
			if (ret < 0) {
				error = -EIO;
				perror("\nError reading an image page");
			} else if (ret > 0) {
				error = -EFAULT;
				perror("\nShort read from /dev/snapshot?");
			}
			break;
		}

		handle->page_ptr += page_size;

		if (!(nr_pages % m)) {
			printf("\b\b\b\b%3d%%", nr_pages / m);
			splash.progress(20 + (nr_pages / m) * 0.75);

			while ((key = splash.key_pressed()) > 0) {
				switch (key) {
				case ABORT_KEY_CODE:
					if (abort_possible) {
						printf(" aborted!\n");
						error = -EINTR;
						goto Exit;
					}
					break;
				case REBOOT_KEY_CODE:
					printf (" reboot enabled\b\b\b\b\b\b\b"
						"\b\b\b\b\b\b\b\b");
					splash.set_caption("Reboot enabled");
					shutdown_method =
							SHUTDOWN_METHOD_REBOOT;
					break;
				}
			}
		}

		if (!((nr_pages + 1) % writeout_rate))
			start_writeout(handle->fd);

		if (handle->page_ptr - handle->buffer >= buffer_size) {
			/* The buffer is full, flush it */
			error = flush_buffer(handle);
			if (error)
				break;
			handle->page_ptr = handle->buffer;
		}
	}

	if (!error) {
		/* Flush whatever's left in the buffer and save the extents */
		error = flush_buffer(handle);
		if (use_threads)
			error = wait_for_finish();
		if (!error)
			error = save_extents(handle, 1);
		if (!error)
			printf(" done (%u pages)\n", nr_pages);
	}

 Exit:
	if (use_threads)
		stop_threads();

	if (abort_possible)
		splash.restore_abort(&savedtrm);

	return error;
}

/**
 *	enough_swap - Make sure we have enough swap to save the image.
 *
 *	Returns TRUE or FALSE after checking the total amount of swap
 *	space avaiable from the resume partition.
 */
static int enough_swap(struct swap_writer *handle)
{
	loff_t free_swap = check_free_swap(handle->dev);
	loff_t size = do_compress ?
			handle->swap_needed / 2 : handle->swap_needed;

	printf("%s: Free swap: %llu kilobytes\n", my_name,
		(unsigned long long)free_swap / 1024);
	return free_swap > size;
}

static struct swsusp_header swsusp_header;

static int mark_swap(int fd, loff_t start)
{
	int error = 0;
	unsigned int size = sizeof(struct swsusp_header);
	off64_t shift = ((off64_t)resume_offset + 1) * page_size - size;

	if (lseek64(fd, shift, SEEK_SET) != shift)
		return -EIO;

	if (read(fd, &swsusp_header, size) < size)
		return -EIO;

	if (!memcmp("SWAP-SPACE", swsusp_header.sig, 10) ||
	    !memcmp("SWAPSPACE2", swsusp_header.sig, 10)) {
		memcpy(swsusp_header.orig_sig, swsusp_header.sig, 10);
		memcpy(swsusp_header.sig, SWSUSP_SIG, 10);
		swsusp_header.image = start;
		if (lseek64(fd, shift, SEEK_SET) != shift)
			return -EIO;

		if (write(fd, &swsusp_header, size) < size)
			error = -EIO;
	} else {
		error = -ENODEV;
	}
	return error;
}

/**
 *	write_image - Write entire image and metadata.
 *	@snapshot_fd: File handle of the snapshot device
 *	@resume_fd: File handle of the swap device used for image saving
 *	@test_fd: (Optional) File handle of a file to read the image from
 *
 *	If @test_fd is not negative, the function works in the test mode in
 *	which the image is read from a regular file instead of the snapshot
 *	device.
 */
static int write_image(int snapshot_fd, int resume_fd, int test_fd)
{
	static struct swap_writer handle;
	struct image_header_info *header;
	loff_t start;
	loff_t image_size;
	double real_size;
	unsigned long nr_pages = 0;
	int error, test_mode = (test_fd >= 0);
	struct timeval begin;

	printf("%s: System snapshot ready. Preparing to write\n", my_name);
	/* Allocate a swap page for the additional "userland" header */
	start = get_swap_page(snapshot_fd);
	if (!start)
		return -ENOSPC;

	header  = getmem(page_size);
	memset(header, 0, page_size);

	error = init_swap_writer(&handle, snapshot_fd, resume_fd, test_fd);
	if (error)
		goto Exit;

	image_size = test_mode ? test_image_size : get_image_size(snapshot_fd);
	if (image_size > 0) {
		nr_pages = (unsigned long)((image_size + page_size - 1) /
						page_size);
	} else {
		/*
		 * The kernel doesn't allow us to get the image size via ioctl,
		 * so we need to read it from the image header.
		 */
		struct swsusp_info *image_header;
		ssize_t ret;

		/*
		 * Do it in such a way that save_image() will believe it has
		 * already read the header page.
		 */
		image_header = handle.page_ptr;
		ret = read(snapshot_fd, image_header, page_size);
		if (ret < page_size) {
			error = ret < 0 ? ret : -EFAULT;
			goto Free_writer;
		}
		handle.page_ptr += page_size;
		image_size = image_header->size;
		nr_pages = image_header->pages;
		if (!nr_pages) {
			error = -ENODATA;
			goto Free_writer;
		}
		/* We have already read one page */
		nr_pages--;
	}
	printf("%s: Image size: %lu kilobytes\n", my_name, (unsigned long) image_size / 1024);
	real_size = image_size;

	handle.swap_needed = image_size;
	if (do_compress) {
		/* This is necessary in case the image is not compressible */
		handle.swap_needed += round_up_page_size(
					(handle.swap_needed >> 4) + 67);
	}
	if (!enough_swap(&handle)) {
		fprintf(stderr, "%s: Not enough free swap\n", my_name);
		error = -ENOSPC;
		goto Free_writer;
	}
	if (!preallocate_swap(&handle)) {
		fprintf(stderr, "%s: Failed to allocate swap\n", my_name);
		error = -ENOSPC;
		goto Free_writer;
	}
	/* Shift handle.cur_offset for the first call to next_swap_page() */
	handle.cur_offset -= page_size;

	header->pages = nr_pages;
	header->flags = 0;
	header->map_start = handle.extents_spc;

	if (compute_checksum)
		header->flags |= IMAGE_CHECKSUM;

	if (do_compress)
		header->flags |= IMAGE_COMPRESSED;

#ifdef CONFIG_ENCRYPT
	if (!do_encrypt)
		goto Save_image;

	if (use_RSA) {
		error = gcry_cipher_setkey(cipher_handle, key_data.key,
						KEY_SIZE);
		if (error)
			goto No_RSA;

		error = gcry_cipher_setiv(cipher_handle, key_data.ivec,
						CIPHER_BLOCK);
		if (error)
			goto No_RSA;

		header->flags |= IMAGE_ENCRYPTED | IMAGE_USE_RSA;
		memcpy(&header->rsa, &key_data.rsa, sizeof(struct RSA_data));
		memcpy(&header->key, &key_data.encrypted_key,
						sizeof(struct encrypted_key));
	} else {
		int j;

No_RSA:
		encrypt_init(key_data.key, key_data.ivec, password);
		splash.progress(20);
		get_random_salt(header->salt, CIPHER_BLOCK);
		for (j = 0; j < CIPHER_BLOCK; j++)
			key_data.ivec[j] ^= header->salt[j];

		error = gcry_cipher_setkey(cipher_handle, key_data.key,
						KEY_SIZE);
		if (!error)
			error = gcry_cipher_setiv(cipher_handle, key_data.ivec,
						CIPHER_BLOCK);
		if (!error)
			header->flags |= IMAGE_ENCRYPTED;
	}

	if (error) {
		fprintf(stderr,"%s: libgcrypt error: %s\n", my_name,
			gcry_strerror(error));
		goto Free_writer;
	}

Save_image:
#endif
	gettimeofday(&begin, NULL);

	error = save_image(&handle, nr_pages);
	if (!error) {
		struct timeval end;

		fsync(resume_fd);

		header->image_data_size = handle.written_data;
		real_size = handle.written_data;

		/*
		 * NOTICE: This needs to go after save_image(), because the
		 * user may modify the behavior.
		 */
		if (shutdown_method == SHUTDOWN_METHOD_PLATFORM)
			header->flags |= PLATFORM_SUSPEND;

		if (compute_checksum || verify_image)
			md5_finish_ctx(&handle.ctx, header->checksum);

		gettimeofday(&end, NULL);
		timersub(&end, &begin, &end);
		header->writeout_time = end.tv_usec / 1000000.0 + end.tv_sec;

		header->resume_pause = resume_pause;

		error = write_page(resume_fd, header, start);
		fsync(resume_fd);
	}

 Free_writer:
	free_swap_writer(&handle);

	if (!error && (verify_image || test_mode)) {
		splash.progress(0);
		if (verify_image)
			printf("%s: Image verification\n", my_name);
		error = read_or_verify(snapshot_fd, resume_fd, header, start,
					verify_image, test_mode);
		if (verify_image)
			printf(error ? "%s: Image verification failed\n" :
					"%s: Image verified successfully\n",
					my_name);
		splash.progress(100);
	}

	if (!error) {
		if (do_compress) {
			printf("%s: Compression ratio %4.2lf\n", my_name,
				real_size / image_size);
		}
		printf("S");
		error = mark_swap(resume_fd, start);
		if (!error) {
			fsync(resume_fd);
			printf( "|" );
		}
		printf("\n");
	}

 Exit:
	freemem(header);

	return error;
}

static int reset_signature(int fd)
{
	int ret, error = 0;
	unsigned int size = sizeof(struct swsusp_header);
	off64_t shift = ((off64_t)resume_offset + 1) * page_size - size;

	if (lseek64(fd, shift, SEEK_SET) != shift)
		return -EIO;

	memset(&swsusp_header, 0, size);
	ret = read(fd, &swsusp_header, size);
	if (ret == size) {
		if (memcmp(SWSUSP_SIG, swsusp_header.sig, 10)) {
			/* Impossible? We wrote signature and it is not there?! */
			error = -EINVAL;
		}
	} else {
		error = ret < 0 ? ret : -EIO;
	}

	if (!error) {
		/* Reset swap signature now */
		memcpy(swsusp_header.sig, swsusp_header.orig_sig, 10);
		if (lseek64(fd, shift, SEEK_SET) == shift) {
			ret = write(fd, &swsusp_header, size);
			if (ret != size)
				error = ret < 0 ? ret : -EIO;
		} else {
			error = -EIO;
		}
	}
	fsync(fd);
	if (error) {
		fprintf(stderr, "%s: Error %d resetting the image.\n"
			"There should be valid image on disk. "
			"Powerdown and carry out normal resume.\n"
			"Continuing with this booted system "
			"will lead to data corruption.\n", my_name, error);
		while(1)
			sleep(10);
	}
	return error;
}

static void suspend_shutdown(int snapshot_fd)
{
	splash.set_caption("Done.");

	if (shutdown_method == SHUTDOWN_METHOD_REBOOT) {
		reboot();
	} else if (shutdown_method == SHUTDOWN_METHOD_PLATFORM) {
		if (platform_enter(snapshot_fd))
			suspend_error("Could not enter the hibernation state, "
					"calling power_off.");
	}
	power_off();
	/* Signature is on disk, it is very dangerous to continue now.
	 * We'd do resume with stale caches on next boot. */
	fprintf(stderr,"Powerdown failed. That's impossible.\n");
	while(1)
		sleep (60);
}

int suspend_system(int snapshot_fd, int resume_fd, int test_fd)
{
	loff_t avail_swap;
	loff_t image_size;
	int attempts, in_suspend, error = 0;
	char message[SPLASH_GENERIC_MESSAGE_SIZE];

	avail_swap = check_free_swap(snapshot_fd);
	if (avail_swap > pref_image_size)
		image_size = pref_image_size;
	else
		image_size = avail_swap;
	if (!avail_swap) {
		suspend_error("Not enough swap space for suspend");
		return ENOSPC;
	}

	error = freeze(snapshot_fd);

	/* This a hack for a bug in bootsplash. Apparently it will
	 * drop to 'verbose mode' after the freeze() call.
	 */
	splash.switch_to();
	splash.progress(15);

	if (error) {
		suspend_error("Freeze failed.");
		goto Unfreeze;
	}

	if (test_fd >= 0) {
		printf("%s: Running in test mode\n", my_name);
		error = write_image(snapshot_fd, resume_fd, test_fd);
		if (error)
			error = -error;
		reset_signature(resume_fd);
		free_swap_pages(snapshot_fd);
		goto Unfreeze;
	}

	if (shutdown_method == SHUTDOWN_METHOD_PLATFORM) {
		if (platform_prepare(snapshot_fd)) {
			suspend_error("Unable to use platform hibernation "
					"support, using shutdown mode.");
			shutdown_method = SHUTDOWN_METHOD_SHUTDOWN;
		}
	}

	sprintf(message, "Snapshotting system");
	printf("%s: %s\n", my_name, message);
	splash.set_caption(message);
	attempts = 2;
	do {
		if (set_image_size(snapshot_fd, image_size)) {
			printf("\e[13]");
			printf("MADHU: set_image_size failed\n");
			error = errno;
			break;
		}
		if (atomic_snapshot(snapshot_fd, &in_suspend)) {
			printf("\e[13]");
			printf("MADHU: atomic_snapshot failed\n");
			error = errno;
			break;
		}
		if (!in_suspend) {
			/* first unblank the console, see console_codes(4) */
			printf("\e[13]");
			printf("%s: returned to userspace\n", my_name);
			free_snapshot(snapshot_fd);
			break;
		}

		error = write_image(snapshot_fd, resume_fd, -1);
		if (error) {
			printf("\e[13]");
			printf("MADHU: write_image failed\n");
			free_swap_pages(snapshot_fd);
			free_snapshot(snapshot_fd);
			image_size = 0;
			error = -error;
			if (error != ENOSPC)
				break;
		} else {
			splash.progress(100);
#ifdef CONFIG_BOTH
			if (s2ram_kms || s2ram) {
				/* If we die (and allow system to continue)
				 * between now and reset_signature(), very bad
				 * things will happen. */
				error = suspend_to_ram(snapshot_fd);
				if (error)
					goto Shutdown;
				reset_signature(resume_fd);
				free_swap_pages(snapshot_fd);
				free_snapshot(snapshot_fd);
				if (!s2ram_kms)
					s2ram_resume();
				goto Unfreeze;
			}
Shutdown:
#endif
			close(resume_fd);
			suspend_shutdown(snapshot_fd);
		}
	} while (--attempts);

Unfreeze:
	/*
	 * We get here during the resume or when we failed to suspend.
	 * Remember, suspend_shutdown() never returns!
	 */
	unfreeze(snapshot_fd);
	return error;
}

/**
 *	console_fd - get file descriptor for given file name and verify
 *	if that's a console descriptor (based on the code of openvt)
 */
static inline int console_fd(const char *fname)
{
	int fd;
	char arg;

	fd = open(fname, O_RDONLY);
	if (fd < 0 && errno == EACCES)
		fd = open(fname, O_WRONLY);
	if (fd >= 0 && (ioctl(fd, KDGKBTYPE, &arg)
	    || (arg != KB_101 && arg != KB_84))) {
		close(fd);
		return -ENOTTY;
	}
	return fd;
}

#ifndef TIOCL_GETKMSGREDIRECT
#define TIOCL_GETKMSGREDIRECT	17
#endif

static int set_kmsg_redirect;

/**
 *	prepare_console - find a spare virtual terminal, open it and attach
 *	the standard streams to it.  The number of the currently active
 *	virtual terminal is saved via @orig_vc
 */
static int prepare_console(int *orig_vc, int *new_vc)
{
	int fd, error, vt = -1;
	char vt_name[GENERIC_NAME_SIZE];
	struct vt_stat vtstat;
	char clear_vt, tiocl[2];

	fd = console_fd("/dev/console");
	if (fd < 0)
		return fd;

	tiocl[0] = TIOCL_GETKMSGREDIRECT;
	if (!ioctl(fd, TIOCLINUX, tiocl)) {
		if (tiocl[0] > 0)
			vt = tiocl[0];
	}

	clear_vt = 0;
	error = ioctl(fd, VT_GETSTATE, &vtstat);
	if (!error) {
		*orig_vc = vtstat.v_active;
		if (vt < 0) {
			clear_vt = 1;
			error = ioctl(fd, VT_OPENQRY, &vt);
		}
	}

	close(fd);

	if (error || vt < 0)
		return -1;

	sprintf(vt_name, "/dev/tty%d", vt);
	fd = open(vt_name, O_RDWR);
	if (fd < 0)
		return fd;
	error = ioctl(fd, VT_ACTIVATE, vt);
	if (error) {
		suspend_error("Could not activate the VT %d.", vt);
		fflush(stderr);
		goto Close_fd;
	}
	error = ioctl(fd, VT_WAITACTIVE, vt);
	if (error) {
		suspend_error("VT %d activation failed.", vt);
		fflush(stderr);
		goto Close_fd;
	}

	if (clear_vt) {
		char *msg = "\33[H\33[J";
		write(fd, msg, strlen(msg));
	}

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	*new_vc = vt;

	set_kmsg_redirect = !tiocl[0];
	if (set_kmsg_redirect) {
		tiocl[0] = TIOCL_SETKMSGREDIRECT;
		tiocl[1] = vt;
		if (ioctl(fd, TIOCLINUX, tiocl)) {
			suspend_error("Failed to redirect kernel messages "
				"to VT %d.", vt);
			fflush(stderr);
			set_kmsg_redirect = 0;
		}
	}

	return fd;
Close_fd:
	close(fd);
	return error;
}

/**
 *	restore_console - switch to the virtual console that was active before
 *	                  suspend
 */
static void restore_console(int fd, int orig_vc)
{
	int error;

	error = ioctl(fd, VT_ACTIVATE, orig_vc);
	if (error) {
		suspend_error("Could not activate the VT %d.", orig_vc);
		fflush(stderr);
		goto Close_fd;
	}
	error = ioctl(fd, VT_WAITACTIVE, orig_vc);
	if (error) {
		suspend_error("VT %d activation failed.", orig_vc);
		fflush(stderr);
	}
	if (set_kmsg_redirect) {
		char tiocl[2];

		tiocl[0] = TIOCL_SETKMSGREDIRECT;
		tiocl[1] = 0;
		ioctl(fd, TIOCLINUX, tiocl);
	}
Close_fd:
	close(fd);
}

static FILE *swappiness_file;

static inline void open_swappiness(void)
{
	swappiness_file = fopen("/proc/sys/vm/swappiness", "r+");
}

static inline int get_swappiness(void)
{
	int swappiness = -1;

	if (swappiness_file) {
		rewind(swappiness_file);
		fscanf(swappiness_file, "%d", &swappiness);
	}
	return swappiness;
}

static inline void set_swappiness(int swappiness)
{
	if (swappiness_file) {
		rewind(swappiness_file);
		fprintf(swappiness_file, "%d\n", swappiness);
		fflush(swappiness_file);
	}
}

static inline void close_swappiness(void)
{
	if (swappiness_file)
		fclose(swappiness_file);
}

#ifdef CONFIG_ENCRYPT
static void generate_key(void)
{
	gcry_sexp_t rsa_pub, rsa_ciph, rsa_plain, rsa_mpi;
	gcry_mpi_t mpi[RSA_FIELDS_PUB];
	size_t size;
	int ret, fd, rnd_fd;
	struct RSA_data *rsa;
	unsigned char *buf;
	int mi;

	fd = open(key_name, O_RDONLY);
	if (fd < 0)
		return;

	rsa = &key_data.rsa;
	if (read(fd, rsa, sizeof(struct RSA_data)) <= 0)
		goto Close;

	buf = rsa->data;
	for (mi = 0; mi < RSA_FIELDS_PUB; mi++) {
		size_t s = rsa->size[mi];

		ret = gcry_mpi_scan(&mpi[mi], GCRYMPI_FMT_USG, buf, s, NULL);
		if (ret)
			break;

		buf += s;
	}
	if (ret) {
		if (mi)
			gcry_mpi_release(mpi[0]);
		fprintf(stderr, "Failed to read mpi[%i] from RSA key: %s\n", mi, gcry_strerror(ret));
		goto Close;
	}

	/* setup public key */
	ret = gcry_sexp_build(&rsa_pub, NULL,
		"(public-key (rsa"
		"(%s %m) (%s %m)"
		"))",
		rsa->field[0], mpi[0],
		rsa->field[1], mpi[1]
	);
	gcry_mpi_release(mpi[0]);
	gcry_mpi_release(mpi[1]);
	if (ret) {
		fprintf(stderr, "Failed to setup RSA public key: %s\n", gcry_strerror(ret));
		goto Close;
	}

	rnd_fd = open("/dev/urandom", O_RDONLY);
	if (rnd_fd <= 0)
		goto Destroy_pub;

	size = KEY_SIZE + CIPHER_BLOCK;
	for (;;) {
		unsigned char *res;
		size_t test;
		int cmp;

		if (read(rnd_fd, key_data.key, size) != size)
			goto Close_urandom;

		ret = gcry_mpi_scan(&mpi[0], GCRYMPI_FMT_USG, key_data.key, size, NULL);
		if (ret)
			continue;
		ret = gcry_mpi_aprint(GCRYMPI_FMT_USG, &res, &test, mpi[0]);
		if (ret)
			continue;
		cmp = memcmp(key_data.key, res, size);
		gcry_free(res);
		if (test == size && !cmp)
			break;
		gcry_mpi_release(mpi[0]);
	}

	/* setup plain text */
	ret = gcry_sexp_build(&rsa_plain, NULL,
		"(data (flags raw) (value %m))", mpi[0]);
	gcry_mpi_release(mpi[0]);
	if (ret) {
		fprintf(stderr, "Failed to setup plain text: %s\n", gcry_strerror(ret));
		goto Close_urandom;
	}

	/* encrypt the main key */
	ret = gcry_pk_encrypt(&rsa_ciph, rsa_plain, rsa_pub);
	if (ret) {
		fprintf(stderr, "Failed to encrypt the main key: %s\n", gcry_strerror(ret));
		goto Destroy_plain;
	}
	// retrieve a-value from S-expr (enc-val (rsa (a %m)))
	rsa_mpi = gcry_sexp_find_token(rsa_ciph, "a", 0);
	if (!rsa_mpi) {
		fputs("Can't find encrypted RSA S-token", stderr);
		ret = -EINVAL;
		goto Destroy_ciph;
	}
	// retrieve encrypted mpi (enc-val (rsa (a %m)))
	mpi[0] = gcry_sexp_nth_mpi(rsa_mpi, 1, GCRYMPI_FMT_USG);
	if (!mpi[0]) {
		fputs("Can't parse encrypted RSA S-token", stderr);
		ret = -EINVAL;
		goto Destroy_mpi;
	}

	struct encrypted_key *key = &key_data.encrypted_key;
	size_t s;

	ret = gcry_mpi_print(GCRYMPI_FMT_USG, key->data, KEY_DATA_SIZE,
				&s, mpi[0]);
	if (!ret) {
		key->size = s;
		use_RSA = 'y';
	} else {
		fputs("Can't parse encrypted RSA MPI", stderr);
		ret = -EINVAL;
	}

	gcry_mpi_release(mpi[0]);
Destroy_mpi:
	gcry_sexp_release(rsa_mpi);
Destroy_ciph:
	gcry_sexp_release(rsa_ciph);
Destroy_plain:
	gcry_sexp_release(rsa_plain);
Close_urandom:
	close(rnd_fd);
Destroy_pub:
	gcry_sexp_release(rsa_pub);
Close:
	close(fd);
}
#endif

static void unlock_vt(void)
{
	ioctl(vfd, VT_SETMODE, &orig_vtm);
	close(vfd);
}

static int lock_vt(void)
{
	struct sigaction sa;
	struct vt_mode vtm;
	struct vt_stat vtstat;
	char vt_name[GENERIC_NAME_SIZE];
	int fd, error;

	fd = console_fd("/dev/console");
	if (fd < 0)
		return fd;

	error = ioctl(fd, VT_GETSTATE, &vtstat);
	close(fd);
	
	if (error < 0)
		return error;

	sprintf(vt_name, "/dev/tty%d", vtstat.v_active);
	vfd = open(vt_name, O_RDWR);
	if (vfd < 0)
		return vfd;

	error = ioctl(vfd, VT_GETMODE, &vtm);
	if (error < 0) 
		return error;

	/* Setting vt mode to VT_PROCESS means this process
	 * will handle vt switching requests.
	 * We just ignore all request by installing SIG_IGN.
	 */
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGUSR1, &sa, NULL);

	orig_vtm = vtm;
	vtm.mode = VT_PROCESS;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR1;
	error = ioctl(vfd, VT_SETMODE, &vtm);
	if (error < 0)
		return error;

	return 0;
}

/* Parse the command line and/or configuration file */
//madhu: cannot be inline TODO
//static inline
int get_config(int argc, char *argv[])
{
	static struct option options[] = {
		   {
		       "help\0\t\t\tthis text",
		       no_argument,		NULL, 'h'
		   },
		   {
		       "version\0\t\t\tversion information",
		       no_argument,		NULL, 'V'
		   },
		   {
		       "config\0\t\talternative configuration file.",
		       required_argument,	NULL, 'f'
		   },
		   {
		       "resume_device\0device that contains swap area",	
		       required_argument,	NULL, 'r'
		   },
		   {
		       "resume_offset\0offset of swap file in resume device.",	
		       required_argument,	NULL, 'o'
		   },
		   {
		       "image_size\0\tdesired size of the image.",
		       required_argument,	NULL, 's'
		   },
		   {
		       "parameter\0\toverride config file parameter.",
		       required_argument,	NULL, 'P'
		   },
#ifdef CONFIG_BOTH
		   HACKS_LONG_OPTS
#endif
		   { NULL,		0,			NULL,  0 }
	};
	int i, error;
	char *conf_name = CONFIG_FILE;
	const char *optstring = "hVf:s:o:r:P:";
	struct stat64 stat_buf;
	int fail_missing_config = 0;

	/* parse only config file argument */
	while ((i = getopt_long(argc, argv, optstring, options, NULL)) != -1) {
		switch (i) {
		case 'h':
			usage(my_name, options, optstring);
			exit(EXIT_SUCCESS);
		case 'V':
			version(my_name, NULL);
			exit(EXIT_SUCCESS);
		case 'f':
			conf_name = optarg;
			fail_missing_config = 1;
			break;
		}
	}

	fprintf(stderr,"madhu: callin stat on %s\n", conf_name);
	fflush (stderr);

	if (stat64(conf_name, &stat_buf)) {
		if (fail_missing_config) {
			fprintf(stderr, "%s: Could not stat configuration file\n",
				my_name);
			fprintf(stderr, "madhu: config file=%s\n", conf_name);
			perror("perror:");
			return -ENOENT;
		}
	}
	else {
		error = parse(my_name, conf_name, parameters);
		if (error) {
			fprintf(stderr, "%s: Could not parse config file\n", my_name);
			return error;
		}
	}

	optind = 0;
	while ((i = getopt_long(argc, argv, optstring, options, NULL)) != -1) {
		switch (i) {
		case 'f':
			/* already handled */
			break;
		case 's':
			pref_image_size = atoll(optarg);
			break;
		case 'o':
			resume_offset = atoll(optarg);
			break;
		case 'r':
			strncpy(resume_dev_name, optarg, MAX_STR_LEN -1);
			break;
		case 'P':
			error = parse_line(optarg, parameters);
			if (error) {
				fprintf(stderr, "%s: Could not parse config string '%s'\n", my_name, optarg);
				return error;
			}
			break;
		default:
#ifdef CONFIG_BOTH
			s2ram_add_flag(i, optarg);
			break;
#else
			usage(my_name, options, optstring);
			return -EINVAL;
#endif
		}
	}

	if (optind < argc)
		strncpy(resume_dev_name, argv[optind], MAX_STR_LEN - 1);

#ifdef CONFIG_BOTH
	s2ram_kms = !s2ram_check_kms();
	if (s2ram_kms)
		return 0;

	s2ram = s2ram_is_supported();
	/* s2ram_is_supported returns EINVAL if there was something wrong
	 * with the options that where added with s2ram_add_flag.
	 * On any other error (unsupported) we will just continue with s2disk.
	 */
	if (s2ram == EINVAL)
		return -EINVAL;
	
	s2ram = !s2ram;
#endif

	return 0;
}

int main(int argc, char *argv[])
{
	unsigned int mem_size;
	struct stat stat_buf;
	int resume_fd, snapshot_fd, vt_fd, orig_vc = -1, suspend_vc = -1;
	int test_fd = -1;
	dev_t resume_dev;
	int orig_loglevel, orig_swappiness, ret;
	struct rlimit rlim;
	static char chroot_path[MAX_STR_LEN];

	my_name = basename(argv[0]);

	/* Make sure the 0, 1, 2 descriptors are open before opening the
	 * snapshot and resume devices
	 */
	do {
		ret = open("/dev/null", O_RDWR);
		if (ret < 0) {
			perror(argv[0]);
			return ret;
		}
	} while (ret < 3);
	close(ret);

	fprintf(stderr,"madhu: calling get_config\n");
	ret = get_config(argc, argv);
	if (ret) {
		fprintf(stderr,"madhu: get_config barfed\n");
		return -ret;
	}
	fprintf(stderr,"madhu: get_config done, resume_device=%s\n",resume_dev_name);

	if (compute_checksum != 'y' && compute_checksum != 'Y')
		compute_checksum = 0;
#ifdef CONFIG_COMPRESS
	if (do_compress != 'y' && do_compress != 'Y') {
		do_compress = 0;
	} else if (lzo_init() != LZO_E_OK) {
		suspend_error("Failed to initialize LZO. "
				"Compression disabled.\n");
		do_compress = 0;
	}
#endif
#ifdef CONFIG_ENCRYPT
	if (do_encrypt != 'y' && do_encrypt != 'Y')
		do_encrypt = 0;
#endif
	if (splash_param != 'y' && splash_param != 'Y')
		splash_param = 0;
	else
		splash_param = SPL_SUSPEND;

	if (early_writeout != 'n' && early_writeout != 'N')
		early_writeout = 1;

	if (!strcmp (shutdown_method_value, "shutdown")) {
		shutdown_method = SHUTDOWN_METHOD_SHUTDOWN;
	} else if (!strcmp (shutdown_method_value, "platform")) {
		shutdown_method = SHUTDOWN_METHOD_PLATFORM;
	} else if (!strcmp (shutdown_method_value, "reboot")) {
		shutdown_method = SHUTDOWN_METHOD_REBOOT;
	}

	if (resume_pause > RESUME_PAUSE_MAX)
		resume_pause = RESUME_PAUSE_MAX;

	if (verify_image != 'y' && verify_image != 'Y')
		verify_image = 0;

#ifdef CONFIG_THREADS
	if (use_threads != 'y' && use_threads != 'Y')
		use_threads = 0;
#endif

	get_page_and_buffer_sizes();

	mem_size = 2 * page_size + buffer_size;
#ifdef CONFIG_COMPRESS
	if (do_compress) {
		/*
		 * The formula below follows from the worst-case expansion
		 * calculation for LZO1 (size / 16 + 67) and the fact that the
		 * size of the compressed data must be stored in the buffer
		 * (sizeof(size_t)).
		 */
		compress_buf_size = buffer_size +
			round_up_page_size((buffer_size >> 4) + 67 +
						sizeof(size_t));
		mem_size += compress_buf_size +
				round_up_page_size(LZO1X_1_MEM_COMPRESS);
	}
#endif
#ifdef CONFIG_ENCRYPT
	if (do_encrypt) {
		printf("%s: libgcrypt version: %s\n", my_name,
			gcry_check_version(NULL));
		gcry_control(GCRYCTL_INIT_SECMEM, page_size, 0);
		ret = gcry_cipher_open(&cipher_handle, IMAGE_CIPHER,
				GCRY_CIPHER_MODE_CFB, GCRY_CIPHER_SECURE);
		if (ret) {
			suspend_error("libgcrypt error %s", gcry_strerror(ret));
			do_encrypt = 0;
		} else {
			encrypt_buf_size = ENCRYPT_BUF_PAGES * page_size;
			mem_size += encrypt_buf_size;
		}
	}
#endif
	if (use_threads) {
		mem_size += (compress_buf_size > 0) ?
				(WRITE_BUFFERS - 1) * compress_buf_size :
				WRITE_BUFFERS * buffer_size;
		if (!do_encrypt)
			mem_size += WRITE_BUFFERS * buffer_size;
	}

	ret = init_memalloc(page_size, mem_size);
	if (ret) {
		suspend_error("Could not allocate memory.");
		return ret;
	}

#ifdef CONFIG_ENCRYPT
	if (do_encrypt)
		generate_key();
#endif

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
		ret = errno;
		suspend_error("Could not lock myself.");
		return ret;
	}

	if (strlen(test_file_name) > 0) {
		if (stat(test_file_name, &stat_buf)) {
			ret = errno;
			suspend_error("Unable to stat test image file %s",
					test_file_name);
			return ret;
		}
		test_image_size = round_down_page_size(stat_buf.st_size);
		if (test_image_size < MIN_TEST_IMAGE_PAGES * page_size) {
			suspend_error("Test image file %s is too small",
					test_file_name);
			return ENODATA;
		}
		test_fd = open(test_file_name, O_RDONLY);
		if (test_fd < 0) {
			ret = errno;
			suspend_error("Unable to open test image file %s",
					test_file_name);
			return ret;
		}
	}

	/* If S3 resume fails /proc/<pid> never never gets unmounted. */
	//snprintf(chroot_path, MAX_STR_LEN, "/proc/%d", getpid());
	snprintf(chroot_path, MAX_STR_LEN, "/dev/shm/root", getpid());
	{
		struct stat buf;
		int ntries = 0;
	check_directory:
		if (lstat(chroot_path, &buf) == -1) {
			ret = errno;
			suspend_error("madhu: could not stat chroot target: %s.\n", chroot_path);
			if (++ntries == 1) {
				if (mkdir(chroot_path,0755) == -1) { 
					suspend_error("madhu: could not mkdir chroot target: %s.\n", chroot_path);
					ret = errno;
					return ret;
				}
				fprintf(stderr, "madhu: checking created chroot target: %s.\n", chroot_path);
				goto check_directory;
			}
			suspend_error("madhu: Could not create chroot target: %s.\n",chroot_path);
			return ret;
		} 
		if (!S_ISDIR(buf.st_mode)) {
			suspend_error("Invalid chroot target: %s.\n", chroot_path);
			ret = EINVAL;
			return ret;
		}
	}

	if (mount("none", chroot_path, "tmpfs", 0, NULL)) {
		ret = errno;
		suspend_error("Could not mount tmpfs on %s.", chroot_path);
		return ret;
	}

	fprintf(stderr,"madhu: callin stat on <<%s>>\n", resume_dev_name);
	fflush (stderr);

	ret = 0;
	if (stat(resume_dev_name, &stat_buf)) {
		suspend_error("Could not stat the resume device file.");
		fprintf(stderr, "madhu: resume device=%s\n", resume_dev_name);
		perror("perror:");
		ret = ENODEV;
		goto Umount;
	}
	if (!S_ISBLK(stat_buf.st_mode)) {
		suspend_error("Invalid resume device.");
		ret = EINVAL;
		goto Umount;
	}
	if (chdir(chroot_path)) {
		ret = errno;
		suspend_error("Could not change directory to %s.",
			chroot_path);
		goto Umount;
	}
	resume_dev = stat_buf.st_rdev;
	if (mknod("resume", S_IFBLK | 0600, resume_dev)) {
		ret = errno;
		suspend_error("Could not create %s/%s.", chroot_path, "resume");
		goto Umount;
	}
	resume_fd = open("resume", O_RDWR);
	if (resume_fd < 0) {
		ret = errno;
		suspend_error("Could not open the resume device.");
		goto Umount;
	}

	if (stat(snapshot_dev_name, &stat_buf)) {
		suspend_error("Could not stat the snapshot device file.");
		ret = ENODEV;
		goto Close_resume_fd;
	}

	if (!S_ISCHR(stat_buf.st_mode)) {
		suspend_error("Invalid snapshot device.");
		ret = EINVAL;
		goto Close_resume_fd;
	}
	snapshot_fd = open(snapshot_dev_name, O_RDONLY);
	if (snapshot_fd < 0) {
		ret = errno;
		suspend_error("Could not open the snapshot device.");
		goto Close_resume_fd;
	}

	if (set_swap_file(snapshot_fd, resume_dev, resume_offset)) {
		ret = errno;
		suspend_error("Could not use the resume device "
			"(try swapon -a).");
		goto Close_snapshot_fd;
	}

	vt_fd = prepare_console(&orig_vc, &suspend_vc);
	if (vt_fd < 0) {
		if (vt_fd == -ENOTTY) {
			suspend_warning("Unable to switch virtual terminals, "
					"using the current console.");
			splash_param = 0;
		} else {
			suspend_error("Could not open a virtual terminal.");
			ret = errno;
			goto Close_snapshot_fd;
		}
	}

	splash_prepare(&splash, splash_param);

	if (vt_fd >= 0) {
		if (lock_vt() < 0) {
			ret = errno;
			suspend_error("Could not lock the terminal.");
			goto Restore_console;
		}
	}

	splash.progress(5);

#ifdef CONFIG_BOTH
	/* If s2ram_hacks returns != 0, better not try to suspend to RAM */
	{int orig = s2ram;
	if (s2ram)
		s2ram = !s2ram_hacks();
	if (orig != s2ram)
	  suspend_error("madhu: s2ram hacks returned 0. not suspending");
	}

#endif
#ifdef CONFIG_ENCRYPT
        if (do_encrypt && ! use_RSA)
                splash.read_password(password, 1);
#endif

	open_printk();
	orig_loglevel = get_kernel_console_loglevel();
	set_kernel_console_loglevel(suspend_loglevel);

	open_swappiness();
	orig_swappiness = get_swappiness();
	set_swappiness(suspend_swappiness);

	sync();

	splash.progress(10);

	rlim.rlim_cur = 0;
	rlim.rlim_max = 0;
	setrlimit(RLIMIT_NOFILE, &rlim);
	setrlimit(RLIMIT_NPROC, &rlim);
	setrlimit(RLIMIT_CORE, &rlim);

	ret = suspend_system(snapshot_fd, resume_fd, test_fd);

	if (orig_loglevel >= 0)
		set_kernel_console_loglevel(orig_loglevel);

	close_printk();

	if(orig_swappiness >= 0)
		set_swappiness(orig_swappiness);
	close_swappiness();

	if (vt_fd >= 0)
		unlock_vt();
Restore_console:
	splash.finish();
	if (vt_fd >= 0)
		restore_console(vt_fd, orig_vc);
Close_snapshot_fd:
	close(snapshot_fd);
Close_resume_fd:
	close(resume_fd);
Umount:
	if (chdir("/")) {
		ret = errno;
		suspend_error("Could not change directory to /");
	} else {
		int mret = umount(chroot_path);
		if (mret < 0) {
			suspend_error("madhu: umount %s failed:\n",chroot_path);
		}
	}

	if (test_fd >= 0)
		close(test_fd);

#ifdef CONFIG_ENCRYPT
	if (do_encrypt)
		gcry_cipher_close(cipher_handle);
#endif
	free_memalloc();

	return ret;
}
