// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fuse.h>
#include <iconv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS)

iconv_t iconv_utf16;
char *DEBUGFS_PATH = "/.debug";


static void
vfat_init(const char *dev) {
    struct fat_boot_header s;

    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
        err(1, "read super block");

    vfat_info.bytes_per_sector = s.bytes_per_sector;
    vfat_info.sectors_per_cluster = s.sectors_per_cluster;
    vfat_info.reserved_sectors = s.reserved_sectors;
    if (s.fat_count != 2) {
        err(1, "fat_count should be 2 (see presentation)");
    }
    vfat_info.fat_count = s.fat_count;
    if (s.root_max_entries) {
        err(1, "root_max_entries should be 0");
    }
    if (s.total_sectors_small) {
        err(1, "total_sectors_small should be 0");
    }

    if (s.sectors_per_fat_small) {
        err(1, "sectors_per_fat_small should be 0");
    }
    if (!s.total_sectors) {
        err(1, "total_sectors should not be 0");
    }
    if (!s.sectors_per_fat) {
        err(1, "sectors_per_fat cannot be 0");
    }
    vfat_info.sectors_per_fat = s.sectors_per_fat;
    vfat_info.fat_size = s.sectors_per_fat;
    if (s.version) {
        err(1, "version should be 0:0");
    }
    vfat_info.cluster_begin_offset = s.root_cluster;
    if (s.backup_sector != 6) {
        warn("backup sector is usually at position 6");
    }
    for (int i = 0; i < 12; ++i) {
        if (s.reserved2[i]) {
            err(1, "reserved should always be 0");
        }
    }
    uint32_t used_sectors = s.reserved_sectors + (s.fat_count * s.sectors_per_fat);
    if (s.total_sectors < used_sectors) {
        err(1, "filesystem is corrupted !");
    }
    uint32_t DataSec = s.total_sectors - used_sectors;
    uint32_t CountOfCluster = DataSec / s.sectors_per_cluster;
    if (CountOfCluster < 65525) {
        err(1, "filesystem is not FAT32");
    }
    vfat_info.fat_entries = CountOfCluster;
    vfat_info.fat_begin_offset = s.reserved_sectors;
    vfat_info.fat = mmap_file(vfat_info.fd, vfat_info.fat_begin_offset * s.bytes_per_sector, vfat_info.fat_size);
    if (NULL == vfat_info.fat) {
        err(1, "unable the load FAT in memory");
    }

    vfat_info.first_data_sector = vfat_info.reserved_sectors + (vfat_info.fat_count * vfat_info.fat_size);
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_mode = 0555 | S_IFDIR;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;

}

/* TODO: XXX add your code here */
int vfat_next_cluster(uint32_t cluster_num) {
    /* TODO: Read FAT to actually get the next cluster */
    if (cluster_num >= vfat_info.fat_entries) {
        return -1;
    }
    return vfat_info.fat[cluster_num];
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata) {
    struct stat st; // we can reuse same stat entry over and over again
    off_t err = 0;
    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    int cluster_number = first_cluster;
    size_t first_sector_of_cluster;
    off_t off = 0;
    int is_finished = 0;

    while (cluster_number != 0x0FFFFFFF && !is_finished) {
        first_sector_of_cluster = ((first_cluster - 2) * vfat_info.sectors_per_cluster) + vfat_info.first_data_sector;
        size_t entry_count = vfat_info.bytes_per_sector / sizeof(struct fat32_direntry);
        struct fat32_direntry sector[entry_count];
        for (size_t sector_number = first_sector_of_cluster;
             sector_number < first_sector_of_cluster + vfat_info.sectors_per_cluster && !is_finished; ++sector_number) {

            err = lseek(vfat_info.fd, sector_number * vfat_info.bytes_per_sector, SEEK_SET);
            if (err < 0) {
                return -1;
            }

            ssize_t byte_read = read(vfat_info.fd, sector, vfat_info.bytes_per_sector);
            if (byte_read != vfat_info.bytes_per_sector) {
                return -1;
            }

            struct fat32_direntry current;
            for (size_t entry = 0; entry < entry_count && !is_finished; ++entry) {

                current = sector[entry];

                if ((current.nameext[0] & 0xFF) == 0xE5) continue;
                else if ((current.attr >> 1) & 1) continue;
                else if ((current.nameext[0] & 0xFF) == 0x00) {
                    return 0;
                } else if ((current.nameext[11] & 0xFF) == 0x0F) {
                    continue;
                }

                st.st_size = current.size;
                st.st_ino = (((uint32_t) current.cluster_hi) << 16) | current.cluster_lo;

                // Attribute parsing
                st.st_mode = ((current.attr >> 4) & 1) ? S_IFDIR : S_IFREG;

                // Name parsing
                for (size_t i = 0; i < 8; i++) {
                    if (isspace(current.name[i])) {
                        current.name[i] = '\0';
                    }
                }
                for (size_t i = 0; i < 3; i++) {
                    if (isspace(current.ext[i])) {
                        current.ext[i] = '\0';
                    }
                }

                size_t nameLen = strnlen(current.name, 8);
                size_t extLen = strnlen(current.ext, 3);
                char fullname[nameLen + extLen + 2];

                strncpy(fullname, current.name, nameLen);
                strncpy(&fullname[nameLen + 1], current.ext, extLen);
                fullname[nameLen] = '.';
                fullname[nameLen + extLen + 1] = '\0';


                is_finished = callback(callbackdata, fullname, &st, off++);
            }
        }

        cluster_number = vfat_next_cluster(cluster_number);
        if (cluster_number < 0) {
            return cluster_number;
        }
    }

    /* XXX add your code here */
    return 0;
}

