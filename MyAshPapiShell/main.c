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
#include <limits.h>

#define MY_SHELL_MAX_INPUT 1024
#define HISTORY_FILE "history.txt"

// Цветовые макросы
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[1;31m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE "\033[1;34m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_CYAN "\033[1;36m"

//Т
// Прототипы функций
void handle_sighup(int sig);
void check_boot_signature(const char *device);
void mount_vfs_cron();
void save_history(const char *command);
void load_history();
void handle_exit();
void execute_command(char *args[]);
void print_env_variable(char *arg);
void builtin_echo(char *args[]);
void setup_signal_handlers();
void execute_command_with_redirection(char *args[]);
void dump_memory(pid_t pid);

// Обработчик сигнала SIGHUP
void handle_sighup(int sig) {
    const char *msg =
        "\n" COLOR_MAGENTA "╔════════════════════════════════════════╗" COLOR_RESET "\n"
        COLOR_MAGENTA "║" COLOR_RESET "    " COLOR_GREEN "Configuration Reloaded" COLOR_RESET "    " COLOR_MAGENTA  "          ║" COLOR_RESET "\n"
        COLOR_MAGENTA "╚════════════════════════════════════════╝" COLOR_RESET "\n";
    write(STDERR_FILENO, msg, strlen(msg));
}

// Установка обработчиков сигналов
void setup_signal_handlers() {
    struct sigaction sa = {0};
    sa.sa_handler = handle_sighup;
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_DFL; // Стандартное поведение для SIGINT
    sigaction(SIGINT, &sa, NULL);
}

// Проверка загрузочного сектора
void check_boot_signature(const char *device) {
    unsigned char buffer[512];
    char device_path[128];
    snprintf(device_path, sizeof(device_path), "/dev/%s", device);
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) { perror("Ошибка открытия устройства"); return; }
    if (read(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
        perror("Ошибка чтения сектора"); close(fd); return;
    }
    close(fd);
    if (buffer[510] == 0x55 && buffer[511] == 0xAA)
        printf("Диск %s является загрузочным (сигнатура 0xAA55 найдена).\n", device_path);
    else
        printf("Диск %s не является загрузочным (сигнатура 0xAA55 отсутствует).\n", device_path);
}

//О
// VFS операции для cron
static int vfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) { stbuf->st_mode = S_IFDIR | 0755; stbuf->st_nlink = 2; }
    else if (strcmp(path, "/tasks") == 0) { stbuf->st_mode = S_IFREG | 0444; stbuf->st_nlink = 1; stbuf->st_size = 1024; }
    else return -ENOENT;
    return 0;
}

static int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".", NULL, 0); filler(buf, "..", NULL, 0); filler(buf, "tasks", NULL, 0);
    return 0;
}

static int vfs_open(const char *path, struct fuse_file_info *fi) {
    if (strcmp(path, "/tasks") != 0) return -ENOENT;
    return 0;
//А
}

static int vfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (strcmp(path, "/tasks") != 0) return -ENOENT;
    FILE *cron = popen("crontab -l", "r");
    if (!cron) return -EIO;
    char tasks[1024];
    size_t len = fread(tasks, 1, sizeof(tasks), cron);
    pclose(cron);
    if (offset >= len) return 0;
    if (offset + size > len) size = len - offset;
    memcpy(buf, tasks + offset, size);
    return size;
}

static struct fuse_operations vfs_ops = {
    .getattr = vfs_getattr,
    .readdir = vfs_readdir,
    .open    = vfs_open,
    .read    = vfs_read,
};

// Монтирование VFS для cron
void mount_vfs_cron() {
    if (mkdir("/tmp/vfs", 0755) == -1 && errno != EEXIST) {
        perror("Ошибка создания /tmp/vfs"); return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) { perror("Ошибка setsid"); exit(EXIT_FAILURE); }
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        open("/dev/null", O_RDONLY); open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY);
        char *argv[] = {"vfs_cron", "/tmp/vfs", "-f", "-o", "nonempty", NULL};
        if (fuse_main(5, argv, &vfs_ops, NULL) == -1) { perror("Ошибка FUSE"); exit(EXIT_FAILURE); }
        exit(0);
    }
    else if (pid < 0) { perror("Ошибка fork"); }
    else { printf("VFS смонтирован в /tmp/vfs. Список задач cron доступен.\n"); }
}

// Сохранение истории команд
void save_history(const char *command) {
    int fd = open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd != -1) { write(fd, command, strlen(command)); write(fd, "\n", 1); close(fd); }
}

// Загрузка истории команд
void load_history() {
    int fd = open(HISTORY_FILE, O_RDONLY);
    if (fd != -1) {
        char buffer[4096]; ssize_t bytes;
        while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) write(STDOUT_FILENO, buffer, bytes);
        close(fd);
    } else printf("История отсутствует.\n");
}

// Выход из шелла
void handle_exit() {
    printf("\nВыход из шелла\n"); exit(0);
}

// Выполнение команды
void execute_command(char *args[]) {
    if (fork() == 0) { execvp(args[0], args); perror("Ошибка выполнения команды"); exit(EXIT_FAILURE); }
    else { int status; wait(&status); }
}

// Печать переменной окружения
void print_env_variable(char *arg) {
    if (arg[0] == '$') {
        char *value = getenv(arg + 1);
        printf("%s\n", value ? value : "Переменная не найдена.");
    }
    else printf("Используйте \\e $VAR.\n");
}

