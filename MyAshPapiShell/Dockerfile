FROM ubuntu:22.04

# Обновляем репозитории и устанавливаем необходимые пакеты
RUN apt-get update && apt-get install -y \
    fuse \
    libfuse-dev \
    gcc \
    && rm -rf /var/lib/apt/lists/*

# Устанавливаем рабочую директорию
WORKDIR /app

# Копируем ваш main.c внутрь контейнера
COPY main.c /app/

# Компилируем ваш myshell
RUN gcc main.c -o myshell -D_FILE_OFFSET_BITS=64 -lfuse

# Определяем точку входа - запуск myshell
ENTRYPOINT ["./myshell"]

