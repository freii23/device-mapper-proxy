#!/bin/bash

# Тестовый скрипт для Device Mapper Proxy модуля
# Запускать с правами root

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Функция для вывода статуса
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Проверка прав root
if [[ $EUID -ne 0 ]]; then
   print_error "Этот скрипт должен запускаться с правами root"
   exit 1
fi

# Переменные
MODULE_NAME="dmp"
MODULE_PATH="./dmp.ko"
DEVICE_SIZE="1048576"  # 1MB в секторах (512 байт на сектор)
TEST_DEVICE="zero1"
PROXY_DEVICE="dmp1"

print_status "Начинаем тестирование DMP модуля..."

# 1. Проверка существования модуля
if [[ ! -f "$MODULE_PATH" ]]; then
    print_error "Модуль $MODULE_PATH не найден. Сначала скомпилируйте модуль командой 'make'"
    exit 1
fi

# 2. Загрузка модуля
print_status "Загружаем модуль $MODULE_NAME..."
if lsmod | grep -q "^$MODULE_NAME "; then
    print_warning "Модуль уже загружен, выгружаем..."
    rmmod $MODULE_NAME 2>/dev/null || true
fi

insmod $MODULE_PATH
if lsmod | grep -q "^$MODULE_NAME "; then
    print_status "Модуль успешно загружен"
else
    print_error "Ошибка загрузки модуля"
    exit 1
fi

# 3. Создание тестового блочного устройства
print_status "Создаем тестовое устройство $TEST_DEVICE..."
dmsetup remove $TEST_DEVICE 2>/dev/null || true
dmsetup create $TEST_DEVICE --table "0 $DEVICE_SIZE zero"

if [[ -e "/dev/mapper/$TEST_DEVICE" ]]; then
    print_status "Тестовое устройство создано: $(ls -l /dev/mapper/$TEST_DEVICE)"
else
    print_error "Ошибка создания тестового устройства"
    exit 1
fi

# 4. Создание proxy устройства
print_status "Создаем proxy устройство $PROXY_DEVICE..."
dmsetup remove $PROXY_DEVICE 2>/dev/null || true
dmsetup create $PROXY_DEVICE --table "0 $DEVICE_SIZE $MODULE_NAME /dev/mapper/$TEST_DEVICE"

if [[ -e "/dev/mapper/$PROXY_DEVICE" ]]; then
    print_status "Proxy устройство создано: $(ls -l /dev/mapper/$PROXY_DEVICE)"
else
    print_error "Ошибка создания proxy устройства"
    exit 1
fi

# 5. Проверка начальной статистики
print_status "Проверяем начальную статистику..."
if [[ -f "/sys/module/$MODULE_NAME/$MODULE_NAME/stat/volumes" ]]; Sthen
    print_status "Sysfs интерфейс доступен"
    echo "--- Начальная статистика ---"
    cat /sys/module/$MODULE_NAME/$MODULE_NAME/stat/volumes
    echo "--- Конец статистики ---"
else
    print_error "Sysfs интерфейс недоступен"
    exit 1S
fi

# 6. Тестовые операции записи
print_status "Выполняем тестовые операции записи..."
echo "Запись 4KB блока:"
dd if=/dev/random of=/dev/mapper/$PROXY_DEVICE bs=4096 count=1 2>&1

echo "Запись 8KB блока:"
dd if=/dev/random of=/dev/mapper/$PROXY_DEVICE bs=8192 count=1 2>&1

echo "Запись 2KB блока:"
dd if=/dev/random of=/dev/mapper/$PROXY_DEVICE bs=2048 count=1 2>&1

# 7. Тестовые операции чтения
print_status "Выполняем тестовые операции чтения..."
echo "Чтение 4KB блока:"
dd of=/dev/null if=/dev/mapper/$PROXY_DEVICE bs=4096 count=1 2>&1

echo "Чтение 8KB блока:"
dd of=/dev/null if=/dev/mapper/$PROXY_DEVICE bs=8192 count=1 2>&1

echo "Чтение 1KB блока:"
dd of=/dev/null if=/dev/mapper/$PROXY_DEVICE bs=1024 count=1 2>&1

# 8. Проверка финальной статистики
print_status "Проверяем финальную статистику..."
echo "--- Итоговая статистика ---"
cat /sys/module/$MODULE_NAME/$MODULE_NAME/stat/volumes
echo "--- Конец статистики ---"

# 9. Очистка
print_status "Очищаем тестовое окружение..."
dmsetup remove $PROXY_DEVICE 2>/dev/null || true
dmsetup remove $TEST_DEVICE 2>/dev/null || true

# 10. Выгрузка модуля (опционально)
read -p "Выгрузить модуль? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    print_status "Выгружаем модуль..."
    rmmod $MODULE_NAME
    print_status "Модуль выгружен"
else
    print_status "Модуль оставлен в системе"
fi

print_status "Тестирование завершено успешно!"