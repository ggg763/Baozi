##
## Auto Generated makefile by CDK
## Do not modify this file, and any manual changes will be erased!!!   
##
## BuildSet
ProjectName            :=Baozi
ConfigurationName      :=BuildSet
WorkspacePath          :=./
ProjectPath            :=./
IntermediateDirectory  :=Obj
OutDir                 :=$(IntermediateDirectory)
User                   :=GGG
Date                   :=28/10/2026
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
ObjectsFileList        :=$(IntermediateDirectory)/Baozi.txt
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
LibPath                :=$(LibraryPathSwitch)Obj $(LibraryPathSwitch)UserFolder/anim  $(PackagesLibPath) 

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
CFLAGS   :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB   -Wno-unused-function -O2 -g -nostdlib -Wpointer-arith -Wall -Wl,-EL -ffunction-sections -fdata-sections -fdata-sections -Os -g  $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd   -Os  -g -DANIM_USE_BIN=1 -pipe 
ASFLAGS  :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB    $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd -pipe 
PreprocessFlags  :=$(PreprocessorSwitch)USE_CHIP_TD1601 $(PreprocessorSwitch)USE_CONSOLE $(PreprocessorSwitch)USE_CSI $(PreprocessorSwitch)USE_MINILIBC $(PreprocessorSwitch)USE_MM $(PreprocessorSwitch)USE_TD1601_EVB   -Wno-unused-function -O2 -g -nostdlib -Wpointer-arith -Wall -Wl,-EL -ffunction-sections -fdata-sections -fdata-sections -Os -g  $(PreprocessorSwitch)CONFIG_ARCH_INTERRUPTSTACK=4096 $(PreprocessorSwitch)CONFIG_BENCHMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_COREMARK=1 $(PreprocessorSwitch)CONFIG_BENCHMARK_DHRY=1 $(PreprocessorSwitch)CONFIG_CHIP_TD1601=1 $(PreprocessorSwitch)CONFIG_CPU_E906FD=1 $(PreprocessorSwitch)CONFIG_DEBUG_MODE=1 $(PreprocessorSwitch)CONFIG_KERNEL_NONE=1 $(PreprocessorSwitch)CONFIG_SUPPORT_TSPEND=1 $(PreprocessorSwitch)CONFIG_XIP=1   -mcpu=e906fd   -Os  -g -DANIM_USE_BIN=1 -pipe 


Objects0=$(IntermediateDirectory)/QSPI_test_video_tetra_test$(ObjectSuffix) $(IntermediateDirectory)/anim_face_action$(ObjectSuffix) $(IntermediateDirectory)/anim_face_app$(ObjectSuffix) $(IntermediateDirectory)/anim_face_draw$(ObjectSuffix) $(IntermediateDirectory)/anim_face_model$(ObjectSuffix) $(IntermediateDirectory)/anim_face_render$(ObjectSuffix) $(IntermediateDirectory)/anim_face_ui_card$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_app_core$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_behavior$(ObjectSuffix) $(IntermediateDirectory)/emotion_emotion_fsm$(ObjectSuffix) \
	$(IntermediateDirectory)/emotion_emotion_manager$(ObjectSuffix) $(IntermediateDirectory)/emotion_mood_fsm$(ObjectSuffix) $(IntermediateDirectory)/gfx_gfx$(ObjectSuffix) $(IntermediateDirectory)/gfx_gfx_demo$(ObjectSuffix) $(IntermediateDirectory)/gfx_gfx_sprite_demo$(ObjectSuffix) $(IntermediateDirectory)/movements_moves$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_angry$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_bored$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_demo$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_eight$(ObjectSuffix) \
	$(IntermediateDirectory)/movements_moves_happy$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_idle$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_loving$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_sad$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_sleep$(ObjectSuffix) $(IntermediateDirectory)/movements_moves_surprised$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_play_card$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_pomo$(ObjectSuffix) $(IntermediateDirectory)/servo_test_servo_core$(ObjectSuffix) $(IntermediateDirectory)/servo_test_servo_dual_test$(ObjectSuffix) \
	$(IntermediateDirectory)/servo_test_servo_mvp_test$(ObjectSuffix) $(IntermediateDirectory)/servo_test_servo_range_test$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) $(IntermediateDirectory)/test_emotion_test$(ObjectSuffix) $(IntermediateDirectory)/test_ir_state_test$(ObjectSuffix) $(IntermediateDirectory)/test_ir_test$(ObjectSuffix) $(IntermediateDirectory)/test_serial_min_test$(ObjectSuffix) $(IntermediateDirectory)/tracking_track_dual$(ObjectSuffix) $(IntermediateDirectory)/tracking_track_yaw$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_ui$(ObjectSuffix) \
	$(IntermediateDirectory)/UserFolder_ui_app$(ObjectSuffix) $(IntermediateDirectory)/chip_config$(ObjectSuffix) $(IntermediateDirectory)/src_ICNT8952BN$(ObjectSuffix) $(IntermediateDirectory)/src_baozi_proto_app$(ObjectSuffix) $(IntermediateDirectory)/src_board_init$(ObjectSuffix) $(IntermediateDirectory)/src_habit_store$(ObjectSuffix) $(IntermediateDirectory)/src_main$(ObjectSuffix) $(IntermediateDirectory)/src_sched_store$(ObjectSuffix) $(IntermediateDirectory)/src_uart_esp$(ObjectSuffix) $(IntermediateDirectory)/src_uart_hal$(ObjectSuffix) \
	$(IntermediateDirectory)/src_uart_protocol$(ObjectSuffix) $(IntermediateDirectory)/src_video$(ObjectSuffix) $(IntermediateDirectory)/src_net_link$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_qr_qrgen$(ObjectSuffix) $(IntermediateDirectory)/UserFolder_qr_net_card$(ObjectSuffix) 



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
	@Baozi.modify.bat $(IntermediateDirectory) $(OutputFile)$(ExeSuffix) 

