#define FUSE_USE_VERSION 30

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <fuse.h>

#define MY_SHELL_MAX_INPUT 1024
#define HISTORY_FILE "history.txt"

// Прототипы функций
void handle_sighup(int sig);
void check_boot_signature(const char *device);
void mount_vfs_cron();
void save_history(const char *command);
void load_history();
void handle_exit();
void execute_command(char *args[]);
void print_env_variable(char *arg);

// --- Реализация функций ---

// Обработчик сигнала SIGHUP
void handle_sighup(int sig) {
    printf("\nConfiguration reloaded\n");
}

// Проверка загрузочного сектора
void check_boot_signature(const char *device) {
    unsigned char buffer[512]; // Буфер для первого сектора (512 байт — размер сектора)
    char device_path[128];
    snprintf(device_path, sizeof(device_path), "/dev/%s", device); // Формируем путь к устройству, например, "/dev/sda"

    // Открываем устройство на чтение
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        perror("Ошибка при открытии устройства");
        return;
    }

    // Считываем первый сектор
    if (read(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
        perror("Ошибка при чтении сектора");
        close(fd);
        return;
    }
    close(fd);

    // Проверяем сигнатуру (последние два байта должны быть 0x55 и 0xAA)
    if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
        printf("Диск %s является загрузочным (сигнатура 0xAA55 найдена).\n", device_path);
    } else {
        printf("Диск %s не является загрузочным (сигнатура 0xAA55 отсутствует).\n", device_path);
    }
}


// --- VFS для задач cron ---

// Атрибуты файловой системы
static int vfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else if (strcmp(path, "/tasks") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 1024; // Примерный размер содержимого
    } else {
        return -ENOENT;
    }
    return 0;
}

// Чтение содержимого директории
static int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, "tasks", NULL, 0);

    return 0;
}

// Открытие файла
static int vfs_open(const char *path, struct fuse_file_info *fi) {
    if (strcmp(path, "/tasks") != 0)
        return -ENOENT;

    return 0;
}

// Чтение файла
static int vfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/tasks") != 0)
        return -ENOENT;

    FILE *cron = popen("crontab -l", "r");
    if (!cron)
        return -EIO;

    char tasks[1024];
    size_t len = fread(tasks, 1, sizeof(tasks), cron);
    pclose(cron);

    if (offset >= len)
        return 0;

    if (offset + size > len)
        size = len - offset;

    memcpy(buf, tasks + offset, size);
    return size;
}

// Операции для FUSE
static struct fuse_operations vfs_ops = {
    .getattr = vfs_getattr,
    .readdir = vfs_readdir,
    .open = vfs_open,
    .read = vfs_read,
};

// Удаление содержимого директории
void clear_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    char filepath[1024];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
        remove(filepath);
    }
    closedir(dir);
}

// Монтирование VFS
void mount_vfs_cron() {
    if (mkdir("/tmp/vfs", 0755) == -1 && errno != EEXIST) {
        perror("Ошибка при создании директории /tmp/vfs");
        return;
    }

    clear_directory("/tmp/vfs");

    pid_t pid = fork();
    if (pid == 0) {
        // Демонизируем процесс, чтобы не блокировать шелл
        if (setsid() < 0) {
            perror("Ошибка при создании новой сессии");
            exit(EXIT_FAILURE);
        }

        // Перенаправляем стандартные файловые дескрипторы в /dev/null, чтобы избежать помех с шеллом
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);

        // Запускаем FUSE
        char *argv[] = {"vfs_cron", "/tmp/vfs", "-f", "-o", "nonempty", NULL};
        if (fuse_main(5, argv, &vfs_ops, NULL) == -1) {
            perror("Ошибка при монтировании FUSE");
            exit(EXIT_FAILURE);
        }
        exit(0);
    } else if (pid < 0) {
        perror("Ошибка при запуске FUSE");
    } else {
        printf("VFS успешно смонтирован в /tmp/vfs. Список задач cron доступен.\n");
    }
}


// --- Функции для работы с историей и командами ---

void save_history(const char *command) {
    int fd = open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd != -1) {
        write(fd, command, strlen(command));
        write(fd, "\n", 1);
        close(fd);
    }
}

void load_history() {
    int fd = open(HISTORY_FILE, O_RDONLY);
    if (fd != -1) {
        char buffer[4096];
        ssize_t bytes;
        while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) {
            write(STDOUT_FILENO, buffer, bytes);
        }
        close(fd);
    } else {
        printf("История отсутствует.\n");
    }
}

