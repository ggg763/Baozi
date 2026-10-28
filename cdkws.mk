.PHONY: clean All Project_Title Project_PreBuild chip_td1601 console csi minilibc mm td1601_evb Project_Build Project_PostBuild

All: Project_Title Project_PreBuild chip_td1601 console csi minilibc mm td1601_evb Project_Build Project_PostBuild

Project_Title:
	@echo "----------Building project:[ Baozi - BuildSet ]----------"

Project_PreBuild:
	@echo Executing Pre Build commands ...
	@export BOARD_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/boards/td1601_evb/v1.0" CDKPath="D:/Baozi/CDK/environment" CDK_VERSION="V2.24.19" CHIP_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/components/chips/chip_td1601/v1.0" CPU="E906FD" ProjectName="Baozi" ProjectPath="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/" SOLUTION_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/" chip_td1601="V1.0.0" console="V1.0.0" csi="V1.0.0" minilibc="V1.0.0" mm="V1.0.0" td1601_evb="V1.0.0" && "D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/utilities/pre_build.sh"
	@echo Done

chip_td1601:
	@make -r -f Obj/BuildSet/Packages/chip_td1601/V1.0.0/Makefile -j 32 -C  ./ 

console:
	@make -r -f Obj/BuildSet/Packages/console/V1.0.0/Makefile -j 32 -C  ./ 

csi:
	@make -r -f Obj/BuildSet/Packages/csi/V1.0.0/Makefile -j 32 -C  ./ 

minilibc:
	@make -r -f Obj/BuildSet/Packages/minilibc/V1.0.0/Makefile -j 32 -C  ./ 

mm:
	@make -r -f Obj/BuildSet/Packages/mm/V1.0.0/Makefile -j 32 -C  ./ 

td1601_evb:
	@make -r -f Obj/BuildSet/Packages/td1601_evb/V1.0.0/Makefile -j 32 -C  ./ 


Project_Build:
	@make -r -f Baozi.mk -j 32 -C  ./ 

Project_PostBuild:
	@echo Executing Post Build commands ...
	@export BOARD_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/boards/td1601_evb/v1.0" CDKPath="D:/Baozi/CDK/environment" CDK_VERSION="V2.24.19" CHIP_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/components/chips/chip_td1601/v1.0" CPU="E906FD" ProjectName="Baozi" ProjectPath="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/" SOLUTION_PATH="D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/" chip_td1601="V1.0.0" console="V1.0.0" csi="V1.0.0" minilibc="V1.0.0" mm="V1.0.0" td1601_evb="V1.0.0" && "D:/Baozi/CDK/official_SDK/td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59/td1601_evb_v0.2.0_cdk/projects/baozi/utilities/aft_build.sh"
	@echo Done


clean:
	@echo "----------Cleaning project:[ Baozi - BuildSet ]----------"

