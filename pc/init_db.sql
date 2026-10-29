-- ============================================================================
--  pc/init_db.sql  ——  实验七 一键建库建表（新机器用）
--
--  用法（三种都行，选一种）：
--    ① 命令行：  mysql -u root -p < pc/init_db.sql
--    ② HeidiSQL：连上后开「查询」页签，粘贴本文件内容，按 F9 执行
--    ③ 任意 GUI：把本文件当脚本导入
--
--  说明：
--    * 实验七的数据表叫 **lab7_data**，在实验六 lab6_data 基础上：
--        - board 只允许 **1 / 2**（实验七是两块板）
--        - 新增 **ai_cls / ai_conf** 两列：板端 X-CUBE-AI 神经网络的判定结果
--          （题2：判定这条数据来自哪块板）
--    * 实验四的 th_data、实验五的 lab5_data、实验六的 lab6_data **都不动**
--      （各实验独立，互不影响）。
--    * 脚本幂等：库/表已存在时不会报错、不会清数据。
--    * 口径：board + seq 区分来源；sample_time 是"采集时间"（PC 按每板各自的
--      up_ms 标定换算），recv_time 是 PC 收到的时间。
--    * ai_cls：0/NULL = 未推理，1 = 判为板1，2 = 判为板2；ai_conf：0..100。
-- ============================================================================

CREATE DATABASE IF NOT EXISTS stm32_lab
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE stm32_lab;

CREATE TABLE IF NOT EXISTS lab7_data (
  id          BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT PRIMARY KEY,
  board       TINYINT UNSIGNED  NOT NULL COMMENT '1=板1，2=板2（实验七只有两块板）',
  seq         INT UNSIGNED      NULL COMMENT '该板采样序号',
  temp        DECIMAL(4,1)      NULL COMMENT '温度 ℃',
  humi        DECIMAL(4,1)      NULL COMMENT '湿度 %RH',
  light       SMALLINT UNSIGNED NULL COMMENT '光敏 AO 原始值 0..4095',
  eco2        INT UNSIGNED      NULL COMMENT 'SGP30 eCO2，ppm',
  tvoc        INT UNSIGNED      NULL COMMENT 'SGP30 TVOC，ppb（教材作"甲醛"指标）',
  status      VARCHAR(8)        NULL COMMENT 'OK / ALM',
  ai_cls      TINYINT UNSIGNED  NULL COMMENT 'AI 判定：0=未推理 1=板1 2=板2',
  ai_conf     TINYINT UNSIGNED  NULL COMMENT 'AI 置信度 0..100（%）',
  up_ms       INT UNSIGNED      NULL COMMENT '该板 up_ms（上电毫秒）',
  sample_time DATETIME(3)       NULL COMMENT '采集时间（PC 换算，毫秒）',
  recv_time   DATETIME(3)       NULL COMMENT 'PC 接收时间（毫秒）',
  KEY idx_board_sample (board, sample_time),
  KEY idx_ai (ai_cls, ai_conf)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='实验七 两板 温湿度+光敏+eCO2+TVOC + AI判定';

-- 自检：应看到 stm32_lab / lab7_data / 14 列
SELECT '建库完成' AS ok, DATABASE() AS db;
SHOW COLUMNS FROM lab7_data;
