##
## Auto Generated makefile by CDK
## Do not modify this file, and any manual changes will be erased!!!   
##
## BuildSet
ProjectName            :=hello_world
ConfigurationName      :=BuildSet
WorkspacePath          :=./
ProjectPath            :=./
IntermediateDirectory  :=Obj
OutDir                 :=$(IntermediateDirectory)
User                   :=GGG
Date                   :=09/09/2026
CDKPath                :=../../../../../../environment
ToolchainPath          :=D:/Baozi/CDK/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/
LinkerName             :=riscv64-unknown-elf-gcc
LinkerNameoption       :=
SIZE                   :=riscv64-unknown-elf-size
READELF                :=riscv64-unknown-elf-readelf
CHECKSUM               :=crc32
SharedObjectLinkerName :=
ObjectSuffix           :=.o
DependSuffix           :=.d
PreprocessSuffix       :=.i
DisassemSuffix         :=.asm
IHexSuffix             :=.ihex
BinSuffix              :=.bin
ExeSuffix              :=.elf
LibSuffix              :=.a
DebugSwitch            :=-g 
IncludeSwitch          :=-I
LibrarySwitch          :=-l
OutputSwitch           :=-o 
ElfInfoSwitch          :=-hlS
LibraryPathSwitch      :=-L
PreprocessorSwitch     :=-D
UnPreprocessorSwitch   :=-U
SourceSwitch           :=-c 
ObjdumpSwitch          :=-S
ObjcopySwitch          :=-O ihex
ObjcopyBinSwitch       :=-O binary
OutputFile             :=$(ProjectName)
ObjectSwitch           :=-o 
ArchiveOutputSwitch    := 
PreprocessOnlySwitch   :=-E
PreprocessOnlyDisableLineSwitch   :=-P
ObjectsFileList        :=$(IntermediateDirectory)/hello_world.txt
MakeDirCommand         :=mkdir
LinkOptions            :=   -nostartfiles  -Wl,--gc-sections  -T../../boards/td1601_evb/v1.0/gcc_flash.ld  -mcpu=e906fd  -pipe 
LinkOtherFlagsOption   :=  -nostartfiles -Wl,--gc-sections -lm  -Wl,-zmax-page-size=1024
 -Wl,-Map=$(ProjectPath)/Lst/$(OutputFile).map 
IncludePackagePath     :=
IncludeCPath           :=$(IncludeSwitch)../../components/chips/chip_td1601/v1.0/include $(IncludeSwitch)../../components/chips/chip_td1601/v1.0/sys $(IncludeSwitch)../../components/console/v1.0/ $(IncludeSwitch)../../components/csi/v1.0/include $(IncludeSwitch)../../components/csi/v1.0/include/core $(IncludeSwitch)../../components/minilibc/v1.0/include $(IncludeSwitch)../../components/mm/v1.0/include $(IncludeSwitch)../../boards/td1601_evb/v1.0/include  $(IncludeSwitch)include $(IncludeSwitch)include/sys $(IncludeSwitch)UserFolder/  
IncludeAPath           :=$(IncludeSwitch)../../components/chips/chip_td1601/v1.0/include $(IncludeSwitch)../../components/chips/chip_td1601/v1.0/sys $(IncludeSwitch)../../components/console/v1.0/ $(IncludeSwitch)../../components/csi/v1.0/include $(IncludeSwitch)../../components/csi/v1.0/include/core $(IncludeSwitch)../../components/minilibc/v1.0/include $(IncludeSwitch)../../components/mm/v1.0/include $(IncludeSwitch)../../boards/td1601_evb/v1.0/include  $(IncludeSwitch)include $(IncludeSwitch)include/sys $(IncludeSwitch)UserFolder/  
Libs                   := -Wl,--whole-archive $(LibrarySwitch)chip_td1601 $(LibrarySwitch)console $(LibrarySwitch)csi $(LibrarySwitch)minilibc $(LibrarySwitch)mm $(LibrarySwitch)td1601_evb  -Wl,--no-whole-archive $(LibrarySwitch)m  
ArLibs                 := 
PackagesLibPath        :=
LibPath                :=$(LibraryPathSwitch)Obj  $(PackagesLibPath) 

