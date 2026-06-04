#!/bin/bash
#./buildapp.sh debug V001R00C000B001 or ./buildapp.sh release V001R00C000B001
LANG=en

if [[ $0 =~ ^\/.* ]]    # 判断当前脚本是否为绝对路径，匹配以/开头下的所有
then
  script=$0
else
  script=$(pwd)/$0
fi

if [ $# -ne 2 ]; then
    echo "pls input compile type(debug/release version(V001R000C000B000))"
    exit 0
fi

if [ $1 != "debug" ] && [ $1 != "release" ]; then
    echo "invalid compile type(debug or release is ok)"
	exit 0
fi

script=`readlink -f $script`   # 获取文件的真实路径
script_path=${script%/*}       # 获取文件所在的目录
realpath=$(readlink -f $script_path)   # 获取文件所在目录的真实路径

tmppath=../../tmp
cd $realpath

make clean
make VERSIONTYPE=$1 BUILDVERSION=$2 2>$tmppath/release_tmp.txt
make deltmp

let pos=`echo "$2" | awk -F "." '{printf "%d", length($0)-length($NF)}'`
newVersion=${2:pos:${#2}-pos}
echo ${2:0:$pos}$((newVersion+1)) > conf.txt

running_result=$(cat $tmppath/release_tmp.txt)

mkdir -p  ../../bin/
if [ $1 == "debug" ]; then   
    mv goodtptester  ../../bin/goodtptester
else    
    mv goodtptester  ../../bin/goodtptester
fi

# TODO::编译成功时返回0,编译失败时返回-1
if [[ $running_result =~ "warning" ]] || [[ $running_result =~ "error" ]];then

	cat $tmppath/release_tmp.txt
	echo ""
	echo "End compiling for src, no passing o(╯□╰)o"
	echo ""

	rm $tmppath/release_tmp.txt

	exit -1 
fi

echo ""
echo "End compiling for src, passing ~@^_^@~"
echo ""

rm $tmppath/release_tmp.txt
exit 0 
