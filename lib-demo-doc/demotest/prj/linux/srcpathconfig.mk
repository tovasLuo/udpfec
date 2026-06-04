#####################################################################################################
#配置产品代码所在根目录
#for example:
#SRCCODEDIRS := ../code
#####################################################################################################
SRCCODEDIRS   := ../../src

#####################################################################################################
#配置库二进制文件路径
#for example:
#LIBFILEDIRS := ../../lib/gmock/lib ../../lib/gtest/lib
#####################################################################################################
system_version := $(shell lsb_release -r);
var := $(findstring 20,$(system_version))
ifeq (20, $(var))
LIBFILEDIRS = ../../lib/bin/cos \
              ../../lib/bin/json \
              ../../lib/bin/goodtp/linux 
else
LIBFILEDIRS = ../../lib/bin/cos \
              ../../lib/bin/json \
              ../../lib/bin/goodtp/linux 
endif

#####################################################################################################
#配置库头文件路径
#for example:
#LIBHEADDIRS := ../../lib/gmock/include ../../lib/gtest/include
#####################################################################################################
LIBHEADDIRS := ../../lib/inc/cos \
               ../../lib/inc/json \
               ../../lib/inc/goodtp \
               ../../lib/inc/slidwin 

#####################################################################################################
#配置库文件
#for example:
#LIBFILE := -lcos -lrt -pthread -lgtest -lgmock
#####################################################################################################
ifeq (debug, $(VERSIONTYPE))
LIBFILE := -pthread -lcosd -ljsond -lgoodtpd
else ifeq (DEBUG, $(VERSIONTYPE))
LIBFILE := -pthread -lcosd -ljsond -lgoodtpd
else
LIBFILE := -pthread -lcos -ljson -lgoodtpr
endif
