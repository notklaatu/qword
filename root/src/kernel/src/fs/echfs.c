#include <stdint.h>
#include <stddef.h>
#include <klib.h>
#include <fs.h>

#define SEARCH_FAILURE          0xffffffffffffffff
#define ROOT_ID                 0xffffffffffffffff
#define ENTRIES_PER_BLOCK       2
#define FILENAME_LEN            218
#define RESERVED_BLOCKS         16
#define FILE_TYPE               0
#define DIRECTORY_TYPE          1
#define DELETED_ENTRY           0xfffffffffffffffe
#define RESERVED_BLOCK          0xfffffffffffffff0
#define END_OF_CHAIN            0xffffffffffffffff

/* TODO ***de-hardcode this*** */
#define BYTES_PER_BLOCK         32768

#define CACHE_NOTREADY 0
#define CACHE_READY 1
#define CACHE_DIRTY 2

typedef struct {
    uint64_t parent_id;
    uint8_t type;
    char name[FILENAME_LEN];
    uint8_t perms;
    uint16_t owner;
    uint16_t group;
    uint64_t time;
    uint64_t payload;
    uint64_t size;
} __attribute__((packed)) entry_t;

typedef struct {
    uint64_t target_entry;
    entry_t target;
    entry_t parent;
    char name[FILENAME_LEN];
    int failure;
    int not_found;
} path_result_t;

typedef struct {
    char path[2048];
    path_result_t path_result;
    uint64_t cached_block;
    uint8_t cache[BYTES_PER_BLOCK];
    int cache_status;
    uint64_t *alloc_map;
} cached_file_t;

static cached_file_t *cached_files;
static int cached_files_ptr = 0;

typedef struct {
    char name[128];
    uint64_t blocks;
    uint64_t fatsize;
    uint64_t fatstart;
    uint64_t dirsize;
    uint64_t dirstart;
    uint64_t datastart;
    int device;
} mount_t;

static mount_t *mounts;
static int mounts_ptr = 0;

typedef struct {
    int free;
    int mnt;
    char path[1024];
    int flags;
    int mode;
    long ptr;
    long begin;
    long end;
    cached_file_t *cached_file;
} echfs_handle_t;

static echfs_handle_t* echfs_handles = (echfs_handle_t*)0;
static int echfs_handles_ptr = 0;

static int echfs_create_handle(echfs_handle_t handle) {
    int handle_n;

    // check for a free handle first
    for (int i = 0; i < echfs_handles_ptr; i++) {
        if (echfs_handles[i].free) {
            handle_n = i;
            goto load_handle;
        }
    }

    echfs_handles = krealloc(echfs_handles, (echfs_handles_ptr + 1) * sizeof(echfs_handle_t));
    handle_n = echfs_handles_ptr++;
    
load_handle:
    echfs_handles[handle_n] = handle;
    
    return handle_n;
}

static inline uint8_t rd_byte(int handle, uint64_t loc) {
    uint8_t buf[1];
    lseek(handle, loc, SEEK_SET);
    read(handle, buf, 1);
    return buf[0];
}

static inline void wr_byte(int handle, uint64_t loc, uint8_t val) {
    lseek(handle, loc, SEEK_SET);
    write(handle, (void *)&val, 1);
    return;
}

static inline uint16_t rd_word(int handle, uint64_t loc) {
    uint16_t buf[1];
    lseek(handle, loc, SEEK_SET);
    read(handle, buf, 2);
    return buf[0];
}

static inline void wr_word(int handle, uint64_t loc, uint16_t val) {
    lseek(handle, loc, SEEK_SET);
    write(handle, (void *)&val, 2);
    return;
}

static inline uint32_t rd_dword(int handle, uint64_t loc) {
    uint32_t buf[1];
    lseek(handle, loc, SEEK_SET);
    read(handle, buf, 4);
    return buf[0];
}

static inline void wr_dword(int handle, uint64_t loc, uint32_t val) {
    lseek(handle, loc, SEEK_SET);
    write(handle, (void *)&val, 4);
    return;
}

static inline uint64_t rd_qword(int handle, uint64_t loc) {
    uint64_t buf[1];
    lseek(handle, loc, SEEK_SET);
    read(handle, buf, 8);
    return buf[0];
}

static inline void wr_qword(int handle, uint64_t loc, uint64_t val) {
    lseek(handle, loc, SEEK_SET);
    write(handle, (void *)&val, 8);
    return;
}

static inline entry_t rd_entry(mount_t *mnt, uint64_t entry) {
    entry_t res;
    uint64_t loc = (mnt->dirstart * BYTES_PER_BLOCK) + (entry * sizeof(entry_t));
    lseek(mnt->device, loc, SEEK_SET);
    read(mnt->device, (void *)&res, sizeof(entry_t));
    
    return res;
}

static inline void wr_entry(mount_t *mnt, uint64_t entry, entry_t entry_src) {
    uint64_t loc = (mnt->dirstart * BYTES_PER_BLOCK) + (entry * sizeof(entry_t));
    lseek(mnt->device, loc, SEEK_SET);
    write(mnt->device, (void *)&entry_src, sizeof(entry_t));
    
    return;
}