int vfat_read(uint32_t first_cluster, char *buf, size_t size, off_t offs, struct stat *st) {
    off_t remaining_byte = st->st_size - offs;
    size = remaining_byte < size ? remaining_byte : size;

    size_t sector_no = offs / vfat_info.bytes_per_sector;
    size_t cluster_no = sector_no / vfat_info.sectors_per_cluster;
    size_t bytes_per_cluster = vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector;

    int cluster_addr = first_cluster;

    for (int i = 0; i < cluster_no; ++i) {
        cluster_addr = vfat_next_cluster(cluster_addr);
        if (cluster_addr < 0) {
            return cluster_addr;
        }
    }

    ssize_t byte_read = 0;
    int finished = 0;
    while (cluster_addr != 0x0FFFFFFF && !finished) {
        //TODO relire méthode
        off_t offset_in_cluster = offs % bytes_per_cluster;
        size_t byte_remaining_in_cluster = bytes_per_cluster - (offs % bytes_per_cluster);
        size_t byte_to_read = remaining_byte < byte_remaining_in_cluster ? remaining_byte : byte_remaining_in_cluster;
        byte_to_read = byte_to_read < size ? byte_to_read : size;

        ssize_t err = pread(vfat_info.fd, buf + byte_read, byte_to_read, cluster_addr + offset_in_cluster);
        if (err < 0) {
            return err;
        }
        byte_read += err;
        offs += err;

        finished = byte_read == size;

        cluster_addr = vfat_next_cluster(cluster_addr);
        if (cluster_addr < 0) {
            return cluster_addr;
        }
    }

    return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    const char *name;
    int found;
    struct stat *st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs) {
    struct vfat_search_data *sd = data;

    if (strcmp(sd->name, name) != 0) return 0;

    sd->found = 1;
    *sd->st = *st;

    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
*/
int vfat_resolve(const char *path, struct stat *st) {
    /* TODO: Add your code here.
        You should tokenize the path (by slash separator) and then
        for each token search the directory for the file/dir with that name.
        You may find it useful to use following functions:
        - strtok to tokenize by slash. See manpage
        - vfat_readdir in conjuction with vfat_search_entry
    */
    int res = -ENOENT; // Not Found
    if (strcmp("/", path) == 0) {
        *st = vfat_info.root_inode;
        res = 0;
    }
    return res;
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st) {
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
}

// Extended attributes useful for debugging
int vfat_fuse_getxattr(const char *path, const char *name, char *buf, size_t size) {
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
        const char *path, void *callback_data,
        fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi) {
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
    */
    return 0;
}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused) {
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    /* TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
    */
    return 0;
}

////////////// No need to modify anything below this point
int vfat_opt_args(void *data, const char *args, int key, struct fuse_args *oargs) {
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(args);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
        .getattr = vfat_fuse_getattr,
        .getxattr = vfat_fuse_getxattr,
        .readdir = vfat_fuse_readdir,
        .read = vfat_fuse_read,
};

int test(void *buf, const char *name, const struct stat *stbuf, off_t off) {
    printf("Name is : \"%s\"\n", name);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);
    vfat_readdir(vfat_info.cluster_begin_offset, test, NULL);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