##
## Common variables
## AR, CXX, CC, AS, CXXFLAGS and CFLAGS can be overriden using an environment variables
##
AR       :=riscv64-unknown-elf-ar rcu
CXX      :=riscv64-unknown-elf-g++
CC       :=riscv64-unknown-elf-gcc
AS       :=riscv64-unknown-elf-gcc
OBJDUMP  :=riscv64-unknown-elf-objdump
OBJCOPY  :=riscv64-unknown-elf-objcopy
CXXFLAGS :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB   -Os -g  -Os -g $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd   -Os  -g -pipe 
CFLAGS   :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB   -Wno-unused-function -O2 -g -nostdlib -Wpointer-arith -Wall -Wl,-EL -ffunction-sections -fdata-sections -fdata-sections -Os -g  $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd   -Os  -g -pipe 
ASFLAGS  :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB    $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd -pipe 
PreprocessFlags  :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB   -Wno-unused-function -O2 -g -nostdlib -Wpointer-arith -Wall -Wl,-EL -ffunction-sections -fdata-sections -fdata-sections -Os -g  $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd   -Os  -g -pipe 


Objects0=$(IntermediateDirectory)/QSPI_test_qspi_lcd$(ObjectSuffix) $(IntermediateDirectory)/QSPI_test_qspi_shapes_test$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) $(IntermediateDirectory)/test_circle_test$(ObjectSuffix) $(IntermediateDirectory)/test_emotion$(ObjectSuffix) $(IntermediateDirectory)/test_independent_test$(ObjectSuffix) $(IntermediateDirectory)/test_ir_test$(ObjectSuffix) $(IntermediateDirectory)/test_limit_test$(ObjectSuffix) $(IntermediateDirectory)/test_pwm_independence_test$(ObjectSuffix) $(IntermediateDirectory)/test_servo_ctl$(ObjectSuffix) \
	$(IntermediateDirectory)/test_servo_test$(ObjectSuffix) $(IntermediateDirectory)/chip_config$(ObjectSuffix) $(IntermediateDirectory)/src_board_init$(ObjectSuffix) $(IntermediateDirectory)/src_main$(ObjectSuffix) 



Objects=$(Objects0) 

##
## Main Build Targets 
##
.PHONY: all
all: $(IntermediateDirectory)/$(OutputFile)

$(IntermediateDirectory)/$(OutputFile):  $(Objects) Always_Link 
	$(LinkerName) $(OutputSwitch) $(IntermediateDirectory)/$(OutputFile)$(ExeSuffix) $(LinkerNameoption) -Wl,-Map=$(ProjectPath)/Lst/$(OutputFile).map  @$(ObjectsFileList)  $(LinkOptions) $(LibPath) $(Libs) $(LinkOtherFlagsOption)
	-@mv $(ProjectPath)/Lst/$(OutputFile).map $(ProjectPath)/Lst/$(OutputFile).temp && $(READELF) $(ElfInfoSwitch) $(ProjectPath)/Obj/$(OutputFile)$(ExeSuffix) > $(ProjectPath)/Lst/$(OutputFile).map && echo ====================================================================== >> $(ProjectPath)/Lst/$(OutputFile).map && cat $(ProjectPath)/Lst/$(OutputFile).temp >> $(ProjectPath)/Lst/$(OutputFile).map && rm -rf $(ProjectPath)/Lst/$(OutputFile).temp
	$(OBJCOPY) $(ObjcopySwitch) $(ProjectPath)/$(IntermediateDirectory)/$(OutputFile)$(ExeSuffix)  $(ProjectPath)/Obj/$(OutputFile)$(IHexSuffix) 
	$(OBJDUMP) $(ObjdumpSwitch) $(ProjectPath)/$(IntermediateDirectory)/$(OutputFile)$(ExeSuffix)  > $(ProjectPath)/Lst/$(OutputFile)$(DisassemSuffix) 
	@echo size of target:
	@$(SIZE) $(ProjectPath)$(IntermediateDirectory)/$(OutputFile)$(ExeSuffix) 
	@echo -n checksum value of target:  
	@$(CHECKSUM) $(ProjectPath)/$(IntermediateDirectory)/$(OutputFile)$(ExeSuffix) 
	@hello_world.modify.bat $(IntermediateDirectory) $(OutputFile)$(ExeSuffix) 

Always_Link:


##
## Objects
##
$(IntermediateDirectory)/QSPI_test_qspi_lcd$(ObjectSuffix): UserFolder/QSPI_test/qspi_lcd.c  
	$(CC) $(SourceSwitch) UserFolder/QSPI_test/qspi_lcd.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/QSPI_test_qspi_lcd$(ObjectSuffix) -MF$(IntermediateDirectory)/QSPI_test_qspi_lcd$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/QSPI_test_qspi_lcd$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/QSPI_test_qspi_lcd$(PreprocessSuffix): UserFolder/QSPI_test/qspi_lcd.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/QSPI_test_qspi_lcd$(PreprocessSuffix) UserFolder/QSPI_test/qspi_lcd.c

