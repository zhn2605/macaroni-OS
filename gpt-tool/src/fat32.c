#include "fat32.h"
#include "structures.h"
#include "utils.h"
#include "gpt_constants.h"

// Write esp
bool write_esp(FILE* image) {
    const uint8_t reserved_sectors = 32;
    Vbr vbr = {
        .BS_jmpBoot = { 0xEB, 0x00, 0x90 }, 
        .BS_OEMName  = { "THISDISK" },
        .BPB_BytesPerSec = LBA_SIZE,
        .BPB_SecPerClus = 1,
        .BPB_RsvdSecCnt = reserved_sectors,
        .BPB_NumFATs = 2,
        .BPB_RootEntCnt = 0,
        .BPB_TotSec16 = 0,
        .BPB_Media = 0xF8, // "Fixed" non-removable media
        .BPB_FATSz16 = 0,
        .BPB_SecPerTrk = 0,
        .BPB_NumHeads = 0,
        .BPB_HiddSec = esp_lba - 1,
        .BPB_TotSec32 = esp_size_lbas,
        .BPB_FATSz32 = ((ALIGNMENT / LBA_SIZE) - reserved_sectors) / 2,
        .BPB_ExtFlags = 0,
        .BPB_FSVer = 0,
        .BPB_RootClus = 2,
        .BPB_FSInfo = 1,
        .BPB_BkBootSec = 6,
        .BPB_Reserved = { 0 },
        .BS_DrvNum = 0x80,
        .BS_Reserved1 = 0,
        .BS_BootSig = 0x29,
        .BS_VolID = { 0 },
        .BS_VolLab = { "NO NAME    " },
        .BS_FilSysType = {"FAT32   "},

        .boot_code = { 0 },
        .bootsect_sig = 0xAA55
    };

    // fill out file system info sector
    FSInfo fsinfo = {
        .FSI_LeadSig = 0x41615252,
        .FSI_Reserved1 = {0},
        .FSI_StrucSig = 0x61417272,
        .FSI_Free_Count = 0xFFFFFFFF,
        .FSI_Next_Free = 5,             // First available cluster after /EFI/BOOT
        .FSI_Reserved2 = {0},
        .FSI_TrailSig = 0xAA550000,
    };

    fat32_fat_lba = esp_lba + vbr.BPB_RsvdSecCnt;
    fat32_data_lba = fat32_fat_lba + (vbr.BPB_NumFATs * vbr.BPB_FATSz32);

    // write vbr and fs info
    fseek(image, esp_lba * LBA_SIZE, SEEK_SET);
    if (fwrite(&vbr, 1, sizeof vbr, image) != sizeof vbr) {
        fprintf(stderr, "Error: Could not write ESP VBR to image\n");
        return false;
    }
    write_full_lba_size(image);

    if (fwrite(&fsinfo, 1, sizeof fsinfo, image) != sizeof fsinfo) {
        fprintf(stderr, "Error: Could not write ESP FSInfo to image\n");
        return false;
    }
    write_full_lba_size(image);

    // go to backup boot sector location
    // write vbr and fsinfo at back up
    fseek(image, (esp_lba  + vbr.BPB_BkBootSec) * LBA_SIZE, SEEK_SET);
    if (fwrite(&vbr, 1, sizeof vbr, image) != sizeof vbr) {
        fprintf(stderr, "Error: Could not write VBR to image\n");
        return false;
    }
    write_full_lba_size(image);

    if (fwrite(&fsinfo, 1, sizeof fsinfo, image) != sizeof fsinfo) {
        fprintf(stderr, "Error: Could not write ESP FSInfo to image\n");
        return false;
    }
    write_full_lba_size(image);
    
    // write FATs
    for (uint8_t i = 0; i < vbr.BPB_NumFATs; i++) {
        fseek(image, (fat32_fat_lba + (i*vbr.BPB_FATSz32)) * LBA_SIZE, SEEK_SET); 
        uint32_t cluster = 0;
        
        // cluster 0 reserved; FAT identifier, lowest 8 bits are media type
        cluster = 0xFFFFFF00 | vbr.BPB_Media;
        fwrite(&cluster, sizeof cluster, 1, image);

        // cluster 1; End of Chain marker
        cluster = 0xFFFFFFFF;
        fwrite(&cluster, sizeof cluster, 1, image);
        
        // cluster 2; Root dir cluster start
        cluster = 0xFFFFFFFF;
        fwrite(&cluster, sizeof cluster, 1, image);

        // cluster 3; '/EFI' dir cluster'
        cluster = 0xFFFFFFFF;
        fwrite(&cluster, sizeof cluster, 1, image);
        
        // cluster 4; '/EFI/BOOT' dir cluster
        cluster = 0xFFFFFFFF;
        fwrite(&cluster, sizeof cluster, 1, image);
        
        // cluster 5+; other files
    }

    // write file data
    fseek(image, fat32_data_lba * LBA_SIZE, SEEK_SET);

    // root directory
    FAT32_Dir_Entry_Short dir_ent = {
        .DIR_Name = { "EFI        " },
        .DIR_Attr = ATTR_DIRECTORY,
        .DIR_NTRes = 0,
        .DIR_CrtTimeTenth = 0,
        .DIR_CrtTime = 0,
        .DIR_CrtDate = 0,
        .DIR_LastAccDate = 0,   // Last access date
        .DIR_FstClusHI = 0,     // First cluster high
        .DIR_WrtTime = 0,
        .DIR_WrtDate = 0,
        .DIR_FstClusLO = 3,     // First cluster low
        .DIR_FileSize = 0,      // Directories have 0 file size
    };

    uint16_t create_time = 0, create_date = 0;
    get_fat_dir_entry_time_date(&create_time, &create_date);

    dir_ent.DIR_CrtTime = create_time;
    dir_ent.DIR_CrtTime = create_time;
    dir_ent.DIR_WrtTime = create_time;
    dir_ent.DIR_WrtDate = create_date;

    fwrite(&dir_ent, sizeof dir_ent, 1, image);
    
    // "/EFI" directory entries
    fseek(image, (fat32_data_lba + 1) * LBA_SIZE, SEEK_SET);
    
    memcpy(dir_ent.DIR_Name, ".          ", 11);    // "." dir entry, this directory itself
    fwrite(&dir_ent, sizeof dir_ent, 1, image);
    
    memcpy(dir_ent.DIR_Name, "..         ", 11);    // ".." dir entry, parent dir (ROOT dir)
    dir_ent.DIR_FstClusLO = 0;                      // Root directory does not have a cluster value
    fwrite(&dir_ent, sizeof dir_ent, 1, image);

    memcpy(dir_ent.DIR_Name, "BOOT       ", 11);    // /EFI/BOOT directory
    dir_ent.DIR_FstClusLO = 4;
    fwrite(&dir_ent, sizeof dir_ent, 1, image);

    // "/EFI/BOOT" directory
    fseek(image, (fat32_data_lba + 2) * LBA_SIZE, SEEK_SET);
    
    memcpy(dir_ent.DIR_Name, ".          ", 11);    // "." dir entry, this directory itself
    fwrite(&dir_ent, sizeof dir_ent, 1, image);
    
    memcpy(dir_ent.DIR_Name, "..         ", 11);    // ".." dir entry, parent dir (/EFI dir)
    dir_ent.DIR_FstClusLO = 3;                      // EFI directory cluster
    fwrite(&dir_ent, sizeof dir_ent, 1, image);

    return true;
}

