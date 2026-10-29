/*==============================================================================
 *  ai_model.c  ---  题2 推理封装实现（集成点只有这一个文件）
 *
 *  依赖：
 *    ai_norm.h              —— 归一化参数（tools/train_nn.py 生成，与模型同一次训练）
 *    network.h              —— X-CUBE-AI 生成的网络 API（stedgeai generate 产出）
 *    network_data_params.h  —— 激活/权重尺寸
 *    runtime/Lib/NetworkRuntime1020_CM4_Keil.lib  —— AI 运行时（Keil/AC5 版）
 *
 *  ★★ 归一化必须与训练完全一致：
 *        x_norm[i] = (x_raw[i] - AI_MEAN[i]) / AI_STD[i]
 *     顺序固定为：temp(℃) humi(%RH) light(0..4095) eco2(ppm) tvoc(ppb)
 *     少做/做错这一步，模型的输出就是"看起来有数、其实全错"。
 *
 *  ★ 模型信息（stedgeai analyze 报告）：
 *      input  'input'  f32(1x5)  20 B       output 'output' f32(1x2)  8 B
 *      weights(ro) 1,608 B   activations(rw) 128 B   macc 464
 *============================================================================*/
#include "common.h"
#include "ai_model.h"
#include "ai_norm.h"        /* AI_MEAN / AI_STD / AI_NORM_N —— 训练时导出的 */

#if (AI_ENABLED != 0)

/*------------------------------------------------------------------------------
 *  ==== 集成区：X-CUBE-AI 生成代码 ====
 *---------------------------------------------------------------------------*/
#include "network.h"
#include "network_data_params.h"

static ai_handle  s_nn  = AI_HANDLE_NULL;   /* 网络句柄 */
static ai_buffer *s_in  = NULL;             /* 输入缓冲区描述 */
static ai_buffer *s_out = NULL;             /* 输出缓冲区描述 */
static u8         s_try = 0U;               /* create/init 失败过几次（失败 3 次就不再试）*/

/* 激活区（尺寸由 network_data_params.h 给出：本模型只有 128 B）
   ★ 必须 4 字节对齐（AI_NETWORK_ACTIVATIONS_ALIGNMENT = 4）*/
AI_ALIGNED(4) static ai_u8 s_act[AI_NETWORK_DATA_ACTIVATIONS_SIZE];

/* 输入/输出数据缓冲：模型是 float32 —— 5 进（温湿光CO2 TVOC）、2 出（板1/板2 概率）*/
static float s_in_buf[AI_NORM_N];
static float s_out_buf[2];

/* 建网 + 初始化（幂等）。成功返回 1。
   ★ 为什么放在首次推理时懒初始化、而不是 App_Init：
     App_Init 跑在**调度器启动之前**，而 create/init 里有内存对齐检查与错误返回，
     万一失败也只该影响 AiTask 自己（屏上/串口能看到提示），不该拖住整个系统启动。*/
static u8 ai_ensure(void)
{
    ai_error err;

    if (s_nn != AI_HANDLE_NULL)
    {
        return 1U;
    }
    if (s_try >= 3U)                /* 试过 3 次还不行就别反复试了 */
    {
        return 0U;
    }
    s_try++;

    /* 权重在 Flash 里（const），所以 weights 传 NULL 即可；
       激活区由调用方提供 —— 这里给的是上面那块 128 B 的静态缓冲。*/
    {
        const ai_handle acts[] = { AI_HANDLE_PTR(s_act) };
        err = ai_network_create_and_init(&s_nn, acts, NULL);
    }
    if (err.type != AI_ERROR_NONE)
    {
        s_nn = AI_HANDLE_NULL;
        return 0U;
    }

    s_in  = ai_network_inputs_get(s_nn, NULL);
    s_out = ai_network_outputs_get(s_nn, NULL);
    if ((s_in == NULL) || (s_out == NULL))
    {
        (void)ai_network_destroy(s_nn);
        s_nn = AI_HANDLE_NULL;
        return 0U;
    }

    /* 把 I/O 缓冲挂上去（之后每帧只写数据，不再改指针）*/
    s_in[0].data  = AI_HANDLE_PTR(s_in_buf);
    s_out[0].data = AI_HANDLE_PTR(s_out_buf);
    return 1U;
}

