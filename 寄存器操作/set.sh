#!/bin/bash

#用法 ./set.sh 4 0x44 init_data.txt 将 init_data.txt里的 地址-值 对写到4号总线地址为0x44的器件上
# 定义变量
BUS_NUMBER=$1        # I2C总线号
DEVICE_ADDRESS=$2    # I2C设备地址
DATA_FILE=$3  # 数据文件名

# 检查数据文件是否存在
if [ ! -f "$DATA_FILE" ]; then
  echo "数据文件 $DATA_FILE 不存在。"
  exit 1
fi

# 读取数据文件并循环写入每对地址和值
while IFS=' ' read -r ADDRESS VALUE; do
  #echo "写入寄存器地址 0x$ADDRESS 值 0x$VALUE"

  # 使用i2cset写入值
  i2cset -f -y $BUS_NUMBER $DEVICE_ADDRESS 0x$ADDRESS $VALUE

  
done < "$DATA_FILE"

echo "所有寄存器写入完成。"