// Helper function to compare null-terminated string with FAT32 directory name
static bool fat32_name_matches(const char dir_name[11], const char *str) {
    int str_len = strlen(str);
    if (str_len > 11) return false;
    
    // Compare the string part (case-insensitive)
    for (int i = 0; i < str_len; i++) {
        char dir_char = dir_name[i];
        char str_char = str[i];
        // Convert to uppercase for comparison
        if (str_char >= 'a' && str_char <= 'z') {
            str_char = str_char - 'a' + 'A';
        }
        if (dir_char != str_char) return false;
    }
    
    // Check that remaining bytes are spaces (0x20)
    for (int i = str_len; i < 11; i++) {
        if (dir_name[i] != ' ') return false;
    }
    
    return true;
}

// Helper function to format filename for FAT32 (8.3 format or space-padded for directories)
static void format_fat32_name(char fat_name[11], const char *filename, File_Type type) {
    memset(fat_name, ' ', 11);  // Fill with spaces
    
    if (type == TYPE_DIR) {
        // For directories, just copy the name and space-pad
        int len = strlen(filename);
        if (len > 11) len = 11;
        memcpy(fat_name, filename, len);
        // Convert to uppercase
        for (int i = 0; i < len; i++) {
            if (fat_name[i] >= 'a' && fat_name[i] <= 'z') {
                fat_name[i] = fat_name[i] - 'a' + 'A';
            }
        }
    } else {
        // For files, use 8.3 format
        const char *dot = strrchr(filename, '.');
        if (dot) {
            // Has extension
            int name_len = dot - filename;
            if (name_len > 8) name_len = 8;
            memcpy(fat_name, filename, name_len);
            
            int ext_len = strlen(dot + 1);
            if (ext_len > 3) ext_len = 3;
            memcpy(fat_name + 8, dot + 1, ext_len);
        } else {
            // No extension
            int name_len = strlen(filename);
            if (name_len > 8) name_len = 8;
            memcpy(fat_name, filename, name_len);
        }
        
        // Convert to uppercase
        for (int i = 0; i < 11; i++) {
            if (fat_name[i] >= 'a' && fat_name[i] <= 'z') {
                fat_name[i] = fat_name[i] - 'a' + 'A';
            }
        }
    }
}

