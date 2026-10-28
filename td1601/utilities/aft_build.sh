#!/bin/bash

ld_file="${PATH_TD1601_EVB}/gcc_flash.ld"
addr_str="SPIFLASH : ORIGIN = "
version_file="${ProjectPath}/package.yaml"
# 使用sed命令来查找包含宏定义那一行，并提取后面的字符串内容
extracted_value=$(grep "^version: V" "$version_file" | sed -n 's/^version: V//p')
extracted_addr=$(grep "SPIFLASH : ORIGIN = " "$ld_file" | grep -oE '0x[0-9a-fA-F]+' | head -n 1)
#combine_str=V0.1.0
echo ${extracted_value}
#echo $combine_str
echo ${ProjectPath}
if [ -z "$ProjectName" ]; then
	echo "ProjectName is NULL"
	ProjectName=FWDependedLibTest
else
	echo "ProjectName is ${ProjectName}"
fi
riscv64-unknown-elf-objcopy -O binary ./Obj/${ProjectName}.elf ./Obj/${ProjectName}_${extracted_addr}.bin

subtract_num=0x18000000
#subtract_num1=0x1000
subtract_num1=0
decimal_result=$((16#${extracted_addr#0x} - 16#${subtract_num#0x}))
decimal_result1=$((16#${extracted_addr#0x} - 16#${subtract_num1#0x}))
file_addr=$(printf "0x%x" "$decimal_result1")
echo ${decimal_result}
echo ${file_addr}
# 判断是否提取到了内容，如果提取到了才进行下一步处理
if [ -n "$extracted_value" ]; then
    # 使用参数扩展来提取v后面的字符串
#    v_following_value=$(echo $extracted_value | cut -dV -f2)
	v_following_value=${extracted_value##*V}
#	echo $v_following_value
    # 判断提取的v后面字符串中是否包含-rc
    if [[ $v_following_value == *-RC* ]]; then
        # 如果包含，使用sed命令去除-rc部分
#        final_value=$(echo $v_following_value | cut -d-RC f2)
		final_value=${v_following_value%-RC*}
#        echo "$final_value"
    else
#		final_value = ${v_following_value%‘”’*}
		final_value=$(echo $v_following_value | sed 's/.$//')
#        echo "$final_value"
    fi
	final_value=${extracted_value}
	dd if=/dev/zero of=fill.bin bs=3988 count=1
	if [ -e ./Obj/${ProjectName}_${extracted_addr}.bin ]; then
		./${PATH_TD1601_EVB}/bootimgs/product.exe sig ./Obj/${ProjectName}_${extracted_addr}.bin -n $final_value -iver 0x58 -s ./${PATH_TD1601_EVB}/bootimgs/private.ecc.pem -o ./Obj/${ProjectName}_${extracted_addr}-mnt.bin -t SPI_Flash -a ${decimal_result} -tr SPI_Flash -ar ${decimal_result} -ss ECC160r1 -ds SHA1 -d
		./${PATH_TD1601_EVB}/bootimgs/crc32.sh ./Obj/${ProjectName}_${extracted_addr}.bin ./Obj/${ProjectName}_${extracted_addr}-mnt.bin
		cat ./Obj/${ProjectName}_${extracted_addr}-mnt.bin fill.bin ./Obj/${ProjectName}_${extracted_addr}.bin > ./Obj/${ProjectName}_${file_addr}_V${final_value}.bin
		./${PATH_TD1601_EVB}/bootimgs/crc32.sh ./Obj/${ProjectName}_${file_addr}_V${final_value}.bin ./Obj/${ProjectName}_${file_addr}_V${final_value}.bin
		#rm ./Obj/${ProjectName}_${extracted_addr}.bin ./Obj/${ProjectName}_${extracted_addr}-mnt.bin
	fi
else
    echo "没有提取到宏对应的初始字符串，无法进行后续提取v后面字符串的操作"
fi

