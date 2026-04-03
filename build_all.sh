#!/bin/bash
set -e

# Сорсим переменные среды ESP-IDF
echo "Sourcing ESP-IDF environment..."
. /home/alexander/ai_voice_agent/esp-idf-5.5.2/export.sh

cd /home/alexander/ai_voice_agent/xiaozhi-esp32

# 1. Сборка для Freenove (дефолтные настройки из sdkconfig)
echo ""
echo "====================================="
echo "=== 1. Сборка Freenove 2.8        ==="
echo "====================================="
echo ""

if [ ! -f sdkconfig.freenove ]; then
    cp sdkconfig sdkconfig.freenove
fi

# Собираем дефолтную (текущую) конфигурацию
idf.py fullclean
idf.py build
mkdir -p dist
cp build/xiaozhi.bin dist/xiaozhi_freenove.bin

# 2. Сборка для Breadboard Mini
echo ""
echo "====================================="
echo "=== 2. Сборка Breadboard Mini    ==="
echo "====================================="
echo ""

cp sdkconfig sdkconfig.breadboard

# Выключаем Freenove, включаем Breadboard (через xingzhi-cube)
# Редактируем sdkconfig.breadboard
sed -i 's/^CONFIG_BOARD_TYPE_FREENOVE_2_8=y/# CONFIG_BOARD_TYPE_FREENOVE_2_8 is not set/' sdkconfig.breadboard
sed -i 's/^# CONFIG_BOARD_TYPE_XINGZHI_CUBE_0_96OLED_WIFI is not set/CONFIG_BOARD_TYPE_XINGZHI_CUBE_0_96OLED_WIFI=y/' sdkconfig.breadboard || echo "CONFIG_BOARD_TYPE_XINGZHI_CUBE_0_96OLED_WIFI=y" >> sdkconfig.breadboard

# Копируем созданный конфиг на место рабочего, чтобы idf.py подхватила его
cp sdkconfig.breadboard sdkconfig

# Сборка
idf.py fullclean
idf.py build
cp build/xiaozhi.bin dist/xiaozhi_breadboard_mini.bin

# Восстанавливаем конфиг Freenove обратно для удобства дальнейшей разработки
cp sdkconfig.freenove sdkconfig

echo ""
echo "=== 3. Загрузка на OTA-сервер ==="
echo "====================================="
echo ""

scp dist/xiaozhi_freenove.bin alexander@192.168.22.102:/mnt/media/docker-compose/ota-remedy/firmwares/xiaozhi_freenove.bin
scp dist/xiaozhi_breadboard_mini.bin alexander@192.168.22.102:/mnt/media/docker-compose/ota-remedy/firmwares/xiaozhi_breadboard_mini.bin

echo ""
echo "=== Сборка и деплой завершены успешно ==="
ls -l dist/xiaozhi_freenove.bin dist/xiaozhi_breadboard_mini.bin