uint32_t add_file_to_esp(char *file_name, FILE *image, File_Type type, uint32_t parent_dir_cluster) {
    // Get FAT32 fs info for VBR 
    Vbr vbr =  { 0 };
    fseek(image, esp_lba * LBA_SIZE, SEEK_SET);
    fread(&vbr, sizeof vbr, 1, image);

    FSInfo fsinfo = { 0 };
    fseek(image, (esp_lba + 1) * LBA_SIZE, SEEK_SET);
    fread(&fsinfo, sizeof fsinfo, 1, image);

    // Get file size if file
    FILE *new_file = NULL;
    uint64_t file_size_bytes = 0, file_size_lbas = 0;
    if (type == TYPE_FILE) {
        new_file = fopen(file_name, "rb");
        if (!new_file) return 0; // Return 0 to indicate error

        fseek(new_file, 0, SEEK_END);
        file_size_bytes = ftell(new_file);
        file_size_lbas = bytes_to_lbas(file_size_bytes);
        rewind(new_file);
    }

    // Get next free cluster in FATs
    uint32_t next_free_cluster = fsinfo.FSI_Next_Free;
    const uint32_t starting_cluster = next_free_cluster;

    // Add new clusters to FATs
    for (uint8_t i = 0; i < vbr.BPB_NumFATs; i++) {
        fseek(image, (fat32_fat_lba + (i * vbr.BPB_FATSz32)) * LBA_SIZE, SEEK_SET);
        fseek(image, next_free_cluster * sizeof next_free_cluster, SEEK_CUR);

        uint32_t cluster = fsinfo.FSI_Next_Free;
        next_free_cluster = cluster;
        if (type == TYPE_FILE) {
            for (uint64_t lba  = 0; lba < file_size_lbas - 1; lba++) {
                cluster++;  // Each cluster points to next cluster of file data
                next_free_cluster++;
                fwrite(&cluster, sizeof cluster, 1, image);
            }
        }

        // only cluster added for a directory, being the EOC marker
        cluster = 0xFFFFFFFF;
        next_free_cluster++;
        fwrite(&cluster, sizeof cluster, 1, image);
    }

    // Update next free cluster
    fsinfo.FSI_Next_Free = next_free_cluster;
    fseek(image, (esp_lba + 1) * LBA_SIZE, SEEK_SET);
    fwrite(&fsinfo, sizeof fsinfo, 1, image);

    // Go to Parent Directory's data location
    fseek(image, (fat32_data_lba + parent_dir_cluster - 2) * LBA_SIZE, SEEK_SET);
    
    // Add new directory entry for this new dir/file
    FAT32_Dir_Entry_Short dir_entry = { 0 };

    fread(&dir_entry, 1, sizeof dir_entry, image);
    while (dir_entry.DIR_Name[0] != '\0') {
        fread(&dir_entry, 1, sizeof dir_entry, image);
    }

    // size of dir_entry = 32, back up to overwrite empty spot
    fseek(image, -32, SEEK_CUR);        
   
    format_fat32_name((char *)dir_entry.DIR_Name, file_name, type);
    if (type == TYPE_DIR) {
        dir_entry.DIR_Attr = ATTR_DIRECTORY;
    }

    uint16_t fat_time, fat_date;
    get_fat_dir_entry_time_date(&fat_time, &fat_date);
    dir_entry.DIR_CrtTime = fat_time;
    dir_entry.DIR_CrtDate = fat_date;
    dir_entry.DIR_WrtTime = fat_time;
    dir_entry.DIR_WrtDate = fat_date;

    dir_entry.DIR_FstClusHI = (starting_cluster >> 16) & 0xFFFF;
    dir_entry.DIR_FstClusLO = starting_cluster & 0xFFFF;
    if (type == TYPE_FILE)
        dir_entry.DIR_FileSize = file_size_bytes;

    fwrite(&dir_entry, 1, sizeof dir_entry, image);

    // Go to new file's cluster's data location
    fseek(image, (fat32_data_lba + starting_cluster - 2) * LBA_SIZE, SEEK_SET);

    // Add new file data
    if (type == TYPE_DIR) {
        // Add "." and ".." for dir entry
        memcpy(dir_entry.DIR_Name, ".          ", 11);
        fwrite(&dir_entry, 1, sizeof dir_entry, image);

        memcpy(dir_entry.DIR_Name, "..         ", 11);
        dir_entry.DIR_FstClusHI = (parent_dir_cluster >> 16) & 0xFFFF;
        dir_entry.DIR_FstClusLO = parent_dir_cluster & 0xFFFF;
        fwrite(&dir_entry, 1, sizeof dir_entry, image);
    }
    else {
        // Add file data
        uint8_t *file_buf = calloc(1, LBA_SIZE);
        for (uint64_t i = 0; i < file_size_lbas; i++) {
            size_t bytes_read = fread(file_buf, 1, LBA_SIZE, new_file);     // read file data into buf, write buf into img
            fwrite(file_buf, 1, bytes_read, image);                         // reason for two parts is due to partial data
        }
        free(file_buf);
    }
    return starting_cluster;
}

