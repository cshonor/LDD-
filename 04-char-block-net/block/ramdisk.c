// ramdisk.c —— 最小块设备例子（blk-mq 内存盘，4 MiB）
//
// 块设备没有 file_operations 式的"一次一调"读写。
// 应用 write 一个块设备时，数据先进页缓存（页缓存不属于你！），
// 内核块层把脏页攒成 request 塞进请求队列，最后才回调你的 queue_rq。
// 所以块驱动要实现的不是 read/write，而是「拿到 request → 搬运数据」。
//
// 本驱动：一块 4 MiB 的内存当盘（vzalloc），数据写入后断电即失，
// 但能 mkfs / mount / dd —— 块设备的所有"盘的样子"它都有。

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/vmalloc.h>

#define RB_SECTORS 8192          /* 8192 × 512B = 4 MiB */

static u8 *rb_data;              /* "盘面"：4 MiB 内存 */
static int rb_major;
static struct blk_mq_tag_set rb_tag_set;
static struct gendisk *rb_disk;

/* 把一个 request 里的所有 bio 段搬到（或搬出）我们的内存盘 */
static void rb_transfer(struct request *rq)
{
    struct bio_vec bvec;
    struct req_iterator iter;
    loff_t pos = (loff_t)blk_rq_pos(rq) << SECTOR_SHIFT;  /* 扇区 → 字节 */

    rq_for_each_segment(bvec, rq, iter) {
        void *kaddr = kmap_atomic(bvec.bv_page);   /* 页缓存页 → 内核虚地址 */
        void *dst  = rb_data + pos;
        bool is_write = op_is_write(req_op(rq));

        if (is_write)
            memcpy(dst, kaddr + bvec.bv_offset, bvec.bv_len);
        else
            memcpy(kaddr + bvec.bv_offset, dst, bvec.bv_len);

        kunmap_atomic(kaddr);
        pos += bvec.bv_len;
    }
}

/* 块层回调：一个 request 到了，处理完就还回去 */
static blk_status_t rb_queue_rq(struct blk_mq_hw_ctx *hctx,
                                const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;

    blk_mq_start_request(rq);      /* 告诉块层：开始处理了（计时） */
    rb_transfer(rq);
    blk_mq_end_request(rq, BLK_STS_OK);  /* 告诉块层：处理完了 */
    return BLK_STS_OK;
}

/* 6.17+ 起 map_queues 收整个 tag_set（旧版是 blk_mq_queue_map *） */
static void rb_map_queues(struct blk_mq_tag_set *set)
{
    blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);  /* 单队列全映到 hwq 0 */
}

static const struct blk_mq_ops rb_mq_ops = {
    .queue_rq    = rb_queue_rq,
    .map_queues  = rb_map_queues,
};

static const struct block_device_operations rb_fops = {
    .owner = THIS_MODULE,
    /* 注意：这里没有 .read/.write —— 块设备的数据通路在 queue_rq */
};

static int __init rb_init(void)
{
    int ret;

    rb_data = vzalloc(RB_SECTORS << SECTOR_SHIFT);
    if (!rb_data)
        return -ENOMEM;

    /* 1. 申请 tag set（请求队列的"工位"） */
    rb_tag_set.ops = &rb_mq_ops;
    rb_tag_set.nr_hw_queues = 1;
    rb_tag_set.queue_depth  = 32;
    rb_tag_set.numa_node    = NUMA_NO_NODE;
    /* 6.x 移除了 BLK_MQ_F_SHOULD_MERGE：合并现在由块层总是执行 */
    ret = blk_mq_alloc_tag_set(&rb_tag_set);
    if (ret)
        goto err_free;

    /* 2. 由 tag set 生出 gendisk + 请求队列
       （6.18 签名：set, queue_limits*, queuedata；NULL lim = 默认限制） */
    rb_disk = blk_mq_alloc_disk(&rb_tag_set, NULL, NULL);
    if (IS_ERR(rb_disk)) {
        ret = PTR_ERR(rb_disk);
        goto err_tags;
    }

    rb_major = register_blkdev(0, "lxx-ramdisk");  /* 0 = 自动分配 major */
    if (rb_major <= 0) {
        ret = -EBUSY;
        goto err_disk;
    }

    rb_disk->major      = rb_major;
    rb_disk->first_minor = 1;
    rb_disk->minors     = 1;
    strscpy(rb_disk->disk_name, "lxx-ramdisk", sizeof(rb_disk->disk_name));
    rb_disk->fops = &rb_fops;
    set_capacity(rb_disk, RB_SECTORS);

    /* 3. 上线：此后 /dev/lxx-ramdisk 出现，lsblk 可见 */
    ret = device_add_disk(NULL, rb_disk, NULL);
    if (ret)
        goto err_unreg;

    pr_info("ramdisk: /dev/lxx-ramdisk ready, 4MiB, major=%d\n", rb_major);
    return 0;

err_unreg:
    unregister_blkdev(rb_major, "lxx-ramdisk");
err_disk:
    put_disk(rb_disk);
err_tags:
    blk_mq_free_tag_set(&rb_tag_set);
err_free:
    vfree(rb_data);
    return ret;
}

static void __exit rb_exit(void)
{
    del_gendisk(rb_disk);
    put_disk(rb_disk);
    blk_mq_free_tag_set(&rb_tag_set);
    unregister_blkdev(rb_major, "lxx-ramdisk");
    vfree(rb_data);
    pr_info("ramdisk: unloaded\n");
}

module_init(rb_init);
module_exit(rb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("minimal blk-mq 4MiB ramdisk demo");
