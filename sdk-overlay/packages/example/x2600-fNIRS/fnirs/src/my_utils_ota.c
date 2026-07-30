/*
 *  my_utils_crc.c
 *  Linux POSIX 实现，CRC16算法
 */


#include "my_interface.h"

/**
 * @brief 判断文件名是否以".bin"结尾
 * @param filename 文件名
 * @return 1: 是.bin文件；0: 不是
 */
static int is_bin_file(const char *filename) {
    if (filename == NULL) {
        return 0;
    }
    // 查找最后一个'.'的位置
    const char *dot = strrchr(filename, '.');
    // 若存在'.'，且后面是"bin"（长度需至少4：如"a.bin"）
    if (dot != NULL && (dot - filename) >= 1 && strcmp(dot + 1, "bin") == 0) {
        return 1;
    }
    return 0;
}

/**
 * @brief 遍历目录下所有.bin文件
 * @param dir_path 目标目录路径（如"/tmp"）
 * @return 成功返回找到的.bin文件数量；失败返回-1
 */
int traverse_bin_files(const char *dir_path) {
    if (dir_path == NULL) {
        fprintf(stderr, "Error: dir_path is NULL\n");
        return -1;
    }

    // 打开目录
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("opendir failed"); // 打印错误原因（如目录不存在、权限不足）
        return -1;
    }

    struct dirent *entry;
    int bin_count = 0;

    // 循环读取目录项
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录（.）和上级目录（..）
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // （可选）精确判断是否为普通文件（排除目录、符号链接等）
        // 注：嵌入式环境中dirent.d_type可能不支持（返回DT_UNKNOWN），需谨慎使用
        #if defined(DT_REG)
        if (entry->d_type != DT_REG) {
            continue; // 只处理普通文件
        }
        #endif

        // 检查是否为.bin文件
        if (is_bin_file(entry->d_name)) {
            // 拼接完整路径（可选，根据需求处理）
            char full_path[256]; // 嵌入式环境路径长度有限，可按需调整
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            full_path[sizeof(full_path) - 1] = '\0'; // 确保字符串结束

            // 此处可添加自定义处理（如打印、存储路径等）
            printf("Found bin file: %s\n", full_path);
            bin_count++;
        }
    }

    // 检查readdir是否因错误退出
    if (closedir(dir) != 0) {
        perror("closedir failed");
        return -1;
    }

    return bin_count;
}



/**
 * @brief 遍历目录下是否存在.bin文件，且返回第一个.bin文件路径
 * @param dir_path 目标目录路径（如"/tmp"）
 * @param ota_bin_path 目标文件路径缓存
 * @param path_len 目标文件路径缓存大小
 * @return 成功返回找到的.bin文件数量；失败返回-1
 */
int traverse_ota_bin_files(const char *dir_path, char *ota_bin_path, int path_len) {
    if (dir_path == NULL) {
        fprintf(stderr, "Error: dir_path is NULL\n");
        return -1;
    }

    // 打开目录
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("opendir failed"); // 打印错误原因（如目录不存在、权限不足）
        return -1;
    }

    struct dirent *entry;
    int bin_count = 0;

    // 循环读取目录项
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录（.）和上级目录（..）
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // （可选）精确判断是否为普通文件（排除目录、符号链接等）
        // 注：嵌入式环境中dirent.d_type可能不支持（返回DT_UNKNOWN），需谨慎使用
        #if defined(DT_REG)
        if (entry->d_type != DT_REG) {
            continue; // 只处理普通文件
        }
        #endif

        // 检查是否为.bin文件，且只保存第一个.bin文件
        if (is_bin_file(entry->d_name) && bin_count == 0) {
            // 拼接完整路径（可选，根据需求处理）
            //char full_path[256]; // 嵌入式环境路径长度有限，可按需调整
            snprintf(ota_bin_path, path_len, "%s/%s", dir_path, entry->d_name);
            ota_bin_path[path_len - 1] = '\0'; // 确保字符串结束

            // 此处可添加自定义处理（如打印、存储路径等）
            printf("Found bin file: %s\n", ota_bin_path);
            bin_count++;

			break;
        }
    }

    // 检查readdir是否因错误退出
    if (closedir(dir) != 0) {
        perror("closedir failed");
        return -1;
    }

    return bin_count;
}

