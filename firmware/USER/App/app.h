#ifndef __APP_H
#define __APP_H

#include "common.h"

/* 应用层入口：
 *   main() 里调用 App_Init() 做一次初始化，然后在 while(1) 里反复调用 App_Loop()。
 *   这样 main.c 保持 CubeMX 原样，业务代码全在 USER/ 目录下。 */
void App_Init(void);
void App_Loop(void);

#endif