void handle_exit() {
    printf("\nВыход из шелла. До свидания!\n");
    exit(0);
}

void execute_command(char *args[]) {
    pid_t pid = fork();
    if (pid == 0) {
        if (execvp(args[0], args) == -1) {
            perror("Ошибка выполнения команды");
        }
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        perror("Ошибка при создании процесса");
    }
}

void print_env_variable(char *arg) {
    if (arg[0] == '$') {
        char *var_name = arg + 1;
        char *value = getenv(var_name);
        if (value) {
            printf("%s\n", value);
        } else {
            printf("Переменная окружения %s не найдена.\n", var_name);
        }
    } else {
        printf("Некорректный формат. Используйте \\e $VAR.\n");
    }
}

void builtin_echo(char *args[]) {
    for (int i = 1; args[i] != NULL; i++) { // Начинаем с args[1], так как args[0] — это "echo"
        printf("%s", args[i]);
        if (args[i + 1] != NULL) {
            printf(" "); // Добавляем пробел между словами
        }
    }
    printf("\n"); // Печатаем перевод строки в конце
}

void dump_memory(pid_t pid) {
    char map_files_path[256], output_file[256], filepath[512];
    snprintf(map_files_path, sizeof(map_files_path), "/proc/%d/map_files", pid);
    snprintf(output_file, sizeof(output_file), "memory_dump_%d.txt", pid);

    DIR *dir = opendir(map_files_path);
    if (!dir) {
        perror("Ошибка при открытии директории map_files");
        return;
    }

    FILE *out_file = fopen(output_file, "w");
    if (!out_file) {
        perror("Ошибка при создании файла дампа");
        closedir(dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", map_files_path, entry->d_name);
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            fprintf(out_file, "Ошибка доступа к %s: %s\n", filepath, strerror(errno));
            continue;
        }

        char buffer[4096];
        ssize_t bytes_read;
        fprintf(out_file, "### Начало дампа: %s ###\n", filepath);
        while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, bytes_read, out_file);
        }
        fprintf(out_file, "\n### Конец дампа: %s ###\n\n", filepath);
        close(fd);
    }

    fclose(out_file);
    closedir(dir);

    printf("Дамп памяти процесса %d сохранён в файл: %s\n", pid, output_file);
}


// --- Основной цикл программы ---

int main() {
    char input[MY_SHELL_MAX_INPUT];
    char *args[128];

    signal(SIGHUP, handle_sighup);
    signal(SIGINT, SIG_IGN);

    printf("Добро пожаловать в пользовательский шелл! Введите 'exit' или '\\q' для выхода.\n");
    printf("Введите 'history' для просмотра командной истории.\n");
    printf("Введите '\\l <device>' для проверки загрузочного сектора (например, \\l sda).\n");
    printf("Введите '\\cron' для монтирования VFS и просмотра задач cron.\n");
    printf("Введите '\\mem <pid>' для получения дампа памяти процесса.\n");

    while (1) {
        printf("> ");
        if (fgets(input, MY_SHELL_MAX_INPUT, stdin) == NULL) {
            handle_exit();
        }

        input[strcspn(input, "\n")] = 0;

        if (strlen(input) > 0) {
            save_history(input);
        }

        char *token = strtok(input, " ");
        int i = 0;
        while (token != NULL) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;

        if (args[0] == NULL) {
            continue;
        } else if (strcmp(args[0], "exit") == 0 || strcmp(args[0], "\\q") == 0) {
            handle_exit();
        } else if (strcmp(args[0], "history") == 0) {
            load_history();
        } else if (strcmp(args[0], "\\e") == 0 && args[1] != NULL) {
            print_env_variable(args[1]);
        } else if (strcmp(args[0], "\\l") == 0 && args[1] != NULL) {
            check_boot_signature(args[1]);
        } else if (strcmp(args[0], "\\cron") == 0) {
            mount_vfs_cron();
        } else if (strcmp(args[0], "\\mem") == 0 && args[1] != NULL) {
            pid_t pid = atoi(args[1]);
            dump_memory(pid);
        } else if (strcmp(args[0], "echo") == 0) {
            builtin_echo(args);
        } else {
            execute_command(args);
        }
    }

    return 0;
}
