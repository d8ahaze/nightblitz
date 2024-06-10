// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/blkdev.h>
#include <linux/numa.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/bio.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/highmem-internal.h>
#include <linux/string.h>

#define NTBZ_MAJOR 241
#define NTBZ_NAME "ntbz_raid"
#define KERNEL_SECTOR_SIZE 512
#define HW_SECT 4096

// TODO: Why when `#define code version(KVER1, KVER2, KVER3)` compiler complains
// about version expression, but in user space program doesn't?
// #define NTBZ_KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define NTBZ_KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define NTBZ_LINUX_VERSION_CODE 395520

static u64 nr_sect;

static char *vd_name = "ntbz_raid0";
module_param(vd_name, charp, S_IRUGO);

// physical drive
static char *pd[1];
module_param_array(pd, charp, NULL, 0);

// 0: kernel_read/write
// 1: bio, memcpy
#define RELAY_TYPE 1

struct ntbz_bd {
	struct gendisk *gd;
	struct request_queue *rq;
	struct blk_mq_tag_set ts;
	bool added;
	u8 *data;
	sector_t ss;	// size in sectors
	#if (RELAY_TYPE == 0)
	struct file *real_file;
	#else
	#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
	struct bdev_handle *real_bdev;
	#else
	struct file *bdev_file;
	struct block_device *real_bdev;
	#endif
	#endif
};

static struct ntbz_bd ntbz_bdev;

static int ntbz_open(struct gendisk *disk, blk_mode_t mode)
{
	pr_info("ntbz: stackable/relay: ntbz_open()\n");
	return 0;
}

static void ntbz_release(struct gendisk *disk)
{
	pr_info("ntbz: stackable/relay: ntbz_release()\n");
}

static struct block_device_operations ntbz_block_ops = {
	.owner = THIS_MODULE,
	.open = ntbz_open,
	.release = ntbz_release
};

#if (RELAY_TYPE == 0)
static int init_botdev(struct ntbz_bd *dev, const char *name)
#else
#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
static struct bdev_handle *open_disk(const char *name)
#else
static int init_botdev(struct ntbz_bd *dev, const char *path)
#endif
#endif
{
	pr_info("ntbz: stackable/relay: getting block device file\n");
	#if (RELAY_TYPE == 0)
	dev->real_file = filp_open(name, O_RDWR | O_SYNC, 0);
	if (IS_ERR(dev->real_file)) {
		pr_err("ntbz: stackable/relay: failed to open %s\n", name);
		return -1;
	}
	// dev->real_bdev = I_BDEV(dev->real_file->f_mapping->host);
	return 0;
	#else
	#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
	struct bdev_handle *bdev_hand = bdev_open_by_path(name,
		BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bdev_hand)) {
		pr_err("ntbz: stackable/relay: bdev_open_by_path()\n");
		return NULL;
	}
	return bdev_hand;
	#else
	struct file *bdev_file = bdev_file_open_by_path(path,
		BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bdev_file)) {
		pr_err("ntbz: stackable/relay: bdev_file_open_by_path()\n");
		return PTR_ERR(bdev_file);
		// return ERR_PTR(ret);
	}
	dev->bdev_file = bdev_file;
	dev->real_bdev = file_bdev(bdev_file);
	return 0;
	#endif
	#endif
}

