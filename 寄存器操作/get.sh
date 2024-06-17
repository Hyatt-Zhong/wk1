#!/bin/bash

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
while IFS=' ' read -r ADDRESS INVALUE; do
  #echo "读取寄存器地址 0x$ADDRESS"

  
  VALUE=$(i2cget -f -y 0x$BUS_NUMBER $DEVICE_ADDRESS 0x$ADDRESS)
  echo "$ADDRESS  $VALUE"
  
done < "$DATA_FILE"