Always_Link:


##
## Objects
##
$(IntermediateDirectory)/QSPI_test_video_tetra_test$(ObjectSuffix): UserFolder/QSPI_test/video_tetra_test.c  
	$(CC) $(SourceSwitch) UserFolder/QSPI_test/video_tetra_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/QSPI_test_video_tetra_test$(ObjectSuffix) -MF$(IntermediateDirectory)/QSPI_test_video_tetra_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/QSPI_test_video_tetra_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/QSPI_test_video_tetra_test$(PreprocessSuffix): UserFolder/QSPI_test/video_tetra_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/QSPI_test_video_tetra_test$(PreprocessSuffix) UserFolder/QSPI_test/video_tetra_test.c

$(IntermediateDirectory)/anim_face_action$(ObjectSuffix): UserFolder/anim/face_action.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_action.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_action$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_action$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_action$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_action$(PreprocessSuffix): UserFolder/anim/face_action.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_action$(PreprocessSuffix) UserFolder/anim/face_action.c

$(IntermediateDirectory)/anim_face_app$(ObjectSuffix): UserFolder/anim/face_app.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_app.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_app$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_app$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_app$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_app$(PreprocessSuffix): UserFolder/anim/face_app.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_app$(PreprocessSuffix) UserFolder/anim/face_app.c

$(IntermediateDirectory)/anim_face_draw$(ObjectSuffix): UserFolder/anim/face_draw.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_draw.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_draw$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_draw$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_draw$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_draw$(PreprocessSuffix): UserFolder/anim/face_draw.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_draw$(PreprocessSuffix) UserFolder/anim/face_draw.c

$(IntermediateDirectory)/anim_face_model$(ObjectSuffix): UserFolder/anim/face_model.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_model.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_model$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_model$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_model$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_model$(PreprocessSuffix): UserFolder/anim/face_model.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_model$(PreprocessSuffix) UserFolder/anim/face_model.c

$(IntermediateDirectory)/anim_face_render$(ObjectSuffix): UserFolder/anim/face_render.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_render.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_render$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_render$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_render$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_render$(PreprocessSuffix): UserFolder/anim/face_render.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_render$(PreprocessSuffix) UserFolder/anim/face_render.c

$(IntermediateDirectory)/anim_face_ui_card$(ObjectSuffix): UserFolder/anim/face_ui_card.c  
	$(CC) $(SourceSwitch) UserFolder/anim/face_ui_card.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/anim_face_ui_card$(ObjectSuffix) -MF$(IntermediateDirectory)/anim_face_ui_card$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/anim_face_ui_card$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/anim_face_ui_card$(PreprocessSuffix): UserFolder/anim/face_ui_card.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/anim_face_ui_card$(PreprocessSuffix) UserFolder/anim/face_ui_card.c

