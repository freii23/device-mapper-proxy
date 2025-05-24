/*
 * Device Mapper Proxy (DMP) - статистический прокси для блочных устройств
 * 
 * Создает виртуальные блочные устройства поверх существующих и собирает
 * статистику операций ввода-вывода.
 */

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#define DM_MSG_PREFIX "dmp"
#define DMP_VERSION "1.0"

/* Структура для хранения статистики */
struct dmp_stats {
    atomic64_t read_reqs;        /* Количество запросов на чтение */
    atomic64_t write_reqs;       /* Количество запросов на запись */
    atomic64_t read_bytes;       /* Общее количество прочитанных байт */
    atomic64_t write_bytes;      /* Общее количество записанных байт */
    spinlock_t lock;             /* Блокировка для синхронизации */
};

/* Контекст target'а */
struct dmp_c {
    struct dm_dev *dev;          /* Подлежащее устройство */
    sector_t start;              /* Начальный сектор */
    struct dmp_stats stats;      /* Статистика */
};

/* Глобальная статистика для всех устройств */
static struct dmp_stats global_stats;

/* Kobject для sysfs */
static struct kobject *dmp_kobj;

/*
 * Конструктор target'а
 */
static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct dmp_c *dmp;
    unsigned long long tmp;
    char dummy;
    int r;

    if (argc != 1) {
        ti->error = "Invalid argument count";
        return -EINVAL;
    }

    dmp = kzalloc(sizeof(*dmp), GFP_KERNEL);
    if (!dmp) {
        ti->error = "Cannot allocate context";
        return -ENOMEM;
    }

    /* Инициализация статистики */
    atomic64_set(&dmp->stats.read_reqs, 0);
    atomic64_set(&dmp->stats.write_reqs, 0);
    atomic64_set(&dmp->stats.read_bytes, 0);
    atomic64_set(&dmp->stats.write_bytes, 0);
    spin_lock_init(&dmp->stats.lock);

    /* Получение устройства */
    r = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dmp->dev);
    if (r) {
        ti->error = "Device lookup failed";
        goto bad;
    }

    dmp->start = 0;
    ti->private = dmp;
    ti->num_flush_bios = 1;
    ti->num_discard_bios = 1;

    return 0;

bad:
    kfree(dmp);
    return r;
}

/*
 * Деструктор target'а
 */
static void dmp_dtr(struct dm_target *ti)
{
    struct dmp_c *dmp = ti->private;

    dm_put_device(ti, dmp->dev);
    kfree(dmp);
}

/*
 * Обновление статистики
 */
static void dmp_update_stats(struct dmp_c *dmp, struct bio *bio)
{
    unsigned int bytes = bio->bi_iter.bi_size;
    
    if (bio_data_dir(bio) == READ) {
        atomic64_inc(&dmp->stats.read_reqs);
        atomic64_add(bytes, &dmp->stats.read_bytes);
        atomic64_inc(&global_stats.read_reqs);
        atomic64_add(bytes, &global_stats.read_bytes);
    } else {
        atomic64_inc(&dmp->stats.write_reqs);
        atomic64_add(bytes, &dmp->stats.write_bytes);
        atomic64_inc(&global_stats.write_reqs);
        atomic64_add(bytes, &global_stats.write_bytes);
    }
}

/*
 * Обработка bio запросов
 */
static int dmp_map(struct dm_target *ti, struct bio *bio)
{
    struct dmp_c *dmp = ti->private;

    /* Обновляем статистику */
    dmp_update_stats(dmp, bio);

    /* Перенаправляем bio на подлежащее устройство */
    bio_set_dev(bio, dmp->dev->bdev);
    if (bio_sectors(bio) || bio_op(bio) == REQ_OP_FLUSH)
        bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

    return DM_MAPIO_REMAPPED;
}

/*
 * Статус target'а
 */