u8 Ai_Ready(void)
{
    return (u8)((ai_ensure() != 0U) ? 1U : 0U);
}

u8 Ai_Infer(s32 temp10, s32 humi10, u32 light, u32 eco2, u32 tvoc,
            u8 *cls, u8 *conf)
{
    ai_i32 n;
    float  p1;
    float  p2;

    if ((cls == NULL) || (conf == NULL))
    {
        return 0U;
    }
    if (ai_ensure() == 0U)
    {
        return 0U;
    }

    /* ★ 与训练完全一致的 z-score。
       喂几个通道由 ai_norm.h 的 AI_NORM_N 决定 —— train_nn.py --features 改几个，
       这里跟着变，不用手改。顺序固定：temp humi light eco2 tvoc。
       多出来的通道**不喂给网络**，但它照样在采集 / 显示 / 上报（题面要求的 5 项不变）。*/
    s_in_buf[0] = ((((float)temp10) / 10.0f) - AI_MEAN[0]) / AI_STD[0];
#if (AI_NORM_N > 1)
    s_in_buf[1] = ((((float)humi10) / 10.0f) - AI_MEAN[1]) / AI_STD[1];
#endif
#if (AI_NORM_N > 2)
    s_in_buf[2] = (((float)light) - AI_MEAN[2]) / AI_STD[2];
#endif
#if (AI_NORM_N > 3)
    s_in_buf[3] = (((float)eco2) - AI_MEAN[3]) / AI_STD[3];
#endif
#if (AI_NORM_N > 4)
    s_in_buf[4] = (((float)tvoc) - AI_MEAN[4]) / AI_STD[4];
#endif

    /* 没被用到的参数显式"消费"掉，免得编译器报 unused（0 Warning 是硬要求）*/
#if (AI_NORM_N < 2)
    (void)humi10;
#endif
#if (AI_NORM_N < 3)
    (void)light;
#endif
#if (AI_NORM_N < 4)
    (void)eco2;
#endif
#if (AI_NORM_N < 5)
    (void)tvoc;
#endif

    n = ai_network_run(s_nn, s_in, s_out);
    if (n <= 0)
    {
        return 0U;
    }

    p1 = s_out_buf[0];                  /* 判为板1 的概率 */
    p2 = s_out_buf[1];                  /* 判为板2 的概率 */

    if (p1 >= p2)
    {
        *cls  = 1U;
        *conf = (u8)((p1 * 100.0f) + 0.5f);
    }
    else
    {
        *cls  = 2U;
        *conf = (u8)((p2 * 100.0f) + 0.5f);
    }
    return 1U;
}

const char *Ai_Info(void)
{
    /* 自己拼一行，免得动 printf 的格式支持。
       ★ 输入维度不再写死 —— 看 ai_norm.h 的 AI_NORM_N（--features 决定）。*/
    return "network: MLP 16-16-2 (input dim = AI_NORM_N)";
}

#else   /* ---------------- 还没集成：全部做成空实现，工程照样编译 ---------------- */

u8 Ai_Ready(void)
{
    return 0U;                  /* app 层据此显示 "--" 且不发 AI 字段 */
}

u8 Ai_Infer(s32 temp10, s32 humi10, u32 light, u32 eco2, u32 tvoc,
            u8 *cls, u8 *conf)
{
    (void)temp10; (void)humi10; (void)light; (void)eco2; (void)tvoc;
    (void)cls; (void)conf;
    return 0U;
}

const char *Ai_Info(void)
{
    return "not integrated (AI_ENABLED=0)";
}

#endif