// Команда echo
void builtin_echo(char *args[]) {
    for (int i = 1; args[i]; i++) { printf("%s%s", args[i], args[i + 1] ? " " : "\n"); }
}

// Выполнение команды с перенаправлением
void execute_command_with_redirection(char *args[]) {
    int i = 0, append = 0;
    while (args[i]) {
        if (!strcmp(args[i], ">")) { append = 0; break; }
        if (!strcmp(args[i], ">>")) { append = 1; break; }
        i++;
    }
    if (!args[i]) { execute_command(args); return; }
    if (!args[i + 1]) { fprintf(stderr, COLOR_RED "Ошибка: отсутствует файл.\n" COLOR_RESET); return; }
    args[i] = NULL; char *filename = args[i + 1];
    if (fork() == 0) {
        int fd = open(filename, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { perror(COLOR_RED "Ошибка открытия файла" COLOR_RESET); exit(EXIT_FAILURE); }
        dup2(fd, STDOUT_FILENO); close(fd);
        execvp(args[0], args); perror(COLOR_RED "Ошибка выполнения команды" COLOR_RESET); exit(EXIT_FAILURE);
    }
    else { int status; wait(&status); }
}

//Ш

// Дамп памяти процесса
void dump_memory(pid_t pid) {
    char filepath[256], output_file[256];
    snprintf(filepath, sizeof(filepath), "/proc/%d/maps", pid);
    snprintf(output_file, sizeof(output_file), "memory_dump_%d.txt", pid);
    FILE *in = fopen(filepath, "r"), *out = fopen(output_file, "w");
    if (!in) { perror("Ошибка открытия maps"); return; }
    if (!out) { perror("Ошибка создания дампа"); fclose(in); return; }
    char line[4096];
    while (fgets(line, sizeof(line), in)) fputs(line, out);
    fclose(in); fclose(out);
    printf("Дамп памяти процесса %d сохранён в %s\n", pid, output_file);
}

// Основной цикл шелла
int main() {
    char input[MY_SHELL_MAX_INPUT], *args[128], cwd[PATH_MAX];
    setup_signal_handlers();
    // Меню команд
    printf(" ┌────────────────────────────────────────────────────────────┐\n");
    printf(" │ " COLOR_CYAN "Введите команды для работы:" COLOR_RESET "                                │\n");
    printf(" ├────────────────────────────────────────────────────────────┤\n");
    printf(" │ • " COLOR_BLUE "'exit'" COLOR_RESET ", " COLOR_BLUE "'\\q'" COLOR_RESET " или  " COLOR_BLUE "'Ctrl+D'" COLOR_RESET "  - для выхода из шелла.       │\n");
    printf(" │ • " COLOR_BLUE "'history'" COLOR_RESET "        - для просмотра командной истории.      │\n");
    printf(" │ • " COLOR_BLUE "'\\l <device>'" COLOR_RESET "   - для проверки загрузочного сектора.     │\n");
    printf(" │     Например: " COLOR_YELLOW "\\l sda" COLOR_RESET "                                       │\n");
    printf(" │ • " COLOR_BLUE "'\\cron'" COLOR_RESET "         - для монтирования VFS и просмотра задач.│\n");
    printf(" │ • " COLOR_BLUE "'\\mem <pid>'" COLOR_RESET "    - для получения дампа памяти процесса.   │\n");
    printf(" │ • " COLOR_BLUE "Бинарные команды" COLOR_RESET ": вы можете использовать доступные       │\n");
    printf(" │   системные команды, такие как " COLOR_YELLOW "ls" COLOR_RESET ", " COLOR_YELLOW "pwd" COLOR_RESET ", " COLOR_YELLOW "cat" COLOR_RESET " и другие.      │\n");
    printf(" │ • " COLOR_BLUE "SIGHUP" COLOR_RESET ": отправьте сигнал для перезагрузки конфигурации:  │\n");
    printf(" │   " COLOR_YELLOW "kill -SIGHUP <pid>" COLOR_RESET ", из терминала Linux, где " COLOR_YELLOW "<pid>" COLOR_RESET "  -     │\n");
    printf(" │   ID процесса шелла.                                       │\n");
    printf(" └────────────────────────────────────────────────────────────┘\n");

    while (1) {
        // Приглашение
        if (getcwd(cwd, sizeof(cwd))) printf(COLOR_MAGENTA "%s >> " COLOR_RESET, cwd);
        else { perror("Ошибка директории"); printf(COLOR_MAGENTA "> " COLOR_RESET); }
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) { if (feof(stdin)) handle_exit(); continue; }
        input[strcspn(input, "\n")] = 0;
        if (strlen(input)) save_history(input);

        // Разбор аргументов
        int i = 0; char *token = strtok(input, " ");
        while (token && i < 127) args[i++] = token, token = strtok(NULL, " ");
        args[i] = NULL;

        if (!args[0]) continue;
        if (!strcmp(args[0], "exit") || !strcmp(args[0], "\\q")) handle_exit();
        else if (!strcmp(args[0], "history")) load_history();
        else if (!strcmp(args[0], "\\e") && args[1]) print_env_variable(args[1]);
        else if (!strcmp(args[0], "\\l") && args[1]) check_boot_signature(args[1]);
        else if (!strcmp(args[0], "\\cron")) mount_vfs_cron();
        else if (!strcmp(args[0], "\\mem") && args[1]) dump_memory(atoi(args[1]));
        else if (!strcmp(args[0], "echo")) builtin_echo(args);
        else execute_command_with_redirection(args);
    }
    return 0;
}