$(IntermediateDirectory)/QSPI_test_qspi_shapes_test$(ObjectSuffix): UserFolder/QSPI_test/qspi_shapes_test.c  
	$(CC) $(SourceSwitch) UserFolder/QSPI_test/qspi_shapes_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/QSPI_test_qspi_shapes_test$(ObjectSuffix) -MF$(IntermediateDirectory)/QSPI_test_qspi_shapes_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/QSPI_test_qspi_shapes_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/QSPI_test_qspi_shapes_test$(PreprocessSuffix): UserFolder/QSPI_test/qspi_shapes_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/QSPI_test_qspi_shapes_test$(PreprocessSuffix) UserFolder/QSPI_test/qspi_shapes_test.c

$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix): UserFolder/test.c  
	$(CC) $(SourceSwitch) UserFolder/test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_test$(PreprocessSuffix): UserFolder/test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_test$(PreprocessSuffix) UserFolder/test.c

$(IntermediateDirectory)/test_circle_test$(ObjectSuffix): UserFolder/test/circle_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/circle_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_circle_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_circle_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_circle_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_circle_test$(PreprocessSuffix): UserFolder/test/circle_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_circle_test$(PreprocessSuffix) UserFolder/test/circle_test.c

$(IntermediateDirectory)/test_emotion$(ObjectSuffix): UserFolder/test/emotion.c  
	$(CC) $(SourceSwitch) UserFolder/test/emotion.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_emotion$(ObjectSuffix) -MF$(IntermediateDirectory)/test_emotion$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_emotion$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_emotion$(PreprocessSuffix): UserFolder/test/emotion.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_emotion$(PreprocessSuffix) UserFolder/test/emotion.c

$(IntermediateDirectory)/test_independent_test$(ObjectSuffix): UserFolder/test/independent_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/independent_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_independent_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_independent_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_independent_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_independent_test$(PreprocessSuffix): UserFolder/test/independent_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_independent_test$(PreprocessSuffix) UserFolder/test/independent_test.c

$(IntermediateDirectory)/test_ir_test$(ObjectSuffix): UserFolder/test/ir_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/ir_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_ir_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_ir_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_ir_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_ir_test$(PreprocessSuffix): UserFolder/test/ir_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_ir_test$(PreprocessSuffix) UserFolder/test/ir_test.c

$(IntermediateDirectory)/test_limit_test$(ObjectSuffix): UserFolder/test/limit_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/limit_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_limit_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_limit_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_limit_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_limit_test$(PreprocessSuffix): UserFolder/test/limit_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_limit_test$(PreprocessSuffix) UserFolder/test/limit_test.c

$(IntermediateDirectory)/test_pwm_independence_test$(ObjectSuffix): UserFolder/test/pwm_independence_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/pwm_independence_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_pwm_independence_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_pwm_independence_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_pwm_independence_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_pwm_independence_test$(PreprocessSuffix): UserFolder/test/pwm_independence_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_pwm_independence_test$(PreprocessSuffix) UserFolder/test/pwm_independence_test.c

$(IntermediateDirectory)/test_servo_ctl$(ObjectSuffix): UserFolder/test/servo_ctl.c  
	$(CC) $(SourceSwitch) UserFolder/test/servo_ctl.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_servo_ctl$(ObjectSuffix) -MF$(IntermediateDirectory)/test_servo_ctl$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_servo_ctl$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_servo_ctl$(PreprocessSuffix): UserFolder/test/servo_ctl.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_servo_ctl$(PreprocessSuffix) UserFolder/test/servo_ctl.c

$(IntermediateDirectory)/test_servo_test$(ObjectSuffix): UserFolder/test/servo_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/servo_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_servo_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_servo_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_servo_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_servo_test$(PreprocessSuffix): UserFolder/test/servo_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_servo_test$(PreprocessSuffix) UserFolder/test/servo_test.c

$(IntermediateDirectory)/chip_config$(ObjectSuffix): chip_config.c  
	$(CC) $(SourceSwitch) chip_config.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/chip_config$(ObjectSuffix) -MF$(IntermediateDirectory)/chip_config$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/chip_config$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/chip_config$(PreprocessSuffix): chip_config.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/chip_config$(PreprocessSuffix) chip_config.c

$(IntermediateDirectory)/src_board_init$(ObjectSuffix): src/board_init.c  
	$(CC) $(SourceSwitch) src/board_init.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_board_init$(ObjectSuffix) -MF$(IntermediateDirectory)/src_board_init$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_board_init$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_board_init$(PreprocessSuffix): src/board_init.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_board_init$(PreprocessSuffix) src/board_init.c

$(IntermediateDirectory)/src_main$(ObjectSuffix): src/main.c  
	$(CC) $(SourceSwitch) src/main.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_main$(ObjectSuffix) -MF$(IntermediateDirectory)/src_main$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_main$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_main$(PreprocessSuffix): src/main.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_main$(PreprocessSuffix) src/main.c


-include $(IntermediateDirectory)/*$(DependSuffix)