$(IntermediateDirectory)/UserFolder_app_core$(ObjectSuffix): UserFolder/app_core.c  
	$(CC) $(SourceSwitch) UserFolder/app_core.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_app_core$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_app_core$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_app_core$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_app_core$(PreprocessSuffix): UserFolder/app_core.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_app_core$(PreprocessSuffix) UserFolder/app_core.c

$(IntermediateDirectory)/UserFolder_behavior$(ObjectSuffix): UserFolder/behavior.c  
	$(CC) $(SourceSwitch) UserFolder/behavior.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_behavior$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_behavior$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_behavior$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_behavior$(PreprocessSuffix): UserFolder/behavior.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_behavior$(PreprocessSuffix) UserFolder/behavior.c

$(IntermediateDirectory)/emotion_emotion_fsm$(ObjectSuffix): UserFolder/emotion/emotion_fsm.c  
	$(CC) $(SourceSwitch) UserFolder/emotion/emotion_fsm.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/emotion_emotion_fsm$(ObjectSuffix) -MF$(IntermediateDirectory)/emotion_emotion_fsm$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/emotion_emotion_fsm$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/emotion_emotion_fsm$(PreprocessSuffix): UserFolder/emotion/emotion_fsm.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/emotion_emotion_fsm$(PreprocessSuffix) UserFolder/emotion/emotion_fsm.c

$(IntermediateDirectory)/emotion_emotion_manager$(ObjectSuffix): UserFolder/emotion/emotion_manager.c  
	$(CC) $(SourceSwitch) UserFolder/emotion/emotion_manager.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/emotion_emotion_manager$(ObjectSuffix) -MF$(IntermediateDirectory)/emotion_emotion_manager$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/emotion_emotion_manager$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/emotion_emotion_manager$(PreprocessSuffix): UserFolder/emotion/emotion_manager.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/emotion_emotion_manager$(PreprocessSuffix) UserFolder/emotion/emotion_manager.c

$(IntermediateDirectory)/emotion_mood_fsm$(ObjectSuffix): UserFolder/emotion/mood_fsm.c  
	$(CC) $(SourceSwitch) UserFolder/emotion/mood_fsm.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/emotion_mood_fsm$(ObjectSuffix) -MF$(IntermediateDirectory)/emotion_mood_fsm$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/emotion_mood_fsm$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/emotion_mood_fsm$(PreprocessSuffix): UserFolder/emotion/mood_fsm.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/emotion_mood_fsm$(PreprocessSuffix) UserFolder/emotion/mood_fsm.c

$(IntermediateDirectory)/gfx_gfx$(ObjectSuffix): UserFolder/gfx/gfx.c  
	$(CC) $(SourceSwitch) UserFolder/gfx/gfx.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/gfx_gfx$(ObjectSuffix) -MF$(IntermediateDirectory)/gfx_gfx$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/gfx_gfx$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/gfx_gfx$(PreprocessSuffix): UserFolder/gfx/gfx.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/gfx_gfx$(PreprocessSuffix) UserFolder/gfx/gfx.c

$(IntermediateDirectory)/gfx_gfx_demo$(ObjectSuffix): UserFolder/gfx/gfx_demo.c  
	$(CC) $(SourceSwitch) UserFolder/gfx/gfx_demo.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/gfx_gfx_demo$(ObjectSuffix) -MF$(IntermediateDirectory)/gfx_gfx_demo$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/gfx_gfx_demo$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/gfx_gfx_demo$(PreprocessSuffix): UserFolder/gfx/gfx_demo.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/gfx_gfx_demo$(PreprocessSuffix) UserFolder/gfx/gfx_demo.c

$(IntermediateDirectory)/gfx_gfx_sprite_demo$(ObjectSuffix): UserFolder/gfx/gfx_sprite_demo.c  
	$(CC) $(SourceSwitch) UserFolder/gfx/gfx_sprite_demo.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/gfx_gfx_sprite_demo$(ObjectSuffix) -MF$(IntermediateDirectory)/gfx_gfx_sprite_demo$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/gfx_gfx_sprite_demo$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/gfx_gfx_sprite_demo$(PreprocessSuffix): UserFolder/gfx/gfx_sprite_demo.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/gfx_gfx_sprite_demo$(PreprocessSuffix) UserFolder/gfx/gfx_sprite_demo.c

