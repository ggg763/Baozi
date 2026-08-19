"""
command_mapper.py

⚠️ 已停用（架构调整 v1.0）

本模块原负责"LLM 动作指令 → 标准动作字典"的映射，
自架构调整后 PC 端不再下发动作指令（LLM 只输出
reply/emotion/intent），动作决策已迁移至 TD1601 行为层
（td1601_code/src/behavior.c + motion.c）。

保留本文件仅作历史参考，主链路（brain/main）不再引用。

---
（原职责说明）

包子机器人动作映射模块


职责：

- 接收LLM输出动作
- 校验动作合法性
- 转换为机器人执行指令


不负责：

- LLM生成
- 舵机控制
- UART通信
- ESP32通信


输入：

    command字符串


输出：

    标准机器人动作字典


"""


from typing import Dict


class CommandMapper:



    # 当前支持动作

    ALLOWED_COMMANDS = {


        "SERVO_NOD",

        "SERVO_SHAKE",

        "SERVO_H",

        "SERVO_V",

        "SCREEN",

        "LED",

        "NONE"

    }



    def __init__(self):

        pass



    def map(
        self,
        command: str
    ) -> Dict:
        """
        将LLM命令转换成标准格式


        输入:

        "SERVO_NOD 1"


        输出:

        {
            "type":"servo",
            "action":"nod",
            "value":1
        }

        """


        if not command:

            return self.none()



        command = command.strip()



        parts = command.split()



        action = parts[0]



        if action not in self.ALLOWED_COMMANDS:

            return self.none()



        # ======================
        # 舵机动作
        # ======================

        if action == "SERVO_NOD":


            count = self._number(
                parts,
                default=1
            )


            return {

                "type":
                    "servo",

                "action":
                    "nod",

                "value":
                    count

            }



        if action == "SERVO_SHAKE":


            count = self._number(
                parts,
                default=1
            )


            return {

                "type":
                    "servo",

                "action":
                    "shake",

                "value":
                    count

            }




        if action == "SERVO_H":


            angle = self._number(
                parts,
                default=90
            )


            return {

                "type":
                    "servo",

                "action":
                    "horizontal",

                "value":
                    angle

            }




        if action == "SERVO_V":


            angle = self._number(
                parts,
                default=90
            )


            return {

                "type":
                    "servo",

                "action":
                    "vertical",

                "value":
                    angle

            }



        # ======================
        # 屏幕
        # ======================

        if action == "SCREEN":


            expression = (

                parts[1]

                if len(parts)>1

                else

                "neutral"

            )


            return {


                "type":

                    "screen",


                "expression":

                    expression


            }




        # ======================
        # LED
        # ======================

        if action == "LED":


            color = (

                parts[1]

                if len(parts)>1

                else

                "white"

            )


            return {


                "type":

                    "led",


                "color":

                    color


            }




        return self.none()



    def none(self):

        """
        空动作
        """

        return {

            "type":

                "none"

        }




    def _number(
        self,
        parts,
        default=1
    ):

        """
        安全数字解析
        """

        if len(parts)<2:

            return default



        try:

            return int(
                parts[1]
            )


        except ValueError:

            return default