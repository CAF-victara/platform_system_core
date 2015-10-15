/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <cutils/partition_utils.h>
#include <sys/mount.h>
#include "ext4_utils.h"
#include "ext4.h"
#include "make_ext4fs.h"
#include "fs_mgr_priv.h"
/* Avoid redefinition warnings */
#undef __le32
#undef __le16
#include <cryptfs.h>

/* These come from cryptfs.c */
#define CRYPT_KEY_IN_FOOTER "footer"
#define CRYPT_MAGIC         0xD0B5B1C4

#define F2FS_SUPER_MAGIC 0xF2F52010
#define EXT4_SUPER_MAGIC 0xEF53

#define INVALID_BLOCK_SIZE -1

int fs_mgr_is_partition_encrypted(struct fstab_rec *fstab)
{
    int fd = -1;
    struct stat statbuf;
    unsigned int sectors;
    off64_t offset;
    __le32 crypt_magic = 0;
    int ret = 0;

    if (!fs_mgr_is_encryptable(fstab))
        return 0;

    if (fstab->key_loc[0] == '/') {
        if ((fd = open(fstab->key_loc, O_RDWR)) < 0) {
            goto out;
        }
    } else if (!strcmp(fstab->key_loc, CRYPT_KEY_IN_FOOTER)) {
        if ((fd = open(fstab->blk_device, O_RDWR)) < 0) {
            goto out;
        }
        if ((ioctl(fd, BLKGETSIZE, &sectors)) == -1) {
            goto out;
        }
        offset = ((off64_t)sectors * 512) - CRYPT_FOOTER_OFFSET;
        if (lseek64(fd, offset, SEEK_SET) == -1) {
            goto out;
        }
    } else {
        goto out;
    }

    if (read(fd, &crypt_magic, sizeof(crypt_magic)) != sizeof(crypt_magic)) {
        goto out;
    }
    if (crypt_magic != CRYPT_MAGIC) {
        goto out;
    }

    /* It's probably encrypted! */
    ret = 1;

out:
    if (fd >= 0) {
        close(fd);
    }
    return ret;
}

/*
 * Search the first 16 sectors, or 4*4k blocks.  This covers the EXT4 alignment
 * requirement and will also find the F2FS backup SB.
 */
#define TOTAL_SECTORS 16
static int is_f2fs(char *block)
{
    __le32 *sb;
    int i;

    for (i = 0; i < TOTAL_SECTORS; i++) {
        sb = (__le32 *)(block + (i * 512));     /* magic is in the first word */
        if (le32_to_cpu(sb[0]) == F2FS_SUPER_MAGIC) {
            return 1;
        }
    }

    return 0;
}

static int is_ext4(char *block)
{
    struct ext4_super_block *sb = (struct ext4_super_block *)block;
    int i;

    for (i = 0; i < TOTAL_SECTORS * 512; i += sizeof(struct ext4_super_block), sb++) {
        if (le32_to_cpu(sb->s_magic) == EXT4_SUPER_MAGIC) {
            return 1;
        }
    }

    return 0;
}

/* Examine the superblock of a block device to see if the type matches what is
 * in the fstab entry.
 */
int fs_mgr_identify_fs(struct fstab_rec *fstab)
{
    char *block = NULL;
    int fd = -1;
    char rc = -1;

    block = calloc(1, TOTAL_SECTORS * 512);
    if (!block) {
        goto out;
    }
    if ((fd = open(fstab->blk_device, O_RDONLY)) < 0) {
        goto out;
    }
    if (read(fd, block, TOTAL_SECTORS * 512) != TOTAL_SECTORS * 512) {
        goto out;
    }

    if ((!strncmp(fstab->fs_type, "f2fs", 4) && is_f2fs(block)) ||
        (!strncmp(fstab->fs_type, "ext4", 4) && is_ext4(block))) {
        rc = 0;
    }

out:
    if (fd >= 0) {
        close(fd);
    }
    if (block) {
        free(block);
    }
    if (rc) {
        ERROR("Did not recognize file system type '%s' on %s\n", fstab->fs_type, fstab->blk_device);
    }
    return rc;
}

extern struct fs_info info;     /* magic global from ext4_utils */
extern void reset_ext4fs_info();