static int serve_request(struct ntbz_bd *dev, struct request *req, u32 *nrb)
{
	if (dev->added == false)
		return 1;

	struct bio_vec bvec;
	struct req_iterator iter;
	#if (RELAY_TYPE == 0)
	struct file *real_file = dev->real_file;
	#else
	struct bio *bio_ptr;
	#endif

	sector_t sector = blk_rq_pos(req);
	pr_info("ntbz: ntbz_transfer_bio(): sector = %lld\n", sector);

	rq_for_each_segment(bvec, req, iter) {
		unsigned long b_len = bvec.bv_len;
		pr_info("ntbz: stackable/relay: len = %ld\n", b_len);
		void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

		if ((sector + b_len) > (dev->ss * KERNEL_SECTOR_SIZE))
			return 1;

		int bapret;
		if (rq_data_dir(req) == REQ_OP_WRITE) {
			pr_info("ntbz: stackable/relay: writing\n");
			memcpy(dev->data + sector, b_buf, b_len);
			#if (RELAY_TYPE == 0)
			if (real_file)
				kernel_write(real_file, b_buf, b_len, &sector);
			#else
			bio_ptr = bio_alloc(
			#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
				dev->real_bdev->bdev,
			#else
				dev->real_bdev,
			#endif
				1, REQ_OP_WRITE, GFP_KERNEL);
			#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
			bio_ptr->bi_bdev = dev->real_bdev->bdev;
			#else
			bio_ptr->bi_bdev = dev->real_bdev;
			#endif
			bio_ptr->bi_iter.bi_sector = blk_rq_pos(req);
			bapret = bio_add_page(bio_ptr, bvec.bv_page, b_len,
				bvec.bv_offset);
			pr_info("ntbz: stackable/relay: bio_add_page() = %d\n", bapret);
			submit_bio_wait(bio_ptr);
			bio_put(bio_ptr);
			#endif
		} else {
			pr_info("ntbz: stackable/relay: reading\n");
			#if (RELAY_TYPE == 1)
			#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
			bio_ptr = bio_alloc(dev->real_bdev->bdev, 1, REQ_OP_READ, GFP_KERNEL);
			#else
			bio_ptr = bio_alloc(dev->real_bdev, 1, REQ_OP_READ, GFP_KERNEL);
			#endif
			#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
			bio_ptr->bi_bdev = dev->real_bdev->bdev;
			#else
			bio_ptr->bi_bdev = dev->real_bdev;
			#endif
			bio_ptr->bi_iter.bi_sector = blk_rq_pos(req);
			bapret = bio_add_page(bio_ptr, bvec.bv_page, b_len, bvec.bv_offset);
			pr_info("ntbz: stackable/relay: bio_add_page() = %d\n", bapret);
			submit_bio_wait(bio_ptr);
			bio_put(bio_ptr);
			#endif
			memcpy(b_buf, dev->data + sector, b_len);
			#if (RELAY_TYPE == 0)
			if (real_file)
				kernel_read(real_file, b_buf, b_len, &sector);
			#endif
		}

		sector += b_len;
		nrb += b_len;
	}

	return 0;
}