$(IntermediateDirectory)/movements_moves$(ObjectSuffix): UserFolder/movements/moves.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves$(PreprocessSuffix): UserFolder/movements/moves.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves$(PreprocessSuffix) UserFolder/movements/moves.c

$(IntermediateDirectory)/movements_moves_angry$(ObjectSuffix): UserFolder/movements/moves_angry.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_angry.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_angry$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_angry$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_angry$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_angry$(PreprocessSuffix): UserFolder/movements/moves_angry.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_angry$(PreprocessSuffix) UserFolder/movements/moves_angry.c

$(IntermediateDirectory)/movements_moves_bored$(ObjectSuffix): UserFolder/movements/moves_bored.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_bored.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_bored$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_bored$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_bored$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_bored$(PreprocessSuffix): UserFolder/movements/moves_bored.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_bored$(PreprocessSuffix) UserFolder/movements/moves_bored.c

$(IntermediateDirectory)/movements_moves_demo$(ObjectSuffix): UserFolder/movements/moves_demo.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_demo.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_demo$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_demo$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_demo$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_demo$(PreprocessSuffix): UserFolder/movements/moves_demo.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_demo$(PreprocessSuffix) UserFolder/movements/moves_demo.c

$(IntermediateDirectory)/movements_moves_eight$(ObjectSuffix): UserFolder/movements/moves_eight.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_eight.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_eight$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_eight$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_eight$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_eight$(PreprocessSuffix): UserFolder/movements/moves_eight.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_eight$(PreprocessSuffix) UserFolder/movements/moves_eight.c

$(IntermediateDirectory)/movements_moves_happy$(ObjectSuffix): UserFolder/movements/moves_happy.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_happy.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_happy$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_happy$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_happy$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_happy$(PreprocessSuffix): UserFolder/movements/moves_happy.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_happy$(PreprocessSuffix) UserFolder/movements/moves_happy.c

$(IntermediateDirectory)/movements_moves_idle$(ObjectSuffix): UserFolder/movements/moves_idle.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_idle.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_idle$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_idle$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_idle$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_idle$(PreprocessSuffix): UserFolder/movements/moves_idle.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_idle$(PreprocessSuffix) UserFolder/movements/moves_idle.c

$(IntermediateDirectory)/movements_moves_loving$(ObjectSuffix): UserFolder/movements/moves_loving.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_loving.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_loving$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_loving$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_loving$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_loving$(PreprocessSuffix): UserFolder/movements/moves_loving.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_loving$(PreprocessSuffix) UserFolder/movements/moves_loving.c

$(IntermediateDirectory)/movements_moves_sad$(ObjectSuffix): UserFolder/movements/moves_sad.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_sad.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_sad$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_sad$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_sad$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_sad$(PreprocessSuffix): UserFolder/movements/moves_sad.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_sad$(PreprocessSuffix) UserFolder/movements/moves_sad.c

$(IntermediateDirectory)/movements_moves_sleep$(ObjectSuffix): UserFolder/movements/moves_sleep.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_sleep.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_sleep$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_sleep$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_sleep$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_sleep$(PreprocessSuffix): UserFolder/movements/moves_sleep.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_sleep$(PreprocessSuffix) UserFolder/movements/moves_sleep.c

$(IntermediateDirectory)/movements_moves_surprised$(ObjectSuffix): UserFolder/movements/moves_surprised.c  
	$(CC) $(SourceSwitch) UserFolder/movements/moves_surprised.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/movements_moves_surprised$(ObjectSuffix) -MF$(IntermediateDirectory)/movements_moves_surprised$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/movements_moves_surprised$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/movements_moves_surprised$(PreprocessSuffix): UserFolder/movements/moves_surprised.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/movements_moves_surprised$(PreprocessSuffix) UserFolder/movements/moves_surprised.c

$(IntermediateDirectory)/UserFolder_play_card$(ObjectSuffix): UserFolder/play_card.c  
	$(CC) $(SourceSwitch) UserFolder/play_card.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_play_card$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_play_card$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_play_card$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_play_card$(PreprocessSuffix): UserFolder/play_card.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_play_card$(PreprocessSuffix) UserFolder/play_card.c

