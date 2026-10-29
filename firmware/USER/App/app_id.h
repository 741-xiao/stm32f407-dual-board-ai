/*==============================================================================
 *  app_id.h  ---  开发者姓名 / 学号 / WiFi，一处开关全换
 *
 *  换人只改下面这一个 STU_WHO，LCD、串口、上位机网页、WiFi 联网四处一起跟着变。
 *
 *  为什么姓名有三份数据：
 *    STU_ID       学号是纯 ASCII，LCD 和串口都直接用；
 *    STU_NAME_GB  姓名按 GB18030 编码成字节数组，给串口和上位机网页用
 *                 （源码里不能出现中文字符串字面量，ARMCC 报 #870-D）；
 *    STU_NAME_HZ  姓名对应的 24x24 点阵下标，给 LCD 用，见 chinese_lcd.h。
 *
 *  ★ WiFi 名/密码也在这里：esp01s.h 会 #include 本文件取 STU_WIFI_SSID / STU_WIFI_PASS。
 *    板1、板2 各有一份本文件，改人时**两边的 STU_WHO 都要设成同一个人**。
 *    新增第 7 个人要另造姓名点阵（见 chinese_lcd.c 字库），本文件只有这 6 人现成。
 *============================================================================*/
#ifndef __APP_ID_H
#define __APP_ID_H

/*  0 = 郑鑫   1 = 肖肃   2 = 刘星宇   3 = 李钟海   4 = 陈浩   5 = 李曦亮  */
#define STU_WHO             1

#if STU_WHO == 0
#define STU_ID              "202440700102"
#define STU_NAME_GB         {0xD6,0xA3,0xF6,0xCE}
#define STU_NAME_GB_N       4U
#define STU_NAME_HZ         {12U, 13U}
#define STU_NAME_HZ_N       2U
#define STU_WIFI_SSID       "YOUR_WIFI_SSID"
#define STU_WIFI_PASS       "YOUR_WIFI_PASSWORD"

#elif STU_WHO == 1
#define STU_ID              "202440700102"      /* TODO: 换成真实学号 */
#define STU_NAME_GB         {0xD0,0xA4,0xCB,0xE0}
#define STU_NAME_GB_N       4U
#define STU_NAME_HZ         {32U, 33U}
#define STU_NAME_HZ_N       2U
#define STU_WIFI_SSID       "YOUR_WIFI_SSID"    /* TODO: 换成本人的热点名 */
#define STU_WIFI_PASS       "YOUR_WIFI_PASSWORD"    /* TODO: 换成本人的热点密码 */

#elif STU_WHO == 2
#define STU_ID              "202440700116"      /* TODO: 换成真实学号 */
#define STU_NAME_GB         {0xC1,0xF5,0xD0,0xC7,0xD3,0xEE}
#define STU_NAME_GB_N       6U
#define STU_NAME_HZ         {34U, 35U, 36U}
#define STU_NAME_HZ_N       3U
#define STU_WIFI_SSID       "TODO_WIFI_NAME"    /* TODO: 换成本人的热点名 */
#define STU_WIFI_PASS       "TODO_WIFI_PASS"    /* TODO: 换成本人的热点密码 */

#elif STU_WHO == 3
#define STU_ID              "202440700105"      /* TODO: 换成真实学号 */
#define STU_NAME_GB         {0xC0,0xEE,0xD6,0xD3,0xBA,0xA3}
#define STU_NAME_GB_N       6U
#define STU_NAME_HZ         {37U, 38U, 39U}
#define STU_NAME_HZ_N       3U
#define STU_WIFI_SSID       "TODO_WIFI_NAME"    /* TODO: 换成本人的热点名 */
#define STU_WIFI_PASS       "TODO_WIFI_PASS"    /* TODO: 换成本人的热点密码 */

#elif STU_WHO == 4
#define STU_ID              "202440700104"      /* TODO: 换成真实学号 */
#define STU_NAME_GB         {0xB3,0xC2,0xBA,0xC6}
#define STU_NAME_GB_N       4U
#define STU_NAME_HZ         {40U, 41U}
#define STU_NAME_HZ_N       2U
#define STU_WIFI_SSID       "TODO_WIFI_NAME"    /* TODO: 换成本人的热点名 */
#define STU_WIFI_PASS       "TODO_WIFI_PASS"    /* TODO: 换成本人的热点密码 */

#elif STU_WHO == 5
#define STU_ID              "202440700107"      /* TODO: 换成真实学号 */
#define STU_NAME_GB         {0xC0,0xEE,0xEA,0xD8,0xC1,0xC1}
#define STU_NAME_GB_N       6U
#define STU_NAME_HZ         {37U, 42U, 43U}
#define STU_NAME_HZ_N       3U
#define STU_WIFI_SSID       "TODO_WIFI_NAME"    /* TODO: 换成本人的热点名 */
#define STU_WIFI_PASS       "TODO_WIFI_PASS"    /* TODO: 换成本人的热点密码 */

#else
#error "STU_WHO 只能是 0~5"
#endif

#endif
