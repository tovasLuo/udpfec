include compileconfig.mk
include srcpathconfig.mk

tmpbuildtm := `date |sed 's/ /_/g'`
TMPBUILDTM = $(tmpbuildtm)

#查找代码目录下所有的子目录
SRCDIRS := $(patsubst ./%,%,$(shell find $(SRCCODEDIRS) -type d))

#查找出所有子目录中*.cpp文件
cpplist := $(foreach d,$(SRCDIRS),$(wildcard $(addprefix $(d)/*,.cpp)))

#查找出所有子目录中*.c文件
clist   := $(foreach d,$(SRCDIRS),$(wildcard $(addprefix $(d)/*,.c)))

objlist := $(patsubst %.cpp,%.o,$(cpplist))
depfile := $(patsubst %.cpp,%.d,$(cpplist))
objlist += $(patsubst %.c,%.o,$(clist))
depfile += $(patsubst %.c,%.d,$(clist))

curcmplmacro   := $(addprefix -D ,$(COMPILEMACRO))
cursrcheaddirs := $(addprefix -I ,$(SRCDIRS))
curlibheaddirs := $(addprefix -I ,$(LIBHEADDIRS))

stdid   := --std=c++11
ifeq (gcc, $(compiler))
LIBFILE += -lstdc++
stdid   := --std=c++1y
else ifeq (GCC, $(VERSIONTYPE))
LIBFILE += -lstdc++
stdid   := --std=c++1y
else
endif

ifeq ($(empty), $(LIBFILEDIRS))
LIBFILEDIRS := ./
endif

LIBDIR :=$(foreach v, $(LIBFILEDIRS), -L $(v))

$(TARGETNAME): showgcctype $(objlist)
	@echo ""
	@echo "all files have been compiled , now begin to link every obj for excutable file"
	@echo ""
	@echo "linking............"
	@echo $(objlist) $(LIBFILE) $(LIBFILEDIRS)
	@echo ""
	@$(compiler) -DPBUILTTIME=\"$(TMPBUILDTM)\" -DPBUILDVERSION=\"$(BUILDVERSION)\" $(curcmplmacro) $(COMPILESWITCH) -o $(TARGETNAME) $(objlist) $(LIBDIR) $(LIBFILE) $(stdid)
	@echo ""
	@echo "linked ok," $(TARGETNAME) "has been created"
	@echo ""
	@echo $(TMPBUILDTM) $(BUILDVERSION)

%.o: %.cpp
	@echo ""
	@echo "start " $< "......compiling"
	@$(compiler) -DPBUILTTIME=\"$(TMPBUILDTM)\" -DPBUILDVERSION=\"$(BUILDVERSION)\" $(curcmplmacro) $(COMPILESWITCH) -c $(cursrcheaddirs) $(curlibheaddirs) $< -o $@ $(stdid)
	@echo "created " $@
	@echo "end   " $< "......compiled ok"

%.o: %.c
	@echo ""
	@echo "start " $< "......compiling"
	@$(compiler) -DPBUILTTIME=\"$(TMPBUILDTM)\" -DPBUILDVERSION=\"$(BUILDVERSION)\" $(curcmplmacro) $(COMPILESWITCH) -c $(cursrcheaddirs) $(curlibheaddirs) $< -o $@
	@echo "created " $@
	@echo "end   " $< "......compiled ok"

showgcctype:
	@echo ""
	@echo "now using " $(compiler) "compiler" "and compiling " $(VERSIONTYPE) " version"

clean:
	@-$(RM) $(TARGETNAME) $(objlist) $(depfile)

deltmp:
	@-$(RM) $(objlist) $(depfile)