static void dmp_status(struct dm_target *ti, status_type_t type,
                      unsigned status_flags, char *result, unsigned maxlen)
{
    struct dmp_c *dmp = ti->private;

    switch (type) {
    case STATUSTYPE_INFO:
        result[0] = '\0';
        break;

    case STATUSTYPE_TABLE:
        scnprintf(result, maxlen, "%s", dmp->dev->name);
        break;
    }
}

/*
 * Функции для sysfs
 */
static ssize_t volumes_show(struct kobject *kobj, struct kobj_attribute *attr,
                           char *buf)
{
    u64 read_reqs = atomic64_read(&global_stats.read_reqs);
    u64 write_reqs = atomic64_read(&global_stats.write_reqs);
    u64 read_bytes = atomic64_read(&global_stats.read_bytes);
    u64 write_bytes = atomic64_read(&global_stats.write_bytes);
    u64 total_reqs = read_reqs + write_reqs;
    u64 total_bytes = read_bytes + write_bytes;
    
    u64 avg_read_size = read_reqs ? read_bytes / read_reqs : 0;
    u64 avg_write_size = write_reqs ? write_bytes / write_reqs : 0;
    u64 avg_total_size = total_reqs ? total_bytes / total_reqs : 0;

    return sprintf(buf, 
        "read:\n"
        "  reqs: %llu\n"
        "  avg size: %llu\n"
        "write:\n"
        "  reqs: %llu\n"
        "  avg size: %llu\n"
        "total:\n"
        "  reqs: %llu\n"
        "  avg size: %llu\n",
        read_reqs, avg_read_size,
        write_reqs, avg_write_size,
        total_reqs, avg_total_size);
}

static struct kobj_attribute volumes_attribute = 
    __ATTR(volumes, 0444, volumes_show, NULL);

static struct attribute *attrs[] = {
    &volumes_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .name = "stat",
    .attrs = attrs,
};

/*
 * Определение target'а
 */
static struct target_type dmp_target = {
    .name   = "dmp",
    .version = {1, 0, 0},
    .module = THIS_MODULE,
    .ctr    = dmp_ctr,
    .dtr    = dmp_dtr,
    .map    = dmp_map,
    .status = dmp_status,
};

/*
 * Инициализация модуля
 */
static int __init dm_dmp_init(void)
{
    int r;

    /* Инициализация глобальной статистики */
    atomic64_set(&global_stats.read_reqs, 0);
    atomic64_set(&global_stats.write_reqs, 0);
    atomic64_set(&global_stats.read_bytes, 0);
    atomic64_set(&global_stats.write_bytes, 0);
    spin_lock_init(&global_stats.lock);

    /* Регистрация target'а */
    r = dm_register_target(&dmp_target);
    if (r < 0) {
        DMERR("register failed %d", r);
        return r;
    }

    /* Создание sysfs интерфейса */
    dmp_kobj = kobject_create_and_add("dmp", &THIS_MODULE->mkobj.kobj);
    if (!dmp_kobj) {
        DMERR("failed to create kobject");
        dm_unregister_target(&dmp_target);
        return -ENOMEM;
    }

    r = sysfs_create_group(dmp_kobj, &attr_group);
    if (r) {
        DMERR("failed to create sysfs group");
        kobject_put(dmp_kobj);
        dm_unregister_target(&dmp_target);
        return r;
    }

    DMINFO("version %s loaded", DMP_VERSION);
    return 0;
}

/*
 * Выгрузка модуля
 */
static void __exit dm_dmp_exit(void)
{
    sysfs_remove_group(dmp_kobj, &attr_group);
    kobject_put(dmp_kobj);
    dm_unregister_target(&dmp_target);
    DMINFO("version %s unloaded", DMP_VERSION);
}

module_init(dm_dmp_init);
module_exit(dm_dmp_exit);

MODULE_DESCRIPTION("Device Mapper Proxy with I/O statistics");
MODULE_AUTHOR("Rusin A.N.");
MODULE_LICENSE("GPL");
MODULE_VERSION(DMP_VERSION);
