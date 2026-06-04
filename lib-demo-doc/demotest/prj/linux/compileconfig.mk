#####################################################################################################
#定义产品目标可执行文件名称
#for example:
#TARGETNAME = demo_app
#####################################################################################################
TARGETNAME = goodtptester

#####################################################################################################
#定义软件版本号
#for example:
#BUILDVERSION = demo_app
#####################################################################################################
BUILDVERSION = V001R000C000B000

#####################################################################################################
#定义默认编译版本
#for example:
#VERSIONTYPE = debug or release
#####################################################################################################
VERSIONTYPE = debug

#####################################################################################################
#定义软件中使用预编译宏
#for example:
#COMPILEMACRO := _DEBUG
#####################################################################################################
COMPILEMACRO := _DEBUG

#####################################################################################################
#定义编译用到的编译选项开关,进程要支持抓取崩溃时的堆栈信息不能进行任何On优化,且要加-rdynamic开关
#for example:
#COMPILESWITCH := -g -O2
#####################################################################################################
COMPILESWITCH := -g -Wunused -ffloat-store -fkeep-inline-functions -fforce-addr -Wcomment -Wreturn-type -Wuninitialized -Wmaybe-uninitialized -Wsign-compare -Wshadow -Wchar-subscripts -Wparentheses -Wredundant-decls -rdynamic -Wno-int-to-pointer-cast

ifeq (debug, $(VERSIONTYPE))
#COMPILESWITCH += -Og
COMPILEMACRO  += _SELFDEBUG
TARGETNAME     = goodtptester
else ifeq (DEBUG, $(VERSIONTYPE))
#COMPILESWITCH += -Og
COMPILEMACRO  += _SELFDEBUG
TARGETNAME     = goodtptester
else
COMPILESWITCH += -Ofast
TARGETNAME     = goodtptester
endif

compiler := g++