static uint64_t search(mount_t *mnt, const char *name, uint64_t parent, uint8_t type) {
    entry_t entry;
    // returns unique entry #, SEARCH_FAILURE upon failure/not found
    for (uint64_t i = 0; ; i++) {
        entry = rd_entry(mnt, i);
        if (!entry.parent_id) return SEARCH_FAILURE;              // check if past last entry
        if (i >= (mnt->dirsize * ENTRIES_PER_BLOCK)) return SEARCH_FAILURE;  // check if past directory table
        if ((entry.parent_id == parent) && (entry.type == type) && (!kstrcmp(entry.name, name)))
            return i;
    }
}

static uint64_t get_free_id(mount_t *mnt) {
    uint64_t id = 1;
    uint64_t i;
    
    entry_t entry;

    for (i = 0; (entry = rd_entry(mnt, i)).parent_id; i++) {
        if ((entry.type == 1) && (entry.payload == id))
            id = (entry.payload + 1);
    }
    
    return id;
}

static path_result_t path_resolver(mount_t *mnt, const char *path, uint8_t type) {
    // returns a struct of useful info
    // failure flag set upon failure
    // not_found flag set upon not found
    // even if the file is not found, info about the "parent"
    // directory and name are still returned
    char name[FILENAME_LEN];
    entry_t parent = {0};
    int last = 0;
    int i;
    path_result_t result;
    entry_t empty_entry = {0};
    
    result.name[0] = 0;
    result.target_entry = 0;
    result.parent = empty_entry;
    result.target = empty_entry;
    result.failure = 0;
    result.not_found = 0;
    
    parent.payload = ROOT_ID;
    
    if ((type == DIRECTORY_TYPE) && !kstrcmp(path, "/")) {
        result.target.payload = ROOT_ID;
        return result; // exception for root
    }
    if ((type == FILE_TYPE) && !kstrcmp(path, "/")) {
        result.failure = 1;
        return result; // fail if looking for a file named "/"
    }
    
    if (*path == '/') path++;

next:    
    for (i = 0; *path != '/'; path++) {
        if (!*path) {
            last = 1;
            break;
        }
        name[i++] = *path;
    }
    name[i] = 0;
    path++;
    
    if (!last) {
        uint64_t search_res = search(mnt, name, parent.payload, DIRECTORY_TYPE);
        if (search_res == SEARCH_FAILURE) {
            result.failure = 1; // fail if search fails
            return result;
        }
        parent = rd_entry(mnt, search_res);
    } else {
        uint64_t search_res = search(mnt, name, parent.payload, type);
        if (search_res == SEARCH_FAILURE)
            result.not_found = 1;
        else {
            result.target = rd_entry(mnt, search_res);
            result.target_entry = search_res;
        }
        result.parent = parent;
        kstrcpy(result.name, name);
        return result;
    }
    
    goto next;
}

static int echfs_mount(const char *source) {
    /* open device */
    int device = open(source, O_RDWR, 0);

    /* verify signature */
    char signature[8];
    lseek(device, 4, SEEK_SET);
    read(device, signature, 8);
    if (kstrncmp(signature, "_ECH_FS_", 8)) {
        kprint(KPRN_ERR, "echidnaFS signature invalid, mount failed!");
        close(device);
        return -1;
    }
    
    mounts = krealloc(mounts, sizeof(mount_t) * (mounts_ptr + 1));

    mounts[mounts_ptr].device = device;
    kstrcpy(mounts[mounts_ptr].name, source);
    mounts[mounts_ptr].blocks = rd_qword(device, 12);
    mounts[mounts_ptr].fatsize = (mounts[mounts_ptr].blocks * sizeof(uint64_t)) / BYTES_PER_BLOCK;
    if ((mounts[mounts_ptr].blocks * sizeof(uint64_t)) % BYTES_PER_BLOCK) mounts[mounts_ptr].fatsize++;
    mounts[mounts_ptr].fatstart = RESERVED_BLOCKS;
    mounts[mounts_ptr].dirsize = rd_qword(device, 20);
    mounts[mounts_ptr].dirstart = mounts[mounts_ptr].fatstart + mounts[mounts_ptr].fatsize;
    mounts[mounts_ptr].datastart = RESERVED_BLOCKS + mounts[mounts_ptr].fatsize + mounts[mounts_ptr].dirsize;

    kprint(KPRN_DBG, "echfs mounted with:");
    kprint(KPRN_DBG, "blocks:        %U", mounts[mounts_ptr].blocks);
    kprint(KPRN_DBG, "fatsize:       %U", mounts[mounts_ptr].fatsize);
    kprint(KPRN_DBG, "fatstart:      %U", mounts[mounts_ptr].fatstart);
    kprint(KPRN_DBG, "dirsize:       %U", mounts[mounts_ptr].dirsize);
    kprint(KPRN_DBG, "dirstart:      %U", mounts[mounts_ptr].dirstart);
    kprint(KPRN_DBG, "datastart:     %U", mounts[mounts_ptr].datastart);

    return mounts_ptr++;
}

void init_echfs(void) {
    fs_t echfs = {0};

    kstrcpy(echfs.type, "echfs");
    echfs.mount = (void *)echfs_mount;

    vfs_install_fs(echfs);
}