$(IntermediateDirectory)/UserFolder_pomo$(ObjectSuffix): UserFolder/pomo.c  
	$(CC) $(SourceSwitch) UserFolder/pomo.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_pomo$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_pomo$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_pomo$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_pomo$(PreprocessSuffix): UserFolder/pomo.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_pomo$(PreprocessSuffix) UserFolder/pomo.c

$(IntermediateDirectory)/servo_test_servo_core$(ObjectSuffix): UserFolder/servo_test/servo_core.c  
	$(CC) $(SourceSwitch) UserFolder/servo_test/servo_core.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/servo_test_servo_core$(ObjectSuffix) -MF$(IntermediateDirectory)/servo_test_servo_core$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/servo_test_servo_core$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/servo_test_servo_core$(PreprocessSuffix): UserFolder/servo_test/servo_core.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/servo_test_servo_core$(PreprocessSuffix) UserFolder/servo_test/servo_core.c

$(IntermediateDirectory)/servo_test_servo_dual_test$(ObjectSuffix): UserFolder/servo_test/servo_dual_test.c  
	$(CC) $(SourceSwitch) UserFolder/servo_test/servo_dual_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/servo_test_servo_dual_test$(ObjectSuffix) -MF$(IntermediateDirectory)/servo_test_servo_dual_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/servo_test_servo_dual_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/servo_test_servo_dual_test$(PreprocessSuffix): UserFolder/servo_test/servo_dual_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/servo_test_servo_dual_test$(PreprocessSuffix) UserFolder/servo_test/servo_dual_test.c

$(IntermediateDirectory)/servo_test_servo_mvp_test$(ObjectSuffix): UserFolder/servo_test/servo_mvp_test.c  
	$(CC) $(SourceSwitch) UserFolder/servo_test/servo_mvp_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/servo_test_servo_mvp_test$(ObjectSuffix) -MF$(IntermediateDirectory)/servo_test_servo_mvp_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/servo_test_servo_mvp_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/servo_test_servo_mvp_test$(PreprocessSuffix): UserFolder/servo_test/servo_mvp_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/servo_test_servo_mvp_test$(PreprocessSuffix) UserFolder/servo_test/servo_mvp_test.c

$(IntermediateDirectory)/servo_test_servo_range_test$(ObjectSuffix): UserFolder/servo_test/servo_range_test.c  
	$(CC) $(SourceSwitch) UserFolder/servo_test/servo_range_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/servo_test_servo_range_test$(ObjectSuffix) -MF$(IntermediateDirectory)/servo_test_servo_range_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/servo_test_servo_range_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/servo_test_servo_range_test$(PreprocessSuffix): UserFolder/servo_test/servo_range_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/servo_test_servo_range_test$(PreprocessSuffix) UserFolder/servo_test/servo_range_test.c

$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix): UserFolder/test.c  
	$(CC) $(SourceSwitch) UserFolder/test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_test$(PreprocessSuffix): UserFolder/test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_test$(PreprocessSuffix) UserFolder/test.c

$(IntermediateDirectory)/test_emotion_test$(ObjectSuffix): UserFolder/test/emotion_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/emotion_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_emotion_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_emotion_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_emotion_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_emotion_test$(PreprocessSuffix): UserFolder/test/emotion_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_emotion_test$(PreprocessSuffix) UserFolder/test/emotion_test.c

$(IntermediateDirectory)/test_ir_state_test$(ObjectSuffix): UserFolder/test/ir_state_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/ir_state_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_ir_state_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_ir_state_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_ir_state_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_ir_state_test$(PreprocessSuffix): UserFolder/test/ir_state_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_ir_state_test$(PreprocessSuffix) UserFolder/test/ir_state_test.c

$(IntermediateDirectory)/test_ir_test$(ObjectSuffix): UserFolder/test/ir_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/ir_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_ir_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_ir_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_ir_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_ir_test$(PreprocessSuffix): UserFolder/test/ir_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_ir_test$(PreprocessSuffix) UserFolder/test/ir_test.c