static blk_status_t ntbz_queue_req(
	struct blk_mq_hw_ctx *hctx,
	const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct ntbz_bd *dev = hctx->queue->queuedata;
	blk_status_t status = BLK_STS_OK;
	u32 nrb = 0;

	blk_mq_start_request(req);

	if (blk_rq_is_passthrough(req)) {
		pr_info("Skip non-fs request\n");
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	if (serve_request(dev, req, &nrb))
		if (dev->added)
			status = BLK_STS_IOERR;

	bool update = blk_update_request(req, status, nrb);
	pr_info("ntbz: stackable/relay: ntbz_queue_req() { update = %d }\n",
		update);
	/*
	if (blk_update_request(req, status, nrb))
		if (dev->added)
			BUG();
	*/

	blk_mq_end_request(req, status);

	pr_info("ntbz: stackable/relay: status = %d\n", status);
	return status;
}

static struct blk_mq_ops ntbz_mq_ops = {
	.queue_rq = ntbz_queue_req,
};

static int ntbz_create_blkdev(struct ntbz_bd *dev)
{
	int rv;

	dev->added = false;

	dev->ts.ops = &ntbz_mq_ops;
	dev->ts.nr_hw_queues = 1;
	dev->ts.queue_depth = 128;
	dev->ts.numa_node = NUMA_NO_NODE;
	dev->ts.cmd_size = 0;
	dev->ts.flags = BLK_MQ_F_SHOULD_MERGE;
	rv = blk_mq_alloc_tag_set(&dev->ts);
	if (rv) {
		pr_err("Error: ntbz_create_blkdev() / blk_mq_alloc_tag_set(): %d\n", rv);
		return rv;
	}

	dev->gd = blk_mq_alloc_disk(&dev->ts, NULL, dev);
	if (dev->gd == NULL) {
		pr_err("Error: ntbz_create_blkdev() / ntbz_mq_alloc_disk()\n");
		return 1;
	}
	dev->rq = dev->gd->queue;
	blk_queue_logical_block_size(dev->rq, KERNEL_SECTOR_SIZE);
	dev->rq->queuedata = dev;

	dev->gd->major = NTBZ_MAJOR;
	dev->gd->first_minor = 0;
	dev->gd->minors = 1;
	dev->gd->fops = &ntbz_block_ops;
	snprintf(dev->gd->disk_name, strlen(vd_name), vd_name);
	pr_info("ntbz: stackable/relay: Calling get_capacity()\n");
	#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
	nr_sect = get_capacity(dev->real_bdev->bdev->bd_disk);
	#else
	nr_sect = get_capacity(dev->real_bdev->bd_disk);
	#endif
	pr_info("ntbz: ntbz_create_blkdev() / nr_sect = %lld\n", nr_sect);
	// set_capacity(dev->gd, nr_sect * (HW_SECT / KERNEL_SECTOR_SIZE));
	set_capacity(dev->gd, nr_sect);
	dev->ss = nr_sect;
	dev->gd->private_data = dev;

	pr_info("ntbz: stackable/relay: Allocating virtual block device\n");
	// dev->data = vmalloc(nr_sect * KERNEL_SECTOR_SIZE);
	dev->data = kmalloc(123 * KERNEL_SECTOR_SIZE, GFP_KERNEL);

	rv = add_disk(dev->gd);
	if (rv) {
		pr_err("Error: ntbz_create_blkdev() / add_disk(): %d\n", rv);
		return rv;
	}
	dev->added = true;

	return 0;
}

static int __init ntbz_init(void)
{
	int err = 0;

	if (pd[0] == NULL) {
		pr_err("ntbz: physical drive is required\n");
		return -EINVAL;
	}

	err = register_blkdev(NTBZ_MAJOR, NTBZ_NAME);
	if (err < 0) {
		pr_err("Error: ntbz_init() / register_blkdev(): %d\n", err);
		return err;
	}
	pr_info("ntbz: device is registered\n");

	#if (RELAY_TYPE == 0)
	err = init_botdev(&ntbz_bdev, pd[0]);
	if (err)
		return err;
	#else
	#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
	ntbz_bdev.real_bdev = open_disk(pd[0]);
	if (ntbz_bdev.real_bdev == NULL)
		return -EINVAL;
	#else
	err = init_botdev(&ntbz_bdev, pd[0]);
	if (err)
		return err;
	#endif
	#endif

	err = ntbz_create_blkdev(&ntbz_bdev);
	if (err) {
		pr_err("ntbz: stackable/relay: ntbz_create_blkdev(): %d\n", err);
		unregister_blkdev(NTBZ_MAJOR, NTBZ_NAME);
		return err;
	}
	pr_info("ntbz: device is created\n");

	return err;
}

static void ntbz_delete_blkdev(struct ntbz_bd *dev)
{
	if (dev->gd)
		del_gendisk(dev->gd);

	if (dev->rq)
		blk_mq_destroy_queue(dev->rq);

	if (dev->ts.tags)
		blk_mq_free_tag_set(&dev->ts);

	if (dev->data)
		kfree(dev->data);
}

static void __exit ntbz_exit(void)
{
	unregister_blkdev(NTBZ_MAJOR, NTBZ_NAME);
	#if (RELAY_TYPE == 1)
	#if (NTBZ_LINUX_VERSION_CODE < NTBZ_KERNEL_VERSION(6, 9, 0))
	bdev_release(ntbz_bdev.real_bdev);
	#else
	fput(ntbz_bdev.bdev_file);
	#endif
	#endif
	ntbz_delete_blkdev(&ntbz_bdev);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("d8ahaze");

module_init(ntbz_init);
module_exit(ntbz_exit);
