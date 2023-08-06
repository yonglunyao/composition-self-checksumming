
echo 'program.c input sc-include-func assert-skip-func-file'

UTILS_LIB=/home/sip/self-checksumming/build/lib/libUtils.so
INPUT_DEP_PATH=/usr/local/lib/
SC_PATH=/home/sip/self-checksumming/build/lib


#------------------ARGS for the script-------------
Source=$1		#Prgram to protect
FilterFile=$2		#List of sensitive functions, one name per line
Con=$3			#Connectivity level

if [ $Con = ""]; then
	Con=2
fi


#-------------------IMPORTANT Input Dependency Transformations --------
#-extract-functions
#-extraction-stats
#-extraction-stats-file="extract.stats"

#-------------------IMPORTANT CMD ARGS for SC------------------
#-connectivity= N		set the desired connectivity of the checkers network

#-extracted-only  		protect extracted only, extracted are never checkers, always checkees

#-use-other-functions		to meet the desired connectivity use other functions 
#				(not in the filter list) as checkers

#-dump-checkers-network		dump the network in the specified path

#-sensitive-only-checked	sensitive functions are never checkers but checkees, 
#				extracted only assumes this regardless of the flag  

#$1 is the .c file for transformation
echo 'build changes'
make -C build/

if [ $? -eq 0 ]; then
	    echo 'OK Compile'
    else
	        echo 'FAIL Compile'
	       exit	
	fi



#这是一个使用`clang-3.9`编译器的命令，用于将C源代码转换为LLVM位码（Bitcode）格式的命令。
#
#具体解释每个选项：
#- `clang-3.9`：这是`clang`编译器的可执行文件，版本号为3.9。`clang`是一个C、C++和Objective-C编译器，它支持LLVM编译器基础设施。
#- `$Source`：这是脚本中的第一个命令行参数，即要编译的C源代码文件的名称。`$Source`是在脚本中传递给该命令的变量。
#- `-c`：这是`clang`的一个选项，表示将输入文件编译成目标文件，而不进行链接。这个选项告诉`clang`只执行编译阶段，不生成最终的可执行文件。
#- `-emit-llvm`：这也是`clang`的一个选项，表示将输出生成为LLVM位码（Bitcode）。LLVM位码是一种中间表示形式，它是一种与具体硬件和操作系统无关的表示形式。
#- `-o guarded.bc`：这是指定输出文件的选项，表示将编译后的LLVM位码输出到名为`guarded.bc`的文件中。
#
#综上所述，该命令将C源代码文件（在`$Source`中指定）编译为LLVM位码文件，该文件将保存在名为`guarded.bc`的文件中。此步骤是将C代码转换为LLVM中间表示，为后续的优化和转换步骤做准备。
clang-3.9 $Source -c -emit-llvm -o guarded.bc

echo 'Remove old files'
rm guide.txt
rm protected
rm out.bc
rm out

bitcode=guarded.bc

echo 'Transform SC'
opt-3.9 -load $INPUT_DEP_PATH/libInputDependency.so -load $UTILS_LIB -load $INPUT_DEP_PATH/libTransforms.so -load $SC_PATH/libSCPass.so $bitcode -strip-debug -unreachableblockelim -globaldce -extract-functions -sc -connectivity=$Con -dump-checkers-network="network_file" -dump-sc-stat="sc.stats" -filter-file=$FilterFile -o out.bc

if [ $? -eq 0 ]; then
	    echo 'OK Transform'
    else
	        echo 'FAIL Transform'
	       exit	
	fi



#link guardMe function
clang-3.9 rtlib.c -c -emit-llvm -o rtlib.bc
llvm-link-3.9 out.bc rtlib.bc -o out.bc


echo 'Post patching binary after hash calls'
llc-3.9 out.bc
gcc -c -rdynamic out.s -o out.o -lncurses
#gcc -g -rdynamic -c rtlib.c -o rtlib.o
gcc -g -rdynamic out.o response.o -o out -lncurses 

#clang++-3.9 -lncurses -rdynamic -std=c++0x out.bc -o out
python patcher/dump_pipe.py out guide.txt patch_guide
echo 'Done patching'

chmod +x out
echo 'Protected file: out'
