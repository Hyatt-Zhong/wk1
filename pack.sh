#!/bin/sh
# 可执行程序名
appname=$1
# 目标文件夹
dst=$2

if [ ! $appname ] || [ ! $dst ]; then  
  echo "appname or dst is null"  
  exit
else  
  echo "pack $appname depend to $dst"  
fi 

# 利用 ldd 提取依赖库的具体路径
liblist=$(ldd $appname | awk '{ if (match($3,"/")){ printf("%s "), $3 } }')
# 目标文件夹的检测
if [ ! -d $dst ];then
		mkdir $dst
fi
# 拷贝库文件和可执行程序到目标文件夹
cp $liblist $dst
cp $appname $dst