#define PACKET_SIZE 56          // 分包大小：56字节

/**
 * @brief 按56字节分包发送文件内容
 * @param bin_path .bin文件路径
 * @param serial_fd 串口文件描述符
 * @return 成功返回发送的总字节数；失败返回-1
 */
ssize_t send_bin_test(const char *bin_path) {
    if (!bin_path) {
        fprintf(stderr, "Invalid bin path or serial fd\n");
        return -1;
    }

    // 1. 获取文件大小
    struct stat file_stat;
    if (stat(bin_path, &file_stat) < 0) {
        perror("stat file failed");
        return -1;
    }
    off_t file_size = file_stat.st_size;
    printf("Found .bin file: %s, size: %ld bytes\n", bin_path, (long)file_size);

    // 2. 打开.bin文件（只读、二进制模式）
    int bin_fd = open(bin_path, O_RDONLY);
    if (bin_fd < 0) {
        perror("open bin file failed");
        return -1;
    }

    // 3. 循环读取并发送
    uint8_t buffer[PACKET_SIZE];
    ssize_t total_sent = 0;
    ssize_t read_len;

    while ((read_len = read(bin_fd, buffer, PACKET_SIZE)) > 0) {
        ssize_t sent = read_len;

        total_sent += sent;
        printf("Sent packet: %zd bytes (total: %zd/%ld)\n", sent, total_sent, (long)file_size);
    }

    if (read_len < 0) {
        perror("read bin file failed");
        close(bin_fd);
        return -1;
    }

    // 关闭文件
    close(bin_fd);
    printf("Send complete. Total bytes: %zd\n", total_sent);
    return total_sent;
}

#define BLOCK_SIZE 1024  // 分块读取大小：1024字节
static uint8_t buffer[BLOCK_SIZE];

/**
 * @brief 读取.bin文件并计算CRC16校验值
 * @param bin_path .bin文件路径
 * @param file_size 输出参数：文件大小（字节）
 * @return 成功返回CRC16校验值；失败返回0xFFFF（需结合file_size判断）
 */
int calculate_bin_crc16(const char *bin_path, off_t *file_size, unsigned short *crc_buf) {
    if (!bin_path || !file_size) {
        fprintf(stderr, "Invalid parameters\n");
        return -1;
    }

    // 1. 获取文件大小
    struct stat file_stat;
    if (stat(bin_path, &file_stat) < 0) {
        perror("stat file failed");
        return -1;
    }
    *file_size = file_stat.st_size;
    printf("File: %s, size: %ld bytes\n", bin_path, (long)*file_size);

    // 2. 打开文件（只读、二进制模式）
    int fd = open(bin_path, O_RDONLY);
    if (fd < 0) {
        perror("open bin file failed");
        return -1;
    }

    // 3. 分块读取并计算CRC16
    uint16_t crc = 0xffff;  // 初始值
    ssize_t read_len;

    while ((read_len = read(fd, buffer, BLOCK_SIZE)) > 0) {
        crc = my_crc16_update(crc, buffer, read_len);  // 累加计算CRC
        //printf("Read %zd bytes (total: %ld/%ld)\n", 
        //       read_len, 
        //       (long)(*file_size - lseek(fd, 0, SEEK_CUR)),  // 已读=总大小-剩余
        //       (long)*file_size);
    }

    // 检查读取错误
    if (read_len < 0) {
        perror("read file failed");
        close(fd);
        return -1;
    }

    close(fd);

	*crc_buf = crc;
	printf("==> crc = 0x%X\r\n", crc);
    return 0;
}


