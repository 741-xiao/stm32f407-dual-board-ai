/**
  ******************************************************************************
  * @file    network_data_params.c
  * @author  AST Embedded Analytics Research Platform
  * @date    2026-10-24T16:56:46+0800
  * @brief   AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */

#include "network_data_params.h"


/**  Activations Section  ****************************************************/
ai_handle g_network_activations_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(NULL),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};




/**  Weights Section  ********************************************************/
AI_ALIGNED(32)
const ai_u64 s_network_weights_array_u64[177] = {
  0x3eabfe0ebeea5b21U, 0x3f1c800abe593e29U, 0x3ec6165abe8c0148U, 0xbe2803113f06c2b7U,
  0xbf07a696bd112db5U, 0xbc8fdfadbe8c0b6eU, 0x3ec2a6723ef1628fU, 0xbd58706a3ef8ac0eU,
  0x3f49d668be547be6U, 0xbf4e58573eecca17U, 0x3edcd5d6be8ca675U, 0x3f2c2ee93dd6c359U,
  0xbdda847cbf1c02eaU, 0x3f014bde3d00589aU, 0xbf1566ac3e979442U, 0xbf0428e33edef2d8U,
  0x3eaf76253eab4266U, 0x3ea655113e572d28U, 0xbda6b1903e8bd8f6U, 0x3e713d70be11663dU,
  0x3e921b713eaa1d9bU, 0x3dfe00c93d185f93U, 0x3e14e8e43e931315U, 0x3e8f42873e6ca03fU,
  0xbf0d7236bd7e6c19U, 0x3e5370bc3e866806U, 0x3db1012a3d643d1bU, 0x3dff38583e89a4adU,
  0x3eec981cbddc045cU, 0x3d308a31be7cb9e9U, 0xbd481f7a3bec5930U, 0x3c61fbbb3e547ec7U,
  0x3f2504153e2fe605U, 0x3d3d70323eabfd82U, 0x3ebf21bb3bd18016U, 0xbee9fb223e931cafU,
  0x3e0f12d53e79156dU, 0xbdfa04193ef2c630U, 0x3e02ec9a3e337014U, 0xbe8f7f553cfbb7deU,
  0xbc09c8d33cc40165U, 0x40049c473e449117U, 0x3e8a329b3fb26eb3U, 0x3f95d683bd9521dfU,
  0x3fcfd727be6728e8U, 0x3e3ecac0bef5292cU, 0x3e853791bed6cd98U, 0x3fe7c7cf3ffc1cb0U,
  0xbe9ac0c9be107295U, 0x3f38adfabe7276e7U, 0xbef827133f474f5bU, 0x3e3b5437bc703208U,
  0x3ebaccffbce567f2U, 0x3e1130e2be83a1f8U, 0x3e8568403ebe918dU, 0xbd6bb7bf3e9668d5U,
  0x3d73355ebf0667b8U, 0x3ec6acd0bdc59676U, 0x3e4ca77e3f2e54f7U, 0x3f142cea3bae5d54U,
  0x3f02da62be1baca4U, 0xbe8e0b253e434d5cU, 0x3e757bf1be5b343dU, 0x3e5573983ed197f1U,
  0xbea11c63bdce331dU, 0x3df4d595bea63939U, 0x3e28298d3dd8a8c9U, 0x3f1ba598be1f1cefU,
  0x3e3ee0c6bec54c77U, 0xbe4e567e3a11b2d0U, 0x3e13e0633e0de81fU, 0x3f1dd47d3f03910eU,
  0x3f3b7b9e3e9851efU, 0x3cead09a3f0a45c2U, 0x3d5d050cbf0b86f6U, 0xbec369debde23babU,
  0x3d2f14553e943ab0U, 0x3d471b073f030a16U, 0x3f0f4c9b3ee1cf75U, 0xbcd7cd423dbaa09eU,
  0xbd81d4ce3e3a59b0U, 0x3f2ec9403e7e8905U, 0x3e1d13ed3daeed40U, 0x3e2220c1bf027000U,
  0x3ecc3fe9bf103fe7U, 0x3b218a53be98ab82U, 0xbd30aae93e52eddcU, 0x3d8d30d63f2e9dc0U,
  0x3d17ad7c3d61b109U, 0x3ef10781be242a42U, 0x3d5110083e879977U, 0x3f1d498abef9c86aU,
  0x3e1e054dbedf88ddU, 0xbd9c8385bdeaa716U, 0xbe8605743d69bf17U, 0x3f04a0193ee48bdaU,
  0xbebe984a3e315490U, 0x3e6d51c5bea24f34U, 0xbed5b4663e8ed33fU, 0x3db9c38a3ec185edU,
  0x3eb9b147be313ec8U, 0xbeb24090be99b3a2U, 0x3d3bce343ec0f10cU, 0x3eec1a38be0465ebU,
  0x3d8d9df63dbe1ed4U, 0xbe5e6fdabe95db96U, 0xbe3d54703cf86db6U, 0xbebdbd0fbe987957U,
  0x3dbc28ae3dbf0f0cU, 0xbea23a463ded2a1cU, 0xbd9898a4bf0a9f57U, 0x3d170f533e96ea1aU,
  0xbef1c3d6bef41ee1U, 0x3e8e41ed3ea78d5aU, 0xbf03f4643e87ed55U, 0xbe60371b3e83448aU,
  0xbd2ff6c0be1a8916U, 0xbd8a9ba43e820d8bU, 0xbe4188403cf5fc17U, 0xbebcbdabbebcca6eU,
  0x3ed5f3c83f97a6feU, 0xbdf50780bf2addd1U, 0xbeac22303eacb971U, 0x3d4a7fc03ea883b9U,
  0x3f138b11bf378beaU, 0xbf09bb88bef32fe9U, 0x3cdfe3493e00e337U, 0x3e08f430bc6db9a7U,
  0x3ea959053ed2cd23U, 0xbec2520e3f07806eU, 0x3da68356be86ea98U, 0xbe8d4388be5d6d2cU,
  0xbe267d703f15018bU, 0x3f2f7e503db6501eU, 0x3f8cb3243dfc0aa9U, 0x3dbcb4f43ccd74c0U,
  0x3f1d2a473f458a73U, 0xbedd97f03f5b656fU, 0x3f0203abbc7cd836U, 0x3ebf1d9bbde83909U,
  0xbeb88dec3f50b3e5U, 0x3f44b6b13e0330b4U, 0x3f5aa5a73f251beaU, 0xbe85f393bed6dc55U,
  0x3e9d1489be41aee7U, 0xbdacf5ecbea47082U, 0xbe117a203e1ff63aU, 0x3ec9617d3ecc4027U,
  0xbeb5f8a1bec40740U, 0x3b870080bd80b8c0U, 0xbc8309403ea2f973U, 0xbea829c53e878b7dU,
  0x3e1864cc3db879baU, 0x3e5a6114bd73b011U, 0x3e8041973d9e95f2U, 0x3e853d753e4070e9U,
  0x3e5691c63e39cdf8U, 0xbdac9f04be0a4c71U, 0x3ead2d2e3e806393U, 0x3ebc12edU,
  0x3f135c5ebeec0018U, 0xbf5eb8e5be59a895U, 0x3e1dcec6bf32de68U, 0x3e55e0d13f4b0121U,
  0xbf3e4f10bf2e5c36U, 0xbe9f2c9cbdc389cdU, 0x3e86b0e7be2fef54U, 0xbf10829c3f16eddaU,
  0x3b9e981ebe066aa6U, 0xbaac56023ede9e74U, 0x3f4ea9c93ebd682bU, 0x3f581558b9869ea0U,
  0x3f4a4e773f3de225U, 0xbd772d4c3d2db148U, 0xbf3952963f6e9ab1U, 0x3d1089b0be4fbc5dU,
  0xbe1cf1833e1cf10eU,
};


ai_handle g_network_weights_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(s_network_weights_array_u64),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};