bool add_path_to_esp(char *path, FILE *image) {
    if (*path != '/') return false; // Path must begin with root '/'

    File_Type type = TYPE_DIR;
    char *start = path + 1; // skip initial slash
    char *end = start;
    uint32_t dir_cluster = 2;   // Next directory's cluster location; start at root

    // Get next name from path until end of path
    while (type == TYPE_DIR) {
        // get next name from path
        while(*end != '/' && *end != '\0') end++;

        if (*end == '/') {
            type = TYPE_DIR;
        } 
        else {
            type = TYPE_FILE;
        }

        *end = '\0';        // Null terminates next name in caseof directory
        
        FAT32_Dir_Entry_Short dir_entry = { 0 };
        bool found = false;
        fseek(image, (fat32_data_lba + dir_cluster - 2) * LBA_SIZE, SEEK_SET);
        do {
            fread(&dir_entry, 1, sizeof dir_entry, image);
            if (fat32_name_matches((const char *)dir_entry.DIR_Name, start)) {
                // Found name in dir
                found = true;
                break;
            };
        } while (dir_entry.DIR_Name[0] != '\0');

        if (!found) {
            uint32_t new_cluster = add_file_to_esp(start, image, type, dir_cluster);
            if (new_cluster == 0)
                return false;
            // If we created a directory, update dir_cluster to navigate into it
            if (type == TYPE_DIR) {
                dir_cluster = new_cluster;
            }
        } else {
            // If we found an existing entry, update dir_cluster to navigate into it if it's a directory
            if (type == TYPE_DIR) {
                dir_cluster = (dir_entry.DIR_FstClusHI << 16) | dir_entry.DIR_FstClusLO;
            }
        }

        *end++ = '/';
        start = end;
    }

    printf("Added file '%s'\n", path);

    return true;
}