$(IntermediateDirectory)/test_serial_min_test$(ObjectSuffix): UserFolder/test/serial_min_test.c  
	$(CC) $(SourceSwitch) UserFolder/test/serial_min_test.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/test_serial_min_test$(ObjectSuffix) -MF$(IntermediateDirectory)/test_serial_min_test$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/test_serial_min_test$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/test_serial_min_test$(PreprocessSuffix): UserFolder/test/serial_min_test.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/test_serial_min_test$(PreprocessSuffix) UserFolder/test/serial_min_test.c

$(IntermediateDirectory)/tracking_track_dual$(ObjectSuffix): UserFolder/tracking/track_dual.c  
	$(CC) $(SourceSwitch) UserFolder/tracking/track_dual.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/tracking_track_dual$(ObjectSuffix) -MF$(IntermediateDirectory)/tracking_track_dual$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/tracking_track_dual$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/tracking_track_dual$(PreprocessSuffix): UserFolder/tracking/track_dual.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/tracking_track_dual$(PreprocessSuffix) UserFolder/tracking/track_dual.c

$(IntermediateDirectory)/tracking_track_yaw$(ObjectSuffix): UserFolder/tracking/track_yaw.c  
	$(CC) $(SourceSwitch) UserFolder/tracking/track_yaw.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/tracking_track_yaw$(ObjectSuffix) -MF$(IntermediateDirectory)/tracking_track_yaw$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/tracking_track_yaw$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/tracking_track_yaw$(PreprocessSuffix): UserFolder/tracking/track_yaw.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/tracking_track_yaw$(PreprocessSuffix) UserFolder/tracking/track_yaw.c

$(IntermediateDirectory)/UserFolder_ui$(ObjectSuffix): UserFolder/ui.c  
	$(CC) $(SourceSwitch) UserFolder/ui.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_ui$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_ui$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_ui$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_ui$(PreprocessSuffix): UserFolder/ui.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_ui$(PreprocessSuffix) UserFolder/ui.c

$(IntermediateDirectory)/UserFolder_ui_app$(ObjectSuffix): UserFolder/ui_app.c  
	$(CC) $(SourceSwitch) UserFolder/ui_app.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_ui_app$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_ui_app$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_ui_app$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_ui_app$(PreprocessSuffix): UserFolder/ui_app.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_ui_app$(PreprocessSuffix) UserFolder/ui_app.c

$(IntermediateDirectory)/chip_config$(ObjectSuffix): chip_config.c  
	$(CC) $(SourceSwitch) chip_config.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/chip_config$(ObjectSuffix) -MF$(IntermediateDirectory)/chip_config$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/chip_config$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/chip_config$(PreprocessSuffix): chip_config.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/chip_config$(PreprocessSuffix) chip_config.c

$(IntermediateDirectory)/src_ICNT8952BN$(ObjectSuffix): src/ICNT8952BN.c  
	$(CC) $(SourceSwitch) src/ICNT8952BN.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_ICNT8952BN$(ObjectSuffix) -MF$(IntermediateDirectory)/src_ICNT8952BN$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_ICNT8952BN$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_ICNT8952BN$(PreprocessSuffix): src/ICNT8952BN.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_ICNT8952BN$(PreprocessSuffix) src/ICNT8952BN.c

$(IntermediateDirectory)/src_baozi_proto_app$(ObjectSuffix): src/baozi_proto_app.c  
	$(CC) $(SourceSwitch) src/baozi_proto_app.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_baozi_proto_app$(ObjectSuffix) -MF$(IntermediateDirectory)/src_baozi_proto_app$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_baozi_proto_app$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_baozi_proto_app$(PreprocessSuffix): src/baozi_proto_app.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_baozi_proto_app$(PreprocessSuffix) src/baozi_proto_app.c

$(IntermediateDirectory)/src_board_init$(ObjectSuffix): src/board_init.c  
	$(CC) $(SourceSwitch) src/board_init.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_board_init$(ObjectSuffix) -MF$(IntermediateDirectory)/src_board_init$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_board_init$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_board_init$(PreprocessSuffix): src/board_init.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_board_init$(PreprocessSuffix) src/board_init.c

$(IntermediateDirectory)/src_habit_store$(ObjectSuffix): src/habit_store.c  
	$(CC) $(SourceSwitch) src/habit_store.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_habit_store$(ObjectSuffix) -MF$(IntermediateDirectory)/src_habit_store$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_habit_store$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_habit_store$(PreprocessSuffix): src/habit_store.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_habit_store$(PreprocessSuffix) src/habit_store.c

