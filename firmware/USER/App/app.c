/*==============================================================================
 *  app.c  ---  角色选择器（本工程"一个工程、两个 Target"的关键）
 *
 *  两个 Target 编的文件列表**完全一样**，只差一个宏：
 *      Target `board1`  Define:  BOARD_SEL=1
 *      Target `board2`  Define:  BOARD_SEL=2
 *  于是所有"角色不同"的东西都靠 BOARD_SEL 分叉，**不需要在 Keil 里给每个
 *  Target 单独排除文件**（那套 XML 又脆又难维护）。
 *
 *  ★ 实验5 阶段3：两板功能已**对称** —— 只留一份共享实现 app_node.c
 *    （原 app_board1.c / app_board2.c 已合并、删除）。
 *    板间差异收敛为 3 处：LED 效果（led_effect.c 按 BOARD_SEL 分）、
 *    报警有无（仅板1，见 app_node.c 的 BOARD_SEL 分叉）、WiFi 角色（esp01s.c 选择器）。
 *  对外接口一样（App_Init / App_Loop，见 app.h）。
 *============================================================================*/
#include "app.h"
#include "common.h"

#include "app_node.c"
