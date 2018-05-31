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
#include <stdlib.h>
#include <time.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"


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
    vfat_info.fat = mmap_file(vfat_info.fd, vfat_info.fat_begin_offset * s.bytes_per_sector,
                              vfat_info.fat_size * s.bytes_per_sector);
    if (NULL == vfat_info.fat) {
        err(1, "unable the load FAT in memory");
    }

    vfat_info.first_data_sector = vfat_info.reserved_sectors + (vfat_info.fat_count * vfat_info.fat_size);
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_dev = 0;
    vfat_info.root_inode.st_rdev = 0;
    vfat_info.root_inode.st_blksize = 0;
    vfat_info.root_inode.st_blocks = 1;
    // vfat_info.root_inode.st_mode =  S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    vfat_info.root_inode.st_mode = S_IFDIR | 0555;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;

}

/* TODO: XXX add your code here */
int vfat_next_cluster(uint32_t cluster_num) {
    return vfat_info.fat[cluster_num];
}

int construct_name(struct fat32_direntry_long *direntry, char *buff) {
    int size = 0;

    for (int i = 0; i < 5; ++i) {
        if ((buff[size++] = (char) direntry->name1[i]) == '\0') {
            return size;
        }
    }
    for (int i = 0; i < 6; ++i) {
        if ((buff[size++] = (char) direntry->name2[i]) == '\0') {
            return size;
        }
    }
    for (int i = 0; i < 2; ++i) {

        if ((buff[size++] = (char) direntry->name3[i]) == '\0') {
            return size;
        }
    }

    return size;
}

uint8_t compute_checksum(const char nameext[11]) {
    uint8_t sum = 0;

    int i;
    for (i = 0; i < 11; i++) {
        // NOTE: The operation is an unsigned char rotate right
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + nameext[i];
    }
    return sum;
}

time_t create_time(uint16_t date, uint16_t time) {
    struct tm time_info;
    memset(&time_info, 0, sizeof(struct tm));

    time_info.tm_mday = date & 0x01F;
    time_info.tm_mon = ((date >> 5) & 0x0F) - 1;
    time_info.tm_year = (date >> 9) + 1980 - 1900;

    time_info.tm_sec = (time & 0x1F) * 2;
    time_info.tm_min = (time >> 5) & 0x03F;
    time_info.tm_hour = (time >> 11) + 1;

    time_t result = mktime(&time_info);

    return result == -1 ? 0 : result;
}


int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata) {
    struct stat st; // we can reuse same stat entry over and over again
    off_t err = 0;
    memset(&st, 0, sizeof(st));

    int cluster_number = first_cluster;
    size_t first_sector_of_cluster;
    int is_finished = 0;

    char *fullname;
    int long_name_counter = 0;
    int in_long_name = 0;
    int skip_long_name = 0;
    uint8_t checksum;
    int our_counter = 0;
    while (cluster_number < 0x0FFFFFF8 && !is_finished) {

        first_sector_of_cluster = ((cluster_number - 2) * vfat_info.sectors_per_cluster) + vfat_info.first_data_sector;
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

                if ((current.nameext[0] & 0xFF) == 0x00) {
                    return 0;
                } else if ((current.nameext[0] & 0xFF) == 0xE5) continue;
                else if ((current.attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
                    if (!skip_long_name) {
                        struct fat32_direntry_long *currentLong = (struct fat32_direntry_long *) &current;
                        if (long_name_counter > 0) {
                            if (currentLong->seq != long_name_counter-- || currentLong->csum != checksum) {
                                skip_long_name = 1;
                                continue;
                            }
                            char first_entry_name[13];
                            int first_entry_name_size = construct_name(currentLong, first_entry_name);

                            strncpy(fullname + long_name_counter * 13, first_entry_name, first_entry_name_size);
                            continue;
                        } else {
                            if ((currentLong->seq & LAST_LONG_ENTRY) != LAST_LONG_ENTRY) {
                                skip_long_name = 1;
                                continue;
                            }
                            our_counter++;
                            checksum = currentLong->csum;
                            long_name_counter = (currentLong->seq & 0x0F) - 1;
                            in_long_name = 1;
                            char first_entry_name[13];
                            int first_entry_name_size = construct_name(currentLong, first_entry_name);
                            fullname = calloc(long_name_counter * 13 + first_entry_name_size, sizeof(char));
                            strncpy(fullname + long_name_counter * 13, first_entry_name, first_entry_name_size);
                            continue;
                        }
                    }
                } else if ((current.attr & ATTR_HIDDEN) == ATTR_HIDDEN) continue;
                else if (!in_long_name || compute_checksum(current.nameext) != checksum) {
                    // Name parsing
                    int finished = 0;
                    for (int i = 7; i >= 0 && !finished; i--) {
                        if (isspace(current.name[i])) {
                            current.name[i] = '\0';
                        } else {
                            finished = 1;
                        }
                    }
                    finished = 0;
                    for (int i = 2; i >= 0 && !finished; i--) {
                        if (isspace(current.ext[i])) {
                            current.ext[i] = '\0';
                        } else {
                            finished = 1;
                        }
                    }

                    size_t nameLen = strnlen(current.name, 8);
                    size_t extLen = strnlen(current.ext, 3);
                    fullname = calloc(nameLen + extLen + 2, sizeof(char));

                    strncpy(fullname, current.name, nameLen);
                    strncpy(&fullname[nameLen + 1], current.ext, extLen);
                    if (extLen != 0) {
                        fullname[nameLen] = '.';
                    }

                    fullname[nameLen + extLen + 1] = '\0';
                }
                long_name_counter = 0;
                in_long_name = 0;
                skip_long_name = 0;

                st.st_uid = vfat_info.mount_uid;
                st.st_gid = vfat_info.mount_gid;
                st.st_nlink = 1;
                st.st_rdev = 0;
                st.st_blksize = 0; // Ignored by FUSE
                st.st_blocks = 1;
                st.st_size = current.size;

                st.st_ino = (((uint32_t) current.cluster_hi) << 16) | current.cluster_lo;

                st.st_atime = create_time(current.atime_date, 0);
                st.st_ctime = create_time(current.ctime_date, current.ctime_time);
                st.st_mtime = create_time(current.mtime_date, current.mtime_time);

                // Attribute parsing
                if ((current.attr & ATTR_DIRECTORY) == ATTR_DIRECTORY) {
                    st.st_mode = S_IFDIR;
                    st.st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
                } else {
                    st.st_mode = S_IFREG;

                }
                st.st_mode |= S_IRUSR | S_IRGRP | S_IROTH;

                if ((current.attr & ATTR_READ_ONLY) != ATTR_READ_ONLY) {
                    st.st_mode |= S_IWUSR | S_IWGRP | S_IWOTH;
                }

                printf("File : %s has inode: %llu\n", fullname, st.st_ino);
                fflush(stdout);
                is_finished = callback(callbackdata, fullname, &st, 0);
                free(fullname);
            }
        }

        cluster_number = vfat_next_cluster(cluster_number);
        if (cluster_number < 0 && !is_finished) {
            return cluster_number;
        }
    }

    return 0;
}