static int format_ext4(char *fs_blkdev, char *fs_mnt_point, int needs_footer)
{
    long int fs_blksize = INVALID_BLOCK_SIZE;
    unsigned int nr_sec;
    off64_t off;
    int fd, rc = 0;

#ifdef BOARD_USERIMAGE_BLOCK_SIZE
    fs_blksize = BOARD_USERIMAGE_BLOCK_SIZE;
#endif

    /* Ext2/3/4 only supports these block sizes, so make sure it is sane. */
    if (fs_blksize != INVALID_BLOCK_SIZE && (fs_blksize != 1024 &&
                                             fs_blksize != 2048 &&
                                             fs_blksize != 4096))
    {
        ERROR("Block size '%ld' not supported; using default\n", fs_blksize);
        fs_blksize = INVALID_BLOCK_SIZE;
    }

    /* Need to calculate the size to format. (Partition size - CRYPT_FOOTER_OFFSET) */
    if ((fd = open(fs_blkdev, O_RDWR)) < 0) {
        ERROR("Cannot open block device.  %s\n", strerror(errno));
        return -1;
    }
    if ((ioctl(fd, BLKGETSIZE, &nr_sec)) == -1) {
        ERROR("Cannot get block device size.  %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    off = ((off64_t)nr_sec * 512);

    if (needs_footer) {
        struct crypt_mnt_ftr crypt_ftr;

        INFO("Wiping old crypto info.\n");
        off -= CRYPT_FOOTER_OFFSET;
        memset (&crypt_ftr, 0, sizeof(crypt_ftr));
        if (lseek64(fd, off, SEEK_SET) == -1) {
            ERROR("Cannot seek to real block device footer.  %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        write(fd, &crypt_ftr, sizeof(struct crypt_mnt_ftr));
    }
    close(fd);

    /* Format the partition using the calculated length */
    reset_ext4fs_info();
    info.len = ((off64_t)nr_sec * 512);

    /* Use make_ext4fs_internal to avoid wiping an already-wiped partition. */
    rc = make_ext4fs_internal(fd, NULL, NULL, fs_mnt_point, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    if (rc) {
        ERROR("make_ext4fs returned %d.\n", rc);
    }
    close(fd);

    return rc;
}

static int format_f2fs(char *fs_blkdev, int needs_footer)
{
    char * args[5];
    char footer_size[10];
    int pid;
    int rc = 0;
    int footer = needs_footer ? CRYPT_FOOTER_OFFSET : 0;

    snprintf(footer_size, sizeof(footer_size), "%d", footer);
    args[0] = (char *)"/sbin/mkfs.f2fs";
    args[1] = (char *)"-r";
    args[2] = footer_size;
    args[3] = fs_blkdev;
    args[4] = (char *)0;
    if (!(pid = fork())) {
        /* This doesn't return */
        execv("/sbin/mkfs.f2fs", args);
        exit(1);
    }
    for(;;) {
        pid_t p = waitpid(pid, &rc, 0);
        if (p != pid) {
            ERROR("Error waiting for child process - %d\n", p);
            rc = -1;
            break;
        }
        if (WIFEXITED(rc)) {
            rc = WEXITSTATUS(rc);
            INFO("%s done, status %d\n", args[0], rc);
            if (rc) {
                rc = -1;
            }
            break;
        }
        ERROR("Still waiting for %s...\n", args[0]);
    }

    return rc;
}

int fs_mgr_do_format(struct fstab_rec *fstab)
{
    int rc = -EINVAL;

    int needs_footer = fstab->key_loc && !strcmp(fstab->key_loc, CRYPT_KEY_IN_FOOTER);

    ERROR("Formatting %s as '%s'%s.\n", fstab->blk_device, fstab->fs_type,
        needs_footer ? ", with footer" : "");

    if (!strncmp(fstab->fs_type, "f2fs", 4)) {
        rc = format_f2fs(fstab->blk_device, needs_footer);
    } else if (!strncmp(fstab->fs_type, "ext4", 4)) {
        rc = format_ext4(fstab->blk_device, fstab->mount_point, needs_footer);
    } else {
        ERROR("File system type '%s' is not supported\n", fstab->fs_type);
    }

    return rc;
}