$(IntermediateDirectory)/src_main$(ObjectSuffix): src/main.c  
	$(CC) $(SourceSwitch) src/main.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_main$(ObjectSuffix) -MF$(IntermediateDirectory)/src_main$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_main$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_main$(PreprocessSuffix): src/main.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_main$(PreprocessSuffix) src/main.c

$(IntermediateDirectory)/src_sched_store$(ObjectSuffix): src/sched_store.c  
	$(CC) $(SourceSwitch) src/sched_store.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_sched_store$(ObjectSuffix) -MF$(IntermediateDirectory)/src_sched_store$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_sched_store$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_sched_store$(PreprocessSuffix): src/sched_store.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_sched_store$(PreprocessSuffix) src/sched_store.c

$(IntermediateDirectory)/src_uart_esp$(ObjectSuffix): src/uart_esp.c  
	$(CC) $(SourceSwitch) src/uart_esp.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_uart_esp$(ObjectSuffix) -MF$(IntermediateDirectory)/src_uart_esp$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_uart_esp$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_uart_esp$(PreprocessSuffix): src/uart_esp.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_uart_esp$(PreprocessSuffix) src/uart_esp.c

$(IntermediateDirectory)/src_uart_hal$(ObjectSuffix): src/uart_hal.c  
	$(CC) $(SourceSwitch) src/uart_hal.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_uart_hal$(ObjectSuffix) -MF$(IntermediateDirectory)/src_uart_hal$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_uart_hal$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_uart_hal$(PreprocessSuffix): src/uart_hal.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_uart_hal$(PreprocessSuffix) src/uart_hal.c

$(IntermediateDirectory)/src_uart_protocol$(ObjectSuffix): src/uart_protocol.c  
	$(CC) $(SourceSwitch) src/uart_protocol.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_uart_protocol$(ObjectSuffix) -MF$(IntermediateDirectory)/src_uart_protocol$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_uart_protocol$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_uart_protocol$(PreprocessSuffix): src/uart_protocol.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_uart_protocol$(PreprocessSuffix) src/uart_protocol.c

$(IntermediateDirectory)/src_video$(ObjectSuffix): src/video.c  
	$(CC) $(SourceSwitch) src/video.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_video$(ObjectSuffix) -MF$(IntermediateDirectory)/src_video$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_video$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_video$(PreprocessSuffix): src/video.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_video$(PreprocessSuffix) src/video.c


$(IntermediateDirectory)/src_net_link$(ObjectSuffix): src/net_link.c  
	$(CC) $(SourceSwitch) src/net_link.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/src_net_link$(ObjectSuffix) -MF$(IntermediateDirectory)/src_net_link$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/src_net_link$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/src_net_link$(PreprocessSuffix): src/net_link.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/src_net_link$(PreprocessSuffix) src/net_link.c

$(IntermediateDirectory)/UserFolder_qr_qrgen$(ObjectSuffix): UserFolder/qr/qrgen.c  
	$(CC) $(SourceSwitch) UserFolder/qr/qrgen.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_qr_qrgen$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_qr_qrgen$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_qr_qrgen$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_qr_qrgen$(PreprocessSuffix): UserFolder/qr/qrgen.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_qr_qrgen$(PreprocessSuffix) UserFolder/qr/qrgen.c

$(IntermediateDirectory)/UserFolder_qr_net_card$(ObjectSuffix): UserFolder/qr/net_card.c  
	$(CC) $(SourceSwitch) UserFolder/qr/net_card.c $(CFLAGS) -MMD -MP -MT$(IntermediateDirectory)/UserFolder_qr_net_card$(ObjectSuffix) -MF$(IntermediateDirectory)/UserFolder_qr_net_card$(DependSuffix) $(ObjectSwitch)$(IntermediateDirectory)/UserFolder_qr_net_card$(ObjectSuffix) $(IncludeCPath) $(IncludePackagePath)
Lst/UserFolder_qr_net_card$(PreprocessSuffix): UserFolder/qr/net_card.c
	$(CC) $(CFLAGS)$(IncludeCPath) $(PreprocessOnlySwitch) $(OutputSwitch) Lst/UserFolder_qr_net_card$(PreprocessSuffix) UserFolder/qr/net_card.c

-include $(IntermediateDirectory)/*$(DependSuffix)