int vfat_read(char *buf, size_t size, off_t offs, struct stat *st) {
    if (offs >= st->st_size) {
        return 0;
    }

    size_t remaining_byte = (size_t) st->st_size - offs;
    size = remaining_byte < size ? remaining_byte : size;

    size_t sector_no = offs / vfat_info.bytes_per_sector;
    size_t cluster_no = sector_no / vfat_info.sectors_per_cluster;
    size_t bytes_per_cluster = vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector;

    int cluster_addr = st->st_ino;

    for (int i = 0; i < cluster_no; ++i) {
        cluster_addr = vfat_next_cluster(cluster_addr);
    }

    ssize_t total_byte_read = 0;
    int finished = 0;
    while (cluster_addr < 0x0FFFFFF8 && !finished) {
        off_t offset_in_cluster = offs % bytes_per_cluster;
        size_t remaining_byte_in_cluster = bytes_per_cluster - offset_in_cluster;
        size_t byte_to_read = size < remaining_byte_in_cluster ? size : remaining_byte_in_cluster;

        size_t first_sector_of_cluster =
                ((cluster_addr - 2) * vfat_info.sectors_per_cluster) + vfat_info.first_data_sector;
        ssize_t byte_read = pread(vfat_info.fd, buf + total_byte_read, byte_to_read,
                                  first_sector_of_cluster * vfat_info.bytes_per_sector + offset_in_cluster);
        if (byte_read < 0) {
            return byte_read;
        }

        total_byte_read += byte_read;
        offs += byte_read;
        size -= byte_read;

        finished = total_byte_read == size;
        if (byte_read == byte_to_read) {
            cluster_addr = vfat_next_cluster(cluster_addr);
        }
    }
    return total_byte_read;
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

    int res = -ENOENT; // Not Found
    if (strcmp("/", path) == 0) {
        *st = vfat_info.root_inode;
        res = 0;
        return res;
    }

    size_t path_name_size = strlen(path);

    if (path_name_size > VFAT_MAX_PATH_LEN) {
        return res;
    }
    char path_copy[path_name_size + (path[0] == '/')];
    strncpy(path_copy, path + (path[0] == '/'), path_name_size);

    char *token;

    token = strtok(path_copy, "/");
    uint32_t curr_inode = vfat_info.root_inode.st_ino;
    struct vfat_search_data sd;
    sd.st = malloc(sizeof(struct stat));
    while (token != NULL) {
        sd.name = token;
        sd.found = 0;
        res = vfat_readdir(curr_inode, vfat_search_entry, &sd);
        if (res < 0 || !sd.found) {
            free(sd.st);
            return -ENOENT;
        }
        curr_inode = sd.st->st_ino;
        token = strtok(NULL, "/");
    }
    *st = *sd.st;
    printf("st.ino in resolve is %llu\n", st->st_ino);
    free(sd.st);

    return 0;
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
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
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
    struct stat st;
    vfat_resolve(path, &st);
    int val = vfat_readdir(st.st_ino, callback, callback_data);
    return val;
}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused) {
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    struct stat st;
    vfat_resolve(path, &st);

    return vfat_read(buf, size, offs, &st);
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

int main(int argc, char **argv) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);

    